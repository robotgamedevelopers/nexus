//! Both native hosts call the same ABI. Its only values are a bundled Starlark
//! module, an event name, and JSON. A module can load only sources in its bundle.
use anyhow::{Context, Result, anyhow, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use starlark::environment::{FrozenModule, Globals, Module};
use starlark::eval::{Evaluator, ReturnFileLoader};
use starlark::syntax::{AstModule, Dialect};
use starlark::values::FrozenHeapName;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::ffi::{CStr, CString, c_char};
use std::io::{Read, Write};
use std::path::Path;

pub const LIMIT: usize = 256 * 1024;

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Bundle {
    pub version: u32,
    pub side: String,
    pub sources: BTreeMap<String, String>,
    pub config: Value,
}

fn defaults(side: &str) -> Result<Bundle> {
    let files: &[(&str, &str)] = include!(concat!(env!("OUT_DIR"), "/rules.rs"));
    let other = if side == "server" {
        "client.star"
    } else {
        "server.star"
    };
    Ok(Bundle {
        version: 1,
        side: side.to_owned(),
        sources: files
            .iter()
            .filter(|(name, _)| *name != other)
            .map(|(name, text)| (name.to_string(), text.to_string()))
            .collect(),
        config: serde_json::from_str(include_str!("../../rules/config.json"))?,
    })
}

pub fn read(path: &Path) -> Result<Vec<u8>> {
    ensure!(
        path.symlink_metadata()?.is_file(),
        "expected a regular file: {}",
        path.display()
    );
    let mut data = Vec::new();
    std::fs::File::open(path)?
        .take(LIMIT as u64 + 1)
        .read_to_end(&mut data)?;
    ensure!(data.len() <= LIMIT, "file exceeds {LIMIT} bytes");
    Ok(data)
}

fn bounded(eval: &mut Evaluator) -> Result<()> {
    eval.set_max_tick_count(50_000)?;
    eval.set_max_heap_size(2 * 1024 * 1024)?;
    Ok(())
}

fn compile(
    name: &str,
    bundle: &Bundle,
    done: &mut HashMap<String, FrozenModule>,
    visiting: &mut HashSet<String>,
) -> Result<FrozenModule> {
    if let Some(module) = done.get(name) {
        return Ok(module.clone());
    }
    ensure!(visiting.insert(name.to_owned()), "cyclic load of {name}");
    let source = bundle
        .sources
        .get(name)
        .ok_or_else(|| anyhow!("module {name} is absent from the bundle"))?;
    let ast =
        AstModule::parse(name, source.clone(), &Dialect::Standard).map_err(|e| anyhow!("{e}"))?;
    let mut imports = Vec::new();
    for load in ast.loads() {
        imports.push((
            load.module_id.to_owned(),
            compile(load.module_id, bundle, done, visiting)?,
        ));
    }
    let frozen = Module::with_temp_heap(|module| -> Result<_> {
        let loader = ReturnFileLoader {
            modules: &imports.iter().map(|(n, m)| (n.as_str(), m)).collect(),
        };
        {
            let mut eval = Evaluator::new(&module);
            bounded(&mut eval)?;
            eval.set_loader(&loader);
            eval.eval_module(ast, &Globals::standard())
                .map_err(|e| anyhow!("{e}"))?;
        }
        module
            .freeze_named(FrozenHeapName::User(Box::new(name.to_owned())))
            .map_err(|e| anyhow!("{e:?}"))
    })?;
    visiting.remove(name);
    done.insert(name.to_owned(), frozen.clone());
    Ok(frozen)
}

pub struct Program {
    module: FrozenModule,
    config: Value,
}

impl Program {
    pub fn new(bundle: Bundle, side: &str) -> Result<Self> {
        ensure!(
            matches!(side, "client" | "server") && bundle.side == side && bundle.version == 1,
            "expected a version 1 {side} policy"
        );
        ensure!(
            bundle.config.is_object() && bundle.sources.len() <= 32,
            "expected a config object and at most 32 source files"
        );
        let module = compile(
            &format!("{side}.star"),
            &bundle,
            &mut HashMap::new(),
            &mut HashSet::new(),
        )?;
        let function = module.get("decide")?;
        ensure!(
            function.value().get_type() == "function",
            "define decide(event, context, config)"
        );
        Ok(Self {
            module,
            config: bundle.config,
        })
    }

    pub fn decide(&self, event: &str, context: Value) -> Result<String> {
        ensure!(context.is_object(), "context must be a JSON object");
        Module::with_temp_heap(|module| {
            let function = module
                .heap()
                .access_owned_frozen_value(&self.module.get("decide")?);
            let heap = module.heap();
            let args = [
                heap.alloc(event),
                heap.alloc(context),
                heap.alloc(&self.config),
            ];
            let mut eval = Evaluator::new(&module);
            bounded(&mut eval)?;
            let answer = eval
                .eval_function(function, &args, &[])
                .map_err(|e| anyhow!("{e}"))?;
            let mut output = Vec::new();
            serde_json::to_writer(Limited(&mut output), &answer)?;
            Ok(String::from_utf8(output)?)
        })
    }
}

struct Limited<'a>(&'a mut Vec<u8>);
impl Write for Limited<'_> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        if self.0.len() + buf.len() > 65536 {
            return Err(std::io::Error::other("answer exceeds 64 KiB"));
        }
        self.0.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn message(s: String) -> *mut c_char {
    CString::new(s.replace('\0', " ")).unwrap().into_raw()
}

