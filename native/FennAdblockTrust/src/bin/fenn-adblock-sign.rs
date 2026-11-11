//! Offline root ceremony and online signing. Never linked into the browser helper.
use aws_lc_rs::signature::{Ed25519KeyPair, KeyPair, UnparsedPublicKey, ED25519};
use serde_json::{json, Value};
use std::{collections::BTreeSet, fs, io::Write, path::Path};
type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn hash(bytes: &[u8]) -> String {
    hex(aws_lc_rs::digest::digest(&aws_lc_rs::digest::SHA256, bytes).as_ref())
}
fn unhex(s: &str) -> Result<Vec<u8>> {
    if s.len() % 2 != 0 || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("invalid hex".into());
    }
    (0..s.len())
        .step_by(2)
        .map(|i| Ok(u8::from_str_radix(&s[i..i + 2], 16)?))
        .collect()
}
fn read(path: &Path, limit: u64) -> Result<Vec<u8>> {
    use std::io::Read;
    if !fs::symlink_metadata(path)?.is_file() {
        return Err("expected regular input file".into());
    }
    let mut bytes = Vec::new();
    fs::File::open(path)?
        .take(limit + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err("input bound".into());
    }
    Ok(bytes)
}
fn write_new(path: &Path, bytes: &[u8]) -> Result<()> {
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}
struct Key {
    pair: Ed25519KeyPair,
    public: Value,
    id: String,
}
impl Key {
    fn from(raw: &[u8]) -> Result<Self> {
        let pair = Ed25519KeyPair::from_pkcs8(raw).map_err(|_| "invalid signing key")?;
        let public = json!({"keytype":"ed25519","scheme":"ed25519","keyval":{"public":hex(pair.public_key().as_ref())}});
        let id = hash(&serde_json::to_vec(&public)?);
        Ok(Self { pair, public, id })
    }
    fn load(path: &Path) -> Result<Self> {
        let mut raw = read(path, 65536)?;
        let key = Self::from(&raw);
        raw.fill(0);
        key
    }
}
// Constrain authored metadata to the subset where sorted serde JSON is TUF
// canonical JSON. In particular do not author floating point or escaped text.
fn canonical(value: &Value) -> Result<Vec<u8>> {
    fn check(v: &Value) -> bool {
        match v {
            Value::String(s) => s
                .bytes()
                .all(|b| (0x20..=0x7e).contains(&b) && b != b'"' && b != b'\\'),
            Value::Array(a) => a.iter().all(check),
            Value::Object(m) => m.iter().all(|(k, v)| check(&json!(k)) && check(v)),
            Value::Number(n) => n.is_u64(),
            Value::Bool(_) => true,
            _ => false,
        }
    }
    if !check(value) {
        return Err("unsupported canonical metadata value".into());
    }
    Ok(serde_json::to_vec(value)?)
}
fn sign(value: Value, keys: &[Key]) -> Result<Value> {
    let raw = canonical(&value)?;
    Ok(
        json!({"signed":value,"signatures":keys.iter().map(|k|json!({"keyid":k.id,"sig":hex(k.pair.sign(&raw).as_ref())})).collect::<Vec<_>>()}),
    )
}
fn validate_role(root: &Value, role: &str, signed: &Value) -> Result<()> {
    let ids = root["roles"][role]["keyids"]
        .as_array()
        .ok_or("missing role keys")?;
    let threshold = root["roles"][role]["threshold"]
        .as_u64()
        .ok_or("missing threshold")?;
    if threshold == 0 || threshold > ids.len() as u64 {
        return Err("invalid threshold".into());
    }
    let raw = canonical(&signed["signed"])?;
    let mut valid = BTreeSet::new();
    for signature in signed["signatures"]
        .as_array()
        .ok_or("missing signatures")?
    {
        let id = signature["keyid"].as_str().ok_or("missing keyid")?;
        if !ids.contains(&json!(id)) {
            continue;
        }
        let public = &root["keys"][id];
        if public["keytype"] != "ed25519"
            || public["scheme"] != "ed25519"
            || hash(&canonical(public)?) != id
        {
            return Err("invalid public identity".into());
        }
        let key = unhex(
            public["keyval"]["public"]
                .as_str()
                .ok_or("missing public key")?,
        )?;
        let sig = unhex(signature["sig"].as_str().ok_or("missing signature")?)?;
        if UnparsedPublicKey::new(&ED25519, key)
            .verify(&raw, &sig)
            .is_ok()
        {
            valid.insert(id);
        }
    }
    if valid.len() < threshold as usize {
        return Err("signature threshold not met".into());
    }
    Ok(())
}
fn production_root(root: &Value) -> Result<()> {
    let payload = &root["signed"];
    if payload["_type"] != "root"
        || payload["spec_version"] != "1.0.0"
        || payload["consistent_snapshot"] != true
        || payload["version"].as_u64().unwrap_or(0) == 0
    {
        return Err("invalid production root".into());
    }
    let public_keys = payload["keys"].as_object().ok_or("missing public keys")?;
    let mut distinct_public = BTreeSet::new();
    if public_keys.len() != 4 {
        return Err("expected three offline keys and one online key".into());
    }
    for (id, key) in public_keys {
        let encoded = key["keyval"]["public"]
            .as_str()
            .ok_or("missing public key")?;
        if unhex(encoded)?.len() != 32
            || *key != json!({"keytype":"ed25519","scheme":"ed25519","keyval":{"public":encoded}})
            || hash(&canonical(key)?) != *id
            || !distinct_public.insert(encoded.to_ascii_lowercase())
        {
            return Err("invalid or duplicated production public key".into());
        }
    }
    let offline = payload["roles"]["root"]["keyids"]
        .as_array()
        .ok_or("missing root keys")?;
    let unique: BTreeSet<_> = offline.iter().filter_map(Value::as_str).collect();
    if unique.len() != 3 || offline.len() != 3 || payload["roles"]["root"]["threshold"] != 2 {
        return Err("production requires 2 of 3 offline root keys".into());
    }
    for role in ["targets", "snapshot", "timestamp"] {
        let ids = payload["roles"][role]["keyids"]
            .as_array()
            .ok_or("missing online key")?;
        if ids.len() != 1 || payload["roles"][role]["threshold"] != 1 || offline.contains(&ids[0]) {
            return Err("online and offline keys must be separate".into());
        }
    }
    validate_role(payload, "root", root)
}
fn field<'a>(v: &'a Value, key: &str) -> Result<&'a str> {
    v[key]
        .as_str()
        .ok_or_else(|| format!("missing {key}").into())
}
fn publish(request: &Value) -> Result<Value> {
    let root_bytes = read(Path::new(field(request, "root")?), 1024 * 1024)?;
    if hash(&root_bytes) != field(request, "root_sha256")? {
        return Err("root pin mismatch".into());
    }
    let root: Value = serde_json::from_slice(&root_bytes)?;
    production_root(&root)?;
    let key = Key::load(Path::new(field(request, "key_file")?))?;
    for role in ["targets", "snapshot", "timestamp"] {
        if root["signed"]["roles"][role]["keyids"] != json!([key.id]) {
            return Err("key is not the authorized online publisher".into());
        }
    }
    let keys = [key];
    let version = request["version"]
        .as_u64()
        .filter(|n| *n > 0)
        .ok_or("invalid version")?;
    let expires = field(request, "expires")?;
    let bundle = Path::new(field(request, "bundle")?);
    let output = Path::new(field(request, "output")?);
    let mut entries = serde_json::Map::new();
    let mut targets = Vec::new();
    for (name, limit) in [
        ("descriptor.json", 65536),
        ("rules.txt", 10 * 1024 * 1024),
        ("provenance.json", 1024 * 1024),
    ] {
        let bytes = read(&bundle.join(name), limit)?;
        let digest = hash(&bytes);
        entries.insert(
            name.into(),
            json!({"length":bytes.len(),"hashes":{"sha256":digest}}),
        );
        targets.push((format!("{digest}.{name}"), bytes));
    }
    let targets_metadata = serde_json::to_vec(&sign(
        json!({"_type":"targets","spec_version":"1.0.0","version":version,"expires":expires,"targets":entries}),
        &keys,
    )?)?;
    let meta = |bytes: &[u8]| json!({"version":version,"length":bytes.len(),"hashes":{"sha256":hash(bytes)}});
    let snapshot = serde_json::to_vec(&sign(
        json!({"_type":"snapshot","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"targets.json":meta(&targets_metadata)}}),
        &keys,
    )?)?;
    let timestamp = serde_json::to_vec(&sign(
        json!({"_type":"timestamp","spec_version":"1.0.0","version":version,"expires":expires,"meta":{"snapshot.json":meta(&snapshot)}}),
        &keys,
    )?)?;
    fs::create_dir(output)?;
    fs::create_dir(output.join("metadata"))?;
    fs::create_dir(output.join("targets"))?;
    for (name, bytes) in targets {
        write_new(&output.join("targets").join(name), &bytes)?;
    }
    let root_version = root["signed"]["version"].as_u64().unwrap();
    write_new(
        &output.join(format!("metadata/{root_version}.root.json")),
        &root_bytes,
    )?;
    write_new(
        &output.join(format!("metadata/{version}.targets.json")),
        &targets_metadata,
    )?;
    write_new(
        &output.join(format!("metadata/{version}.snapshot.json")),
        &snapshot,
    )?;
    write_new(&output.join("metadata/timestamp.json"), &timestamp)?;
    Ok(json!({"version":version,"root_sha256":hash(&root_bytes),"output":output}))
}
fn run(request: &Value) -> Result<Value> {
    match field(request, "op")? {
        "key" => {
            let path = Path::new(field(request, "output")?);
            let mut raw = Ed25519KeyPair::generate_pkcs8(&aws_lc_rs::rand::SystemRandom::new())
                .map_err(|_| "key generation failed")?
                .as_ref()
                .to_vec();
            let key = Key::from(&raw)?;
            let result = write_new(path, &raw);
            raw.fill(0);
            result?;
            let public = json!({"keyid":key.id,"key":key.public});
            write_new(
                &path.with_extension("public.json"),
                &serde_json::to_vec(&public)?,
            )?;
            Ok(public)
        }
        "root" => {
            let paths = request["key_files"]
                .as_array()
                .filter(|a| a.len() <= 6)
                .ok_or("root signers required")?;
            let keys = paths
                .iter()
                .map(|p| Key::load(Path::new(p.as_str().ok_or("invalid signer path")?)))
                .collect::<Result<Vec<_>>>()?;
            let root = sign(request["signed"].clone(), &keys)?;
            production_root(&root)?;
            let version = root["signed"]["version"].as_u64().unwrap();
            if version > 1 {
                let old: Value = serde_json::from_slice(&read(
                    Path::new(field(request, "previous_root")?),
                    1024 * 1024,
                )?)?;
                production_root(&old)?;
                if old["signed"]["version"]
                    .as_u64()
                    .and_then(|n| n.checked_add(1))
                    != Some(version)
                {
                    return Err("root rotation must be sequential".into());
                }
                validate_role(&old["signed"], "root", &root)?;
            }
            let bytes = serde_json::to_vec(&root)?;
            write_new(Path::new(field(request, "output")?), &bytes)?;
            Ok(json!({"root_sha256":hash(&bytes),"version":version}))
        }
        "publish" => publish(request),
        _ => Err("unknown operation".into()),
    }
}
fn main() {
    let result = (|| -> Result<Value> {
        let args = std::env::args_os().skip(1).collect::<Vec<_>>();
        if args.len() != 1 {
            return Err("usage: fenn-adblock-sign REQUEST.json".into());
        }
        run(&serde_json::from_slice(&read(
            Path::new(&args[0]),
            1024 * 1024,
        )?)?)
    })();
    match result {
        Ok(value) => println!("{value}"),
        Err(error) => {
            eprintln!("signing failed: {error}");
            std::process::exit(1);
        }
    }
}
