//! Both native hosts call the same ABI. Its only values are a bundled Starlark
//! module, an event name, and JSON. A module can load only sources in its bundle.
//! Built by cmake/policy.cmake, which runs cargo without the job's CC and CFLAGS.
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
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

pub const LIMIT: usize = 2 * 1024 * 1024;

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
        // A server without an applied fork profile keeps the behavior of an absent setting.
        // The shipped profile's config.json is applied explicitly alongside its modules.
        config: serde_json::json!({}),
    })
}

// What `nexus-policy apply <side> policy/rules` writes from this build's commit: the side's entry point and every
// module it loads, with the shipped config.json. An applied bundle that is anything else runs in place of the rules
// this binary packages, so the hosts say so when they start.
const PACKAGED_CONFIG: &str = include_str!("../../rules/config.json");

pub fn packaged(side: &str) -> Result<Bundle> {
    let files: &[(&str, &str)] = include!(concat!(env!("OUT_DIR"), "/rules.rs"));
    let all: BTreeMap<&str, &str> = files.iter().copied().collect();
    let mut sources = BTreeMap::new();
    let mut pending = vec![format!("{side}.star")];
    while let Some(name) = pending.pop() {
        if sources.contains_key(&name) {
            continue;
        }
        let text = all
            .get(name.as_str())
            .ok_or_else(|| anyhow!("the packaged rules lack {name}"))?;
        let ast = AstModule::parse(&name, text.to_string(), &Dialect::Standard)
            .map_err(|e| anyhow!("{e}"))?;
        pending.extend(ast.loads().into_iter().map(|l| l.module_id.to_owned()));
        sources.insert(name, text.to_string());
    }
    Ok(Bundle {
        version: 1,
        side: side.to_owned(),
        sources,
        config: serde_json::from_str(PACKAGED_CONFIG).context("reading the packaged config.json")?,
    })
}

// The first way an applied bundle differs from the packaged one, by content, or None when they are the same.
pub fn difference(applied: &[u8], side: &str) -> Result<Option<String>> {
    let expected = packaged(side)?;
    let applied: Bundle = match serde_json::from_slice(applied) {
        Ok(bundle) => bundle,
        Err(e) => return Ok(Some(format!("it is not a policy bundle ({e})"))),
    };
    if applied.version != expected.version || applied.side != expected.side {
        return Ok(Some(format!("it is not a version 1 {side} bundle")));
    }
    for (name, text) in &expected.sources {
        match applied.sources.get(name) {
            None => return Ok(Some(format!("it lacks {name}"))),
            Some(t) if t != text => return Ok(Some(format!("its {name} differs"))),
            Some(_) => {}
        }
    }
    if let Some(name) = applied.sources.keys().find(|n| !expected.sources.contains_key(*n)) {
        return Ok(Some(format!("it has {name}, which this build does not package")));
    }
    if applied.config != expected.config {
        return Ok(Some("its config differs from policy/rules/config.json".to_owned()));
    }
    Ok(None)
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
    let opposite = if bundle.side == "server" {
        "client"
    } else {
        "server"
    };
    ensure!(
        name != format!("{opposite}.star") && !name.starts_with(&format!("{opposite}/")),
        "{} cannot import {name}",
        bundle.side
    );
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
    application: FrozenModule,
}

impl Program {
    pub fn new(bundle: Bundle, side: &str) -> Result<Self> {
        ensure!(
            matches!(side, "client" | "server") && bundle.side == side && bundle.version == 1,
            "expected a version 1 {side} policy"
        );
        ensure!(
            bundle.config.is_object() && bundle.sources.len() <= 128,
            "expected a config object and at most 128 source files"
        );
        let module = compile(
            &format!("{side}.star"),
            &bundle,
            &mut HashMap::new(),
            &mut HashSet::new(),
        )?;
        let decider = module.get_option("decide")?;
        let handler = module.get_option("handle")?;
        ensure!(
            decider.is_some() || handler.is_some(),
            "export handle(event, state, config) or decide(event, context, config)"
        );
        for function in [&decider, &handler].into_iter().flatten() {
            ensure!(
                function.value().get_type() == "function",
                "application entry points must be functions"
            );
        }
        let fallback = if decider.is_none() || handler.is_none() {
            compile(
                &format!("{side}.star"),
                &defaults(side)?,
                &mut HashMap::new(),
                &mut HashSet::new(),
            )?
        } else {
            module.clone()
        };
        Ok(Self {
            application: if handler.is_some() {
                module.clone()
            } else {
                fallback.clone()
            },
            module: if decider.is_some() { module } else { fallback },
            config: bundle.config,
        })
    }

    pub fn decide(&self, event: &str, context: Value) -> Result<String> {
        let (entry, event) = match event.strip_prefix("app.") {
            Some(name) => ("handle", name),
            None => ("decide", event),
        };
        self.call(entry, event, context)
    }

