// Exposes the locked TUF library version for verification receipts.
fn main() {
    println!("cargo:rerun-if-changed=Cargo.lock");
    let lock = std::fs::read_to_string("Cargo.lock").expect("Cargo.lock");
    let mut lines = lock.lines();
    while let Some(line) = lines.next() {
        if line == "name = \"tough\"" {
            let version = lines
                .next()
                .and_then(|v| v.strip_prefix("version = \""))
                .and_then(|v| v.strip_suffix('"'))
                .expect("tough version");
            println!("cargo:rustc-env=FENN_TOUGH_VERSION={version}");
            return;
        }
    }
    panic!("tough is not locked");
}
