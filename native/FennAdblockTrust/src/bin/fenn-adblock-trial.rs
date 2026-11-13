//! Development (trial) feed signing with a DPAPI-sealed single key. Never
//! linked into or shipped with the browser; production uses fenn-adblock-sign.
type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
const META: u64 = 1024 * 1024;
const TARGETS: [(&str, u64); 3] = [
    ("descriptor.json", 65536),
    ("rules.txt", 10 * 1024 * 1024),
    ("provenance.json", META),
];
fn read(path: &Path, limit: u64) -> std::io::Result<Vec<u8>> {
    use std::io::Read;
    let meta = fs::symlink_metadata(path)?;
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if meta.file_attributes() & 0x400 != 0 {
            return Err(std::io::Error::other("reparse point rejected"));
        }
    }
    if !meta.is_file() {
        return Err(std::io::Error::other("expected regular input file"));
    }
    let mut bytes = Vec::new();
    fs::File::open(path)?
        .take(limit + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err(std::io::Error::other("input bound exceeded"));
    }
    Ok(bytes)
}
fn hex_hash(bytes: &[u8]) -> String {
    hex(aws_lc_rs::digest::digest(&aws_lc_rs::digest::SHA256, bytes).as_ref())
}
use aws_lc_rs::signature::{Ed25519KeyPair, KeyPair};
use serde_json::{json, Value};
use std::{ffi::c_void, fs, io::Write, path::Path, ptr};
#[repr(C)]
struct Blob {
    len: u32,
    data: *mut u8,
}
#[link(name = "crypt32")]
unsafe extern "system" {
    fn CryptProtectData(
        input: *const Blob,
        description: *const u16,
        entropy: *const Blob,
        reserved: *mut c_void,
        prompt: *mut c_void,
        flags: u32,
        output: *mut Blob,
    ) -> i32;
    fn CryptUnprotectData(
        input: *const Blob,
        description: *mut *mut u16,
        entropy: *const Blob,
        reserved: *mut c_void,
        prompt: *mut c_void,
        flags: u32,
        output: *mut Blob,
    ) -> i32;
}
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LocalFree(ptr: *mut c_void) -> *mut c_void;
}
fn protect(bytes: &[u8], decrypt: bool) -> Result<Vec<u8>> {
    let input = Blob {
        len: bytes.len().try_into()?,
        data: bytes.as_ptr() as *mut u8,
    };
    let mut output = Blob {
        len: 0,
        data: ptr::null_mut(),
    };
    let ok = unsafe {
        if decrypt {
            CryptUnprotectData(
                &input,
                ptr::null_mut(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
                1,
                &mut output,
            )
        } else {
            CryptProtectData(
                &input,
                ptr::null(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
                1,
                &mut output,
            )
        }
    };
    if ok == 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    let result = unsafe { std::slice::from_raw_parts(output.data, output.len as usize).to_vec() };
    unsafe {
        LocalFree(output.data.cast());
    }
    Ok(result)
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
struct Key {
    pair: Ed25519KeyPair,
    public: Value,
    id: String,
}
impl Key {
    fn from(bytes: &[u8]) -> Result<Self> {
        let pair = Ed25519KeyPair::from_pkcs8(bytes).map_err(|_| "invalid trial key")?;
        let public = json!({"keytype":"ed25519","scheme":"ed25519","keyval":{"public":hex(pair.public_key().as_ref())}});
        let id = hex_hash(&serde_json::to_vec(&public)?);
        Ok(Self { pair, public, id })
    }
    fn sign(&self, value: Value) -> Result<Vec<u8>> {
        // Only ASCII keys/values and integers are authored here: sorted compact
        // serde JSON is TUF canonical JSON for this constrained metadata.
        let bytes = serde_json::to_vec(&value)?;
        if !bytes.is_ascii() {
            return Err("metadata must be ASCII".into());
        }
        Ok(serde_json::to_vec(
            &json!({"signed":value,"signatures":[{"keyid":self.id,"sig":hex(self.pair.sign(&bytes).as_ref())}]}),
        )?)
    }
}
pub fn key(path: &Path, expires: &str) -> Result<Value> {
    let bytes = Ed25519KeyPair::generate_pkcs8(&aws_lc_rs::rand::SystemRandom::new())
        .map_err(|_| "key generation failed")?;
    let key = Key::from(bytes.as_ref())?;
    let role = json!({"keyids":[key.id],"threshold":1});
    let root=key.sign(json!({"_type":"root","spec_version":"1.0.0","version":1,"expires":expires,"consistent_snapshot":true,
        "keys":{key.id.clone():key.public},"roles":{"root":role,"targets":role,"snapshot":role,"timestamp":role}}))?;
    let sealed = protect(bytes.as_ref(), false)?;
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)?;
    file.write_all(&sealed)?;
    file.sync_all()?;
    let root_path = path.with_extension("root.json");
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&root_path)?;
    file.write_all(&root)?;
    file.sync_all()?;
    Ok(
        json!({"root":root_path,"keyid":key.id,"protection":"Windows DPAPI current user; trial only"}),
    )
}
pub fn publish(
    path: &Path,
    bundle: &Path,
    output: &Path,
    version: u64,
    expires: &str,
) -> Result<Value> {
    if version == 0 {
        return Err("version must be positive".into());
    }
    let mut raw = protect(&read(path, 65536)?, true)?;
    let parsed = Key::from(&raw);
    raw.fill(0);
    let key = parsed?;
    fs::create_dir(output)?;
    fs::create_dir(output.join("metadata"))?;
    fs::create_dir(output.join("targets"))?;
    let mut entries = serde_json::Map::new();
    for (name, limit) in TARGETS {
        let bytes = read(&bundle.join(name), limit)?;
        let digest = hex_hash(&bytes);
        fs::write(
            output.join("targets").join(format!("{digest}.{name}")),
            &bytes,
        )?;
        entries.insert(
            name.into(),
            json!({"length":bytes.len(),"hashes":{"sha256":digest}}),
        );
    }
    let targets=key.sign(json!({"_type":"targets","spec_version":"1.0.0","version":version,"expires":expires,"targets":entries}))?;
    let meta =
        |b: &[u8]| json!({"version":version,"length":b.len(),"hashes":{"sha256":hex_hash(b)}});
    let snapshot=key.sign(json!({"_type":"snapshot","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"targets.json":meta(&targets)}}))?;
    let timestamp=key.sign(json!({"_type":"timestamp","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"snapshot.json":meta(&snapshot)}}))?;
    let root = read(&path.with_extension("root.json"), META)?;
    fs::write(output.join("metadata/1.root.json"), &root)?;
    fs::write(
        output
            .join("metadata")
            .join(format!("{version}.targets.json")),
        targets,
    )?;
    fs::write(
        output
            .join("metadata")
            .join(format!("{version}.snapshot.json")),
        snapshot,
    )?;
    fs::write(output.join("metadata/timestamp.json"), timestamp)?;
    Ok(json!({"version":version,"expires":expires,"root_sha256":hex_hash(&root)}))
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let result = if args.len() == 3 && args[0] == "--trial-key" {
        key(Path::new(&args[1]), &args[2].to_string_lossy())
    } else if args.len() == 6 && args[0] == "--trial-publish" {
        args[4]
            .to_string_lossy()
            .parse::<u64>()
            .map_err(|e| Box::new(e) as Box<dyn std::error::Error>)
            .and_then(|v| {
                publish(
                    Path::new(&args[1]),
                    Path::new(&args[2]),
                    Path::new(&args[3]),
                    v,
                    &args[5].to_string_lossy(),
                )
            })
    } else {
        eprintln!("usage: fenn-adblock-trial --trial-key KEY EXPIRES | --trial-publish KEY BUNDLE OUTPUT VERSION EXPIRES");
        std::process::exit(2);
    };
    match result {
        Ok(value) => println!("{value}"),
        Err(error) => {
            eprintln!("trial publication failed: {error}");
            std::process::exit(1);
        }
    }
}