    fn call(&self, entry: &str, event: &str, context: Value) -> Result<String> {
        ensure!(context.is_object(), "context must be a JSON object");
        Module::with_temp_heap(|module| {
            let source = if entry == "handle" {
                &self.application
            } else {
                &self.module
            };
            let function = module.heap().access_owned_frozen_value(&source.get(entry)?);
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
        if self.0.len() + buf.len() > 256 * 1024 {
            return Err(std::io::Error::other("answer exceeds 256 KiB"));
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
    watch: Option<Watch>,
    generation: u64,
}

struct Watch {
    stop: mpsc::Sender<()>,
    thread: Option<JoinHandle<()>>,
    pending: Arc<Mutex<Option<Program>>>,
}

impl Drop for Watch {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn read_revision(path: &str) -> Result<Option<Vec<u8>>> {
    Ok(if path.is_empty() {
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
    })
}

fn compile_revision(bytes: &Option<Vec<u8>>, side: &str) -> Result<Program> {
    compile_directory_revision(bytes, side, None)
}

fn compile_directory_revision(
    bytes: &Option<Vec<u8>>,
    side: &str,
    directory: Option<&[u8]>,
) -> Result<Program> {
    let mut bundle = match bytes {
        Some(bytes) => serde_json::from_slice(bytes).context("reading policy bundle")?,
        None => defaults(side)?,
    };
    if let Some(directory) = directory {
        let data: Value =
            serde_json::from_slice(directory).context("reading Nova friend directory")?;
        ensure!(data.is_object(), "expected a Nova friend directory object");
        bundle.config["nova_friends"] = data;
    }
    Program::new(bundle, side)
}

impl Host {
    fn load(&mut self, path: &str) -> Result<()> {
        let revision = read_revision(path)?;
        if self
            .active
            .as_ref()
            .is_none_or(|(bytes, _)| *bytes != revision)
        {
            let program = compile_revision(&revision, &self.side)?;
            self.active = Some((revision, program));
        }
        Ok(())
    }

    // Load the initial window application before its first frame. A worker owns all
    // subsequent filesystem access and compilation. Only frozen modules cross threads.
    fn watch(&mut self, path: String) -> Result<()> {
        let (stop, stopping) = mpsc::channel();
        let pending = Arc::new(Mutex::new(None));
        let publish = Arc::clone(&pending);
        self.load("")?;
        if let Err(e) = self.load(&path) {
            eprintln!("nexus: client policy: {e:#}");
        }
        let side = self.side.clone();
        let initial = self.active.as_ref().unwrap().0.clone();
        let directory_path = Path::new(&path).with_file_name("nova-friends.json");
        let directory_path = directory_path
            .to_str()
            .context("invalid directory path")?
            .to_owned();
        let mut initial_directory = None;
        let initial_load = (|| -> Result<()> {
            if let Some(data) = read_revision(&directory_path)? {
                let program = compile_directory_revision(&initial, &side, Some(&data))?;
                self.active = Some((initial.clone(), program));
                initial_directory = Some(data);
            }
            Ok(())
        })();
        if let Err(e) = initial_load {
            eprintln!("nexus: Nova friend directory: {e:#}");
        }
        let worker = thread::Builder::new()
            .name("nexus-policy".into())
            .spawn(move || {
                let mut previous_directory = initial_directory;
                let mut previous = initial;
                let mut last_error = None;
                while matches!(
                    stopping.recv_timeout(Duration::from_millis(250)),
                    Err(mpsc::RecvTimeoutError::Timeout)
                ) {
                    let result = (|| -> Result<()> {
                        let bytes = read_revision(&path)?;
                        let directory = read_revision(&directory_path)?;
                        if previous != bytes || previous_directory != directory {
                            let program =
                                compile_directory_revision(&bytes, &side, directory.as_deref())?;
                            previous_directory = directory;
                            previous = bytes;
                            *publish.lock().unwrap() = Some(program);
                        }
                        Ok(())
                    })();
                    let error = result.err().map(|e| format!("{e:#}"));
                    if error != last_error
                        && let Some(e) = &error
                    {
                        eprintln!("nexus: client policy: {e}");
                    }
                    last_error = error;
                }
            })?;
        self.watch = Some(Watch {
            stop,
            thread: Some(worker),
            pending,
        });
        Ok(())
    }

    fn refresh(&mut self) -> u64 {
        if let Some(watch) = &self.watch
            && let Ok(mut pending) = watch.pending.try_lock()
            && let Some(program) = pending.take()
        {
            self.active = Some((None, program));
            self.generation += 1;
        }
        self.generation
    }

    fn decide(&mut self, path: &str, event: &str, context: Value) -> Result<String> {
        if self.watch.is_none() {
            self.load(path)?;
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
            watch: None,
            generation: 0,
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
pub unsafe extern "C" fn nexus_star_watch_v1(p: *mut Host, path: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        (*p).watch(arg(path)?.to_owned())
    }))
    .unwrap_or_else(|_| Err(anyhow!("policy watcher panicked")));
    result
        .err()
        .map(|e| message(format!("{e:#}")))
        .unwrap_or(std::ptr::null_mut())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_refresh_v1(p: *mut Host) -> u64 {
    unsafe { (*p).refresh() }
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

// Null when no bundle is at `path` or it is the one this build packages; otherwise why it is not, one line.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_stale_v1(side: *const c_char, path: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| unsafe {
        let side = arg(side)?;
        ensure!(matches!(side, "client" | "server"), "unknown policy side");
        match read_revision(arg(path)?)? {
            None => Ok(None),
            Some(bytes) => difference(&bytes, side),
        }
    })
    .unwrap_or_else(|_| Err(anyhow!("policy comparison panicked")));
    match result {
        Ok(None) => std::ptr::null_mut(),
        Ok(Some(why)) => message(why),
        Err(e) => message(format!("it could not be compared: {e:#}")),
    }
}

// The bundle `nexus-policy apply <side> policy/rules` writes from this build's commit, as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nexus_star_packaged_v1(side: *const c_char, err: *mut *mut c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| unsafe {
        let side = arg(side)?;
        ensure!(matches!(side, "client" | "server"), "unknown policy side");
        Ok(serde_json::to_string(&packaged(side)?)?)
    })
    .unwrap_or_else(|_| Err(anyhow!("policy packaging panicked")));
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
