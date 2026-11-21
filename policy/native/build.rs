use std::{env, fs, path::PathBuf};

fn collect(
    root: &std::path::Path,
    dir: &std::path::Path,
    modules: &mut Vec<String>,
) -> std::io::Result<()> {
    for file in fs::read_dir(dir)? {
        let entry = file?;
        let path = entry.path();
        if entry.file_type()?.is_dir() {
            collect(root, &path, modules)?;
        } else if entry.file_type()?.is_file()
            && path.extension().and_then(|s| s.to_str()) == Some("star")
        {
            let name = path
                .strip_prefix(root)
                .unwrap()
                .to_str()
                .unwrap()
                .replace('\\', "/");
            modules.push(format!(
                "({name:?}, include_str!({:?}))",
                path.canonicalize()?
            ));
        }
    }
    Ok(())
}

fn main() -> std::io::Result<()> {
    println!("cargo:rerun-if-changed=../rules");
    let root = std::path::Path::new("../rules");
    let mut modules = Vec::new();
    collect(root, root, &mut modules)?;
    modules.sort();
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("rules.rs");
    fs::write(output, format!("&[{}]", modules.join(", ")))
}