unsafe fn arg<'a>(s: *const c_char) -> Result<&'a str> {
    if s.is_null() {
        bail!("missing argument");
    }
    Ok(unsafe { CStr::from_ptr(s) }.to_str()?)
}

pub struct Host {
    side: String,
    active: Option<(Option<Vec<u8>>, Program)>,
}

impl Host {
    fn decide(&mut self, path: &str, event: &str, context: Value) -> Result<String> {
        let revision = if path.is_empty() {
            None
        } else {
            match read(Path::new(path)) {
                Ok(bytes) => Some(bytes),
                Err(e)
                    if e.downcast_ref::<std::io::Error>()
                        .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound) =>
                {
                    None
                }
                Err(e) => return Err(e.context(format!("reading {path}"))),
            }
        };
        if self
            .active
            .as_ref()
            .is_none_or(|(bytes, _)| *bytes != revision)
        {
            let bundle = match &revision {
                Some(bytes) => serde_json::from_slice(bytes).context("reading policy bundle")?,
                None => defaults(&self.side)?,
            };
            self.active = Some((revision, Program::new(bundle, &self.side)?));
        }
        self.active.as_ref().unwrap().1.decide(event, context)
    }
}

// C++ owns one host per process and calls it from its owner thread. The caller
// owns returned strings and releases them with nexus_star_free_v1.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_open_v1(
    side: *const c_char,
    err: *mut *mut c_char,
) -> *mut Host {
    let result = std::panic::catch_unwind(|| unsafe {
        let side = arg(side)?;
        ensure!(matches!(side, "client" | "server"), "unknown policy side");
        Ok(Host {
            side: side.to_string(),
            active: None,
        })
    })
    .unwrap_or_else(|_| Err(anyhow!("policy compiler panicked")));
    match result {
        Ok(p) => Box::into_raw(Box::new(p)),
        Err(e) => {
            unsafe { *err = message(format!("{e:#}")) };
            std::ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_call_v1(
    p: *mut Host,
    path: *const c_char,
    event: *const c_char,
    json: *const c_char,
    err: *mut *mut c_char,
) -> *mut c_char {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        (*p).decide(arg(path)?, arg(event)?, serde_json::from_str(arg(json)?)?)
    }))
    .unwrap_or_else(|_| Err(anyhow!("policy evaluation panicked")));
    match result {
        Ok(s) => message(s),
        Err(e) => {
            unsafe { *err = message(format!("{e:#}")) };
            std::ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_close_v1(p: *mut Host) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_free_v1(p: *mut c_char) {
    if !p.is_null() {
        drop(unsafe { CString::from_raw(p) });
    }
}
