use anyhow::{Context, Result, bail, ensure};
use nexus_policy_runtime::{Bundle, LIMIT, Program, read};
use std::{
    collections::BTreeMap,
    env, fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
};

fn bundle(dir: &Path, side: &str) -> Result<Bundle> {
    let mut sources = BTreeMap::new();
    let mut bytes = 0;
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.extension().and_then(|s| s.to_str()) != Some("star") {
            continue;
        }
        let name = path
            .file_name()
            .unwrap()
            .to_str()
            .context("non-UTF-8 module name")?;
        // Each host receives only its own entry point and the shared modules.
        if name
            == format!(
                "{}.star",
                if side == "server" { "client" } else { "server" }
            )
        {
            continue;
        }
        let source = read(&path)?;
        bytes += source.len();
        ensure!(
            bytes <= LIMIT && sources.len() < 32,
            "source bundle too large"
        );
        sources.insert(name.to_owned(), String::from_utf8(source)?);
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
    fs::File::open(&dir)?.sync_all()?;
    println!("Applied {} policy to {}", side, path.display());
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("nexus-policy: {e:#}");
        std::process::exit(1);
    }
}
