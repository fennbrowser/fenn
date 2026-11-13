//! Offline root ceremony and online signing. Never linked into the browser helper.
use aws_lc_rs::signature::{
    Ed25519KeyPair, KeyPair, UnparsedPublicKey, ED25519, RSA_PSS_2048_8192_SHA256,
};
use serde::Serialize;
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
// Use the same canonical formatter as tough. RSA PEM contains literal newlines
// in canonical JSON, unlike ordinary serde JSON's escaped newlines.
fn canonical(value: &Value) -> Result<Vec<u8>> {
    fn check(v: &Value) -> bool {
        match v {
            Value::String(s) => s.bytes().all(|b| (0x20..=0x7e).contains(&b) || b == b'\n'),
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
    let mut bytes = Vec::new();
    value.serialize(&mut serde_json::Serializer::with_formatter(
        &mut bytes,
        olpc_cjson::CanonicalFormatter::new(),
    ))?;
    Ok(bytes)
}
fn public_key(value: &Value) -> Result<tough::schema::key::Key> {
    let encoded = field(&value["keyval"], "public")?;
    let (kind, scheme) = match field(value, "keytype")? {
        "ed25519" if unhex(encoded)?.len() == 32 => ("ed25519", "ed25519"),
        "rsa" => ("rsa", "rsassa-pss-sha256"),
        _ => return Err("unsupported public key".into()),
    };
    if *value != json!({"keytype":kind,"scheme":scheme,"keyval":{"public":encoded}}) {
        return Err("invalid public key fields".into());
    }
    let key: tough::schema::key::Key = serde_json::from_value(value.clone())?;
    if let tough::schema::key::Key::Rsa { keyval, .. } = &key {
        let bits = aws_lc_rs::signature::RsaParameters::public_modulus_len(keyval.public.as_ref())
            .map_err(|_| "invalid RSA public key")?;
        if bits != 2048 {
            return Err("PIV root keys must be RSA-2048".into());
        }
    }
    // Reject alternate PEM/hex encodings that would change tough's key ID.
    if serde_json::to_value(&key)? != *value {
        return Err("public key must use normalized encoding".into());
    }
    Ok(key)
}
fn verify_signature(public: &Value, raw: &[u8], sig: &[u8]) -> Result<bool> {
    Ok(match public_key(public)? {
        tough::schema::key::Key::Ed25519 { keyval, .. } => {
            UnparsedPublicKey::new(&ED25519, keyval.public.as_ref())
                .verify(raw, sig)
                .is_ok()
        }
        tough::schema::key::Key::Rsa { keyval, .. } => {
            UnparsedPublicKey::new(&RSA_PSS_2048_8192_SHA256, keyval.public.as_ref())
                .verify(raw, sig)
                .is_ok()
        }
        _ => false,
    })
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
        if hash(&canonical(public)?) != id {
            return Err("invalid public identity".into());
        }
        let sig = unhex(signature["sig"].as_str().ok_or("missing signature")?)?;
        if verify_signature(public, &raw, &sig)? {
            valid.insert(id);
        }
    }
    if valid.len() < threshold as usize {
        return Err("signature threshold not met".into());
    }
    Ok(())
}
fn production_root(root: &Value) -> Result<()> {
    production_payload(&root["signed"])?;
    validate_role(&root["signed"], "root", root)
}
fn production_payload(payload: &Value) -> Result<()> {
    // Parse the runtime schema before requesting any hardware signature, including
    // its expiry representation and public-key identity checks.
    let _: tough::schema::Root = serde_json::from_value(payload.clone())?;
    if payload["_type"] != "root"
        || payload["spec_version"] != "1.0.0"
        || payload["consistent_snapshot"] != true
        || payload["version"].as_u64().unwrap_or(0) == 0
    {
        return Err("invalid production root".into());
    }
    let public_keys = payload["keys"].as_object().ok_or("missing public keys")?;
    let mut distinct_public = BTreeSet::new();
    for (id, key) in public_keys {
        let encoded = key["keyval"]["public"]
            .as_str()
            .ok_or("missing public key")?;
        public_key(key)?;
        if hash(&canonical(key)?) != *id || !distinct_public.insert(encoded.to_owned()) {
            return Err("invalid or duplicated production public key".into());
        }
    }
    let offline = payload["roles"]["root"]["keyids"]
        .as_array()
        .ok_or("missing root keys")?;
    let unique: BTreeSet<_> = offline.iter().filter_map(Value::as_str).collect();
    let legacy = offline.len() == 3 && payload["roles"]["root"]["threshold"] == 2;
    let hardware = (1..=2).contains(&offline.len())
        && payload["roles"]["root"]["threshold"] == 1
        && offline.iter().all(|id| {
            id.as_str()
                .is_some_and(|id| payload["keys"][id]["keytype"] == "rsa")
        });
    if !(legacy || hardware)
        || unique.len() != offline.len()
        || public_keys.len() != offline.len() + 1
        || unique.iter().any(|id| !public_keys.contains_key(*id))
        || payload["roles"].as_object().map(|v| v.len()) != Some(4)
    {
        return Err("production requires 2 of 3 root keys or 1 of 1/2 RSA root keys".into());
    }
    for role in ["targets", "snapshot", "timestamp"] {
        let ids = payload["roles"][role]["keyids"]
            .as_array()
            .ok_or("missing online key")?;
        if ids.len() != 1
            || payload["roles"][role]["threshold"] != 1
            || offline.contains(&ids[0])
            || ids
                != payload["roles"]["targets"]["keyids"]
                    .as_array()
                    .ok_or("missing targets key")?
            || ids[0]
                .as_str()
                .is_none_or(|id| payload["keys"][id]["keytype"] != "ed25519")
        {
            return Err("online and offline keys must be separate".into());
        }
    }
    Ok(())
}
fn previous_root(request: &Value) -> Result<Option<Value>> {
    production_payload(&request["signed"])?;
    let version = request["signed"]["version"]
        .as_u64()
        .ok_or("missing version")?;
    if version == 1 {
        if request.get("previous_root").is_some() {
            return Err("initial root cannot have a predecessor".into());
        }
        return Ok(None);
    }
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
    Ok(Some(old))
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
        "verify-proof" => {
            let nonce = field(request, "nonce")?;
            if nonce.len() != 64 || unhex(nonce)?.len() != 32 {
                return Err("invalid proof nonce".into());
            }
            let raw = format!("Fenn adblock hardware acceptance v1\n{nonce}");
            let signature = unhex(field(request, "signature")?)?;
            if !verify_signature(&request["key"], raw.as_bytes(), &signature)? {
                return Err("hardware proof signature rejected".into());
            }
            Ok(
                json!({"verified":true,"keyid":hash(&canonical(&request["key"])?),"payload_sha256":hash(raw.as_bytes())}),
            )
        }
        "public" => {
            let key: tough::schema::key::Key = field(request, "public_pem")?.parse()?;
            let public = serde_json::to_value(key)?;
            public_key(&public)?;
            Ok(json!({"keyid":hash(&canonical(&public)?),"key":public}))
        }
        "root-prepare" => {
            previous_root(request)?;
            let raw = canonical(&request["signed"])?;
            write_new(Path::new(field(request, "output")?), &raw)?;
            Ok(json!({"payload_sha256":hash(&raw),"version":request["signed"]["version"]}))
        }
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
            let previous = previous_root(request)?;
            let empty = json!([]);
            let paths = request
                .get("key_files")
                .unwrap_or(&empty)
                .as_array()
                .filter(|a| a.len() <= 6)
                .ok_or("root signers required")?;
            let keys = paths
                .iter()
                .map(|p| Key::load(Path::new(p.as_str().ok_or("invalid signer path")?)))
                .collect::<Result<Vec<_>>>()?;
            let mut root = sign(request["signed"].clone(), &keys)?;
            if let Some(signatures) = request.get("signatures") {
                let signatures = signatures
                    .as_array()
                    .filter(|a| a.len() <= 6)
                    .ok_or("too many detached signatures")?;
                root["signatures"]
                    .as_array_mut()
                    .unwrap()
                    .extend(signatures.iter().cloned());
            }
            production_root(&root)?;
            let version = root["signed"]["version"].as_u64().unwrap();
            if let Some(old) = previous {
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
