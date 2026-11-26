use anyhow::{Context, Result, bail, ensure};
use nexus_policy_runtime::{Bundle, LIMIT, Program, read};
use std::{
    collections::BTreeMap,
    env,
    io::{self, Read, Write},
    path::{Path, PathBuf},
};

fn bundle(dir: &Path, side: &str) -> Result<Bundle> {
    let mut sources = BTreeMap::new();
    let root = dir
        .canonicalize()
        .with_context(|| format!("opening {}", dir.display()))?;
    let mut pending = vec![format!("{side}.star")];
    let mut total = 0;
    while let Some(name) = pending.pop() {
        if sources.contains_key(&name) {
            continue;
        }
        ensure!(
            name.ends_with(".star")
                && name.split('/').all(|part| !part.is_empty()
                    && part != "."
                    && part != ".."
                    && part
                        .bytes()
                        .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))),
            "invalid module name {name}"
        );
        let opposite = if side == "server" { "client" } else { "server" };
        ensure!(
            name != format!("{opposite}.star") && !name.starts_with(&format!("{opposite}/")),
            "{side} cannot import {name}"
        );
        let path = root.join(&name);
        ensure!(
            path.canonicalize()
                .with_context(|| format!("loading {name}"))?
                .starts_with(&root),
            "module {name} lies outside the rules directory"
        );
        let source = String::from_utf8(read(&path).with_context(|| format!("reading {name}"))?)?;
        total += source.len();
        ensure!(
            total <= LIMIT && sources.len() < 128,
            "application exceeds bundle limits"
        );
        let ast = starlark::syntax::AstModule::parse(
            &name,
            source.clone(),
            &starlark::syntax::Dialect::Standard,
        )
        .map_err(|e| anyhow::anyhow!("{e}"))?;
        pending.extend(
            ast.loads()
                .into_iter()
                .map(|load| load.module_id.to_owned()),
        );
        sources.insert(name, source);
    }
    Ok(Bundle {
        version: 1,
        side: side.to_owned(),
        sources,
        config: serde_json::from_slice(&read(&dir.join("config.json"))?)?,
    })
}

fn run() -> Result<()> {
    let args: Vec<String> = env::args().skip(1).collect();
    ensure!(
        (args.len() == 4 && args[0] == "preview")
            || ((args.len() == 3 || args.len() == 4) && args[0] == "apply"),
        "usage: nexus-policy preview client|server RULES EVENT <context.json; nexus-policy apply client|server RULES [HOST-DIR]"
    );
    let side = &args[1];
    ensure!(
        matches!(side.as_str(), "client" | "server"),
        "side must be client or server"
    );
    let b = bundle(Path::new(&args[2]), side)?;
    let bytes = serde_json::to_vec(&b)?;
    ensure!(bytes.len() <= LIMIT, "encoded bundle exceeds {LIMIT} bytes");
    let program = Program::new(b, side)?;
    if args[0] == "preview" {
        let mut buf = Vec::new();
        io::stdin().take(LIMIT as u64 + 1).read_to_end(&mut buf)?;
        ensure!(buf.len() <= LIMIT, "context too large");
        println!(
            "{}",
            program.decide(&args[3], serde_json::from_slice(&buf)?)?
        );
        return Ok(());
    }
    let dir = if let Some(arg) = args.get(3) {
        PathBuf::from(arg)
    } else {
        let home = env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
            .context("missing home directory")?;
        PathBuf::from(home).join(".nexus").join(side)
    };
    if !dir.is_dir() {
        bail!(
            "host directory {} does not exist; start Nexus or specify its directory",
            dir.display()
        );
    }
    let path = dir.join("policy.bundle.json");
    let mut tmp = tempfile::NamedTempFile::new_in(&dir)?;
    tmp.write_all(&bytes)?;
    tmp.as_file().sync_all()?;
    tmp.persist(&path)?;
    #[cfg(unix)]
    std::fs::File::open(&dir)?.sync_all()?;
    println!("Applied {} policy to {}", side, path.display());
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("nexus-policy: {e:#}");
        std::process::exit(1);
    }
}
