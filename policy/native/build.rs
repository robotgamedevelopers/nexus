use std::{env, fs, path::PathBuf};

fn main() -> std::io::Result<()> {
    println!("cargo:rerun-if-changed=../rules");
    let mut modules = Vec::new();
    for file in fs::read_dir("../rules")? {
        let path = file?.path();
        if path.extension().and_then(|s| s.to_str()) == Some("star") {
            modules.push(format!(
                "({:?}, include_str!({:?}))",
                path.file_name().unwrap(),
                path.canonicalize()?
            ));
        }
    }
    modules.sort();
    let output = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("rules.rs");
    fs::write(output, format!("&[{}]", modules.join(", ")))
}
