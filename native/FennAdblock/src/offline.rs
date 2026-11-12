//! Offline development store. Hash integrity is NOT publisher authentication.
//! No transport, downloaded executable resources, or implicit production import.
use super::*;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

const CONTRACT: &str = "adblock-0.13.3/abi-2/offline-1";
const QUOTA: u64 = 256 * 1024 * 1024;

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Target {
    sha256: String,
    bytes: u64,
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Descriptor {
    schema: u32,
    sequence: u64,
    stream: String,
    engine_contract: String,
    resource_catalog_sha256: String,
    preprocessor_contract: String,
    rules: Target,
    provenance: Target,
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct State {
    generation: u64,
    active: Option<String>,
    previous: Option<String>,
    quarantined: Vec<String>,
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Watermark {
    sequence: u64,
    digest: String,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record<T> {
    schema: u32,
    payload: T,
    sha256: String,
}

pub struct Candidate {
    engine: Engine,
    stats: Value,
    root: PathBuf,
    digest: String,
    sequence: u64,
    generation: u64,
    handle: String,
    auth_expires: Option<u64>,
}
enum Outcome {
    Staged(Candidate),
    Committed(Candidate),
}
static WORK: Mutex<Option<JoinHandle<Result<Outcome, String>>>> = Mutex::new(None);
static CANDIDATE: Mutex<Option<Candidate>> = Mutex::new(None);
static CANCEL: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static SERIAL: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

// Exact legacy rules already rejected by Gecko. Preserve signed raw bundle hashes;
// this is not a general allowance for malformed cosmetics.
fn validate_published_rule(line: &str) -> Result<(), String> {
    use adblock::filters::cosmetic::CosmeticFilterError;
    use adblock::lists::FilterParseError;
    match adblock::lists::parse_filter(line, false, ParseOptions::default()) {
        Ok(_) => Ok(()),
        Err(FilterParseError::Cosmetic(
            CosmeticFilterError::InvalidCssSelector
            | CosmeticFilterError::ProceduralFilterWithMultipleSelectors,
        )) if include_str!("../../../configs/adblock/legacy-inert-cosmetics.txt")
            .lines()
            .any(|legacy| legacy == line) =>
        {
            Ok(())
        }
        Err(error) => Err(format!("rejected rule: {error}")),
    }
}

#[test]
fn legacy_css_allowance_is_exact_and_does_not_admit_other_invalid_rules() {
    for line in include_str!("../../../configs/adblock/legacy-inert-cosmetics.txt").lines() {
        assert!(validate_published_rule(line).is_ok());
        assert!(validate_published_rule(&format!("other.example,{line}")).is_err());
    }
    assert!(validate_published_rule("example.com##:-abp-properties(height: 300px;)").is_err());
    assert!(validate_published_rule("example.com##^script").is_err());
    assert!(validate_published_rule("||ads.example^").is_ok());
}

pub fn digest(bytes: &[u8]) -> Result<String, String> {
    #[cfg(windows)]
    {
        #[link(name = "bcrypt")]
        extern "system" {
            fn BCryptOpenAlgorithmProvider(
                h: *mut usize,
                name: *const u16,
                provider: *const u16,
                flags: u32,
            ) -> i32;
            fn BCryptHash(
                h: usize,
                secret: *const u8,
                secret_len: u32,
                data: *const u8,
                len: u32,
                out: *mut u8,
                out_len: u32,
            ) -> i32;
            fn BCryptCloseAlgorithmProvider(h: usize, flags: u32) -> i32;
        }
        let mut h = 0;
        let mut out = [0u8; 32];
        let name: Vec<u16> = "SHA256\0".encode_utf16().collect();
        let len = u32::try_from(bytes.len()).map_err(|_| "hash input too large")?;
        unsafe {
            if BCryptOpenAlgorithmProvider(&mut h, name.as_ptr(), std::ptr::null(), 0) < 0 {
                return Err("CNG open failed".into());
            }
            let status = BCryptHash(
                h,
                std::ptr::null(),
                0,
                bytes.as_ptr(),
                len,
                out.as_mut_ptr(),
                32,
            );
            BCryptCloseAlgorithmProvider(h, 0);
            if status < 0 {
                return Err("CNG hash failed".into());
            }
        }
        Ok(out.iter().map(|b| format!("{b:02x}")).collect())
    }
    #[cfg(not(windows))]
    {
        let _ = bytes;
        Err("offline store requires Windows CNG".into())
    }
}
fn valid_digest(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn regular(path: &Path) -> Result<(), String> {
    let m = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if m.file_attributes() & 0x400 != 0 {
            return Err("reparse point rejected".into());
        }
    }
    if m.file_type().is_symlink() {
        return Err("symlink rejected".into());
    }
    Ok(())
}
fn directory(path: &Path) -> Result<(), String> {
    if !path.exists() {
        fs::create_dir(path).map_err(|e| e.to_string())?;
    }
    regular(path)?;
    if !path.is_dir() {
        return Err("expected store directory".into());
    }
    Ok(())
}
fn read(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
    regular(path)?;
    if !path.is_file() {
        return Err("expected regular file".into());
    }
    bounded_read(path, limit)
}
fn root(profile: &Path) -> Result<PathBuf, String> {
    let profile = profile.canonicalize().map_err(|e| e.to_string())?;
    let root = profile.join("fenn-adblock-offline");
    directory(&root)?;
    directory(&root.join("bundles"))?;
    directory(&root.join("snapshots"))?;
    Ok(root)
}
fn usage(root: &Path) -> Result<u64, String> {
    let mut paths = vec![root.to_path_buf()];
    let mut size = 0u64;
    let mut count = 0;
    while let Some(p) = paths.pop() {
        regular(&p)?;
        count += 1;
        if count > 4096 {
            return Err("store entry bound exceeded".into());
        }
        if p.is_dir() {
            for e in fs::read_dir(&p).map_err(|e| e.to_string())? {
                paths.push(e.map_err(|e| e.to_string())?.path());
            }
        } else {
            size = size
                .checked_add(fs::metadata(&p).map_err(|e| e.to_string())?.len())
                .ok_or("store size overflow")?;
        }
    }
    Ok(size)
}
// Called only by the serialized update worker, never on startup. The browser
// profile lock supplies the cross-process single-writer boundary. Keep every
// generation usable by either recovery record, not merely the current pair.
fn reclaim(
    root: &Path,
    base: &Value,
    catalog: &str,
    incoming: &str,
    incoming_key: &str,
) -> Result<u64, String> {
    use std::collections::HashSet;
    regular(root)?;
    let mut keep = HashSet::new();
    let current = state(root)?;
    for id in current.active.iter().chain(current.previous.iter()) {
        keep.insert(id.clone());
    }
    let previous = root.join("state.previous.json");
    if previous.exists() {
        let old = validate_state(record::<State>(&previous)?)?;
        for id in old.active.iter().chain(old.previous.iter()) {
            keep.insert(id.clone());
        }
    }
    let mut snapshots = HashSet::from([incoming_key.to_owned()]);
    for id in &keep {
        let (_, bytes, actual) = bundle(&root.join("bundles").join(id), base, catalog)?;
        if actual != *id {
            return Err("retention bundle identity mismatch".into());
        }
        snapshots.insert(digest(&bytes)?);
    }
    keep.insert(incoming.to_owned());
    let mut files = Vec::new();
    let mut dirs = Vec::new();
    // Validate the complete bounded deletion plan before touching any file.
    // Unknown content is retained; there is deliberately no recursive delete.
    for area in ["bundles", "snapshots"] {
        let parent = root.join(area);
        regular(&parent)?;
        for path in crate::fetch_jobs::entries(&parent, 1024).map_err(|e| e.to_string())? {
            regular(&path)?;
            let name = path
                .file_name()
                .and_then(|s| s.to_str())
                .ok_or("retention filename")?;
            if area == "snapshots" {
                let key = name.strip_suffix(".cache.tmp").unwrap_or(name);
                if !valid_digest(key) || !path.is_file() {
                    return Err("unknown snapshot entry".into());
                }
                if !snapshots.contains(key) {
                    files.push(path);
                }
            } else {
                if !valid_digest(name) || !path.is_dir() {
                    return Err("unknown bundle entry".into());
                }
                if keep.contains(name) {
                    continue;
                }
                for file in crate::fetch_jobs::entries(&path, 6).map_err(|e| e.to_string())? {
                    regular(&file)?;
                    if !file.is_file()
                        || !matches!(
                            file.file_name().and_then(|s| s.to_str()),
                            Some(
                                "rules.txt"
                                    | "provenance.json"
                                    | "descriptor.json"
                                    | "rules.next"
                                    | "provenance.next"
                                    | "descriptor.next"
                            )
                        )
                    {
                        return Err("unknown bundle content".into());
                    }
                    files.push(file);
                }
                dirs.push(path);
            }
        }
    }
    let mut removed = 0;
    for path in files {
        regular(&path)?;
        let size = fs::metadata(&path).map_err(|e| e.to_string())?.len();
        fs::remove_file(&path).map_err(|e| e.to_string())?;
        removed += size;
    }
    for path in dirs {
        regular(&path)?;
        fs::remove_dir(&path).map_err(|e| e.to_string())?;
    }
    Ok(removed)
}
fn atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    regular(path.parent().ok_or("missing parent")?)?;
    if path.exists() {
        regular(path)?;
    }
    let temp = path.with_extension("next");
    if temp.exists() {
        regular(&temp)?;
        fs::remove_file(&temp).map_err(|e| e.to_string())?;
    }
    let mut f = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)
        .map_err(|e| e.to_string())?;
    f.write_all(bytes)
        .and_then(|_| f.sync_all())
        .map_err(|e| e.to_string())?;
    drop(f);
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        extern "system" {
            fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
        }
        let from: Vec<u16> = temp.as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 1 | 8) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
    }
    #[cfg(not(windows))]
    {
        fs::rename(&temp, path).map_err(|e| e.to_string())?;
    }
    Ok(())
}
fn encode<T: Serialize>(value: &T) -> Result<Vec<u8>, String> {
    let bytes = serde_json::to_vec(value).map_err(|e| e.to_string())?;
    serde_json::to_vec(&Record {
        schema: 1,
        payload: value,
        sha256: digest(&bytes)?,
    })
    .map_err(|e| e.to_string())
}
fn record<T: Serialize + for<'a> Deserialize<'a>>(path: &Path) -> Result<T, String> {
    let r: Record<T> = serde_json::from_slice(&read(path, 65536)?).map_err(|e| e.to_string())?;
    if r.schema != 1
        || digest(&serde_json::to_vec(&r.payload).map_err(|e| e.to_string())?)? != r.sha256
    {
        return Err("state checksum mismatch".into());
    }
    Ok(r.payload)
}
fn state(root: &Path) -> Result<State, String> {
    let p = root.join("state.json");
    let s: State = if p.exists() {
        record(&p)?
    } else {
        State::default()
    };
    validate_state(s)
}
fn validate_state(s: State) -> Result<State, String> {
    if s.quarantined.len() > 16
        || s.active
            .iter()
            .chain(s.previous.iter())
            .chain(s.quarantined.iter())
            .any(|x| !valid_digest(x))
    {
        return Err("invalid state identities".into());
    }
    Ok(s)
}
fn write_state(root: &Path, old: &State, new: &State) -> Result<(), String> {
    atomic(&root.join("state.previous.json"), &encode(old)?)?;
    atomic(&root.join("state.json"), &encode(new)?)
}
#[cfg(test)]
fn baseline(profile: &Path) -> Result<(Value, String), String> {
    baseline_assets(profile, None)
}
fn baseline_assets(profile: &Path, assets: Option<&Path>) -> Result<(Value, String), String> {
    if let Some(dir) = assets {
        regular(dir)?;
        let manifest: Value = serde_json::from_slice(&read(&dir.join("manifest.json"), 65536)?)
            .map_err(|e| e.to_string())?;
        if manifest["schema"] != 1
            || manifest["engine_contract"] != CONTRACT
            || manifest["development_only"] != true
        {
            return Err("invalid packaged asset manifest".into());
        }
        let bytes = read(&dir.join("baseline.json"), MAX_CONFIG)?;
        let resources = read(&dir.join("resources.json"), MAX_CONFIG)?;
        for (name, data) in [("baseline", &bytes), ("resources", &resources)] {
            if manifest[name]["bytes"].as_u64() != Some(data.len() as u64)
                || manifest[name]["sha256"].as_str() != Some(digest(data)?.as_str())
            {
                return Err("packaged asset hash mismatch".into());
            }
        }
        let v: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
        if v["version"] != 1
            || !v["resources"].is_array()
            || serde_json::to_vec(&v["resources"]).map_err(|e| e.to_string())? != resources
        {
            return Err("packaged catalog mismatch".into());
        }
        validate_rules(v["rules"].as_str().ok_or("missing packaged rules")?)?;
        return Ok((v, digest(&resources)?));
    }
    let v: Value = serde_json::from_slice(&read(
        &profile.join("fenn-adblock-prototype.json"),
        MAX_CONFIG,
    )?)
    .map_err(|e| e.to_string())?;
    if v["version"] != 1 || !v["resources"].is_array() {
        return Err("invalid offline baseline".into());
    }
    let catalog = digest(&serde_json::to_vec(&v["resources"]).map_err(|e| e.to_string())?)?;
    Ok((v, catalog))
}
fn bundle(
    dir: &Path,
    baseline: &Value,
    catalog: &str,
) -> Result<(Descriptor, Vec<u8>, String), String> {
    regular(dir)?;
    let descriptor = read(&dir.join("descriptor.json"), 65536)?;
    let d: Descriptor = serde_json::from_slice(&descriptor).map_err(|e| e.to_string())?;
    if d.schema != 1
        || d.sequence == 0
        || d.stream != "fenn-offline-v1"
        || d.engine_contract != CONTRACT
        || d.preprocessor_contract != "resolved-v1"
        || d.resource_catalog_sha256 != catalog
    {
        return Err("incompatible offline descriptor/catalog".into());
    }
    let rules = read(&dir.join("rules.txt"), MAX_CONFIG)?;
    let provenance = read(&dir.join("provenance.json"), 1024 * 1024)?;
    for (target, bytes) in [(&d.rules, &rules), (&d.provenance, &provenance)] {
        if target.bytes != bytes.len() as u64 || digest(bytes)? != target.sha256 {
            return Err("target length/hash mismatch".into());
        }
    }
    let _: Value = serde_json::from_slice(&provenance).map_err(|e| e.to_string())?;
    let rules = std::str::from_utf8(&rules).map_err(|e| e.to_string())?;
    validate_rules(rules)?;
    for line in rules
        .lines()
        .filter(|l| !l.trim().is_empty() && !l.starts_with('!') && !l.starts_with('['))
    {
        validate_published_rule(line)?;
    }
    let config =
        serde_json::to_vec(&json!({"version":1,"rules":rules,"resources":baseline["resources"]}))
            .map_err(|e| e.to_string())?;
    if config.len() > MAX_CONFIG {
        return Err("combined config too large".into());
    }
    Ok((d, config, digest(&descriptor)?))
}
fn prepare_bundle(root: &Path, id: &str, base: &Value, catalog: &str) -> Prepared {
    if !valid_digest(id) {
        return Err("invalid bundle identity".into());
    }
    let (_, bytes, actual) = bundle(&root.join("bundles").join(id), base, catalog)?;
    if actual != id {
        return Err("descriptor identity mismatch".into());
    }
    let key = digest(&bytes)?;
    let snapshot = root.join("snapshots").join(&key);
    if snapshot.exists() {
        regular(&snapshot)?;
    }
    let (engine, mut stats) = super::prepare_config(bytes, snapshot, false, Instant::now())?;
    stats["offlineDigest"] = json!(id);
    Ok((engine, stats))
}
#[cfg(test)]
pub fn startup(profile: &Path) -> Prepared {
    startup_assets(profile, None)
}
pub fn startup_assets(profile: &Path, assets: Option<&Path>) -> Prepared {
    let root = root(profile)?;
    let (base, catalog) = baseline_assets(profile, assets)?;
    let mut recovery = String::new();
    let initial = if !root.join("state.json").exists() && root.join("state.previous.json").exists()
    {
        Err("missing state record".into())
    } else {
        state(&root)
    };
    let mut s = match initial {
        Ok(s) => s,
        Err(_) => {
            recovery = "state-record".into();
            let old: State = record(&root.join("state.previous.json")).unwrap_or_default();
            atomic(&root.join("state.json"), &encode(&old)?)?;
            state(&root)?
        }
    };
    for id in [s.active.clone(), s.previous.clone()].into_iter().flatten() {
        if s.quarantined.contains(&id) {
            continue;
        }
        match prepare_bundle(&root, &id, &base, &catalog) {
            Ok((engine, mut stats)) => {
                if s.active.as_ref() != Some(&id) {
                    let old = s.clone();
                    s.generation = s.generation.checked_add(1).ok_or("generation overflow")?;
                    s.active = Some(id);
                    s.previous = None;
                    write_state(&root, &old, &s)?;
                }
                stats["offlineGeneration"] = json!(s.generation);
                stats["offlineRecovery"] = json!(recovery);
                stats["catalogSha256"] = json!(catalog);
                return Ok((engine, stats));
            }
            Err(e) => {
                recovery = e;
                if s.quarantined.len() >= 16 {
                    return Err("quarantine bound exceeded".into());
                }
                s.quarantined.push(id);
            }
        }
    }
    if s.active.is_some() {
        let old = state(&root)?;
        s.generation = s.generation.checked_add(1).ok_or("generation overflow")?;
        s.active = None;
        s.previous = None;
        write_state(&root, &old, &s)?;
    }
    let bytes = serde_json::to_vec(&base).map_err(|e| e.to_string())?;
    let (engine, mut stats) =
        super::prepare_config(bytes, root.join("baseline.cache"), false, Instant::now())?;
    stats["offlineDigest"] = Value::Null;
    stats["offlineGeneration"] = json!(s.generation);
    stats["offlineRecovery"] = json!(recovery);
    stats["catalogSha256"] = json!(catalog);
    Ok((engine, stats))
}
#[cfg(test)]
fn stage(profile: &Path, source: &Path, expected: u64) -> Result<Candidate, String> {
    stage_assets(profile, source, expected, None)
}
fn stage_assets(
    profile: &Path,
    source: &Path,
    expected: u64,
    assets: Option<&Path>,
) -> Result<Candidate, String> {
    let root = root(profile)?;
    let s = state(&root)?;
    if s.generation != expected {
        return Err("stale store generation".into());
    }
    let (base, catalog) = baseline_assets(profile, assets)?;
    let (d, bytes, id) = bundle(source, &base, &catalog)?;
    if s.quarantined.contains(&id) {
        return Err("candidate quarantined".into());
    }
    let watermark = root.join("watermark.json");
    if watermark.exists() {
        let high: Watermark = record(&watermark)?;
        if d.sequence < high.sequence || (d.sequence == high.sequence && id != high.digest) {
            return Err("sequence rollback/conflict".into());
        }
    } else if s.generation != 0
        || root.join("state.json").exists()
        || root.join("state.previous.json").exists()
    {
        return Err("missing high-water record".into());
    }
    // Cleanup failure must not invalidate the active engine. The existing hard
    // quota still rejects growth when reclaiming is unsafe or files are held.
    let reclaimed = reclaim(&root, &base, &catalog, &id, &digest(&bytes)?);
    if usage(&root)? + MAX_CACHE as u64 + bytes.len() as u64 + 2 * 1024 * 1024 > QUOTA {
        return Err("offline store quota exceeded".into());
    }
    let dir = root.join("bundles").join(&id);
    directory(&dir)?;
    for file in ["rules.txt", "provenance.json", "descriptor.json"] {
        let raw = read(&source.join(file), MAX_CONFIG)?;
        let dest = dir.join(file);
        if dest.exists() {
            if read(&dest, MAX_CONFIG)? != raw {
                return Err("immutable bundle conflict".into());
            }
        } else {
            atomic(&dest, &raw)?;
        }
    }
    let (engine, mut stats) = prepare_bundle(&root, &id, &base, &catalog)?;
    stats["storeReclaimedBytes"] = json!(reclaimed.as_ref().ok());
    stats["storeCleanupError"] = json!(reclaimed.err());
    stats["catalogSha256"] = json!(catalog);
    let handle = format!(
        "{}-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos(),
        SERIAL.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    );
    Ok(Candidate {
        engine,
        stats,
        root,
        digest: id,
        sequence: d.sequence,
        generation: expected,
        handle,
        auth_expires: None,
    })
}
fn fail(point: &str, wanted: &str) -> Result<(), String> {
    if point == wanted {
        Err(format!("injected failure: {point}"))
    } else {
        Ok(())
    }
}
fn validate_trust_assets(assets: &Path, helper: &Path) -> Result<(), String> {
    let manifest: Value = serde_json::from_slice(&read(&assets.join("manifest.json"), 65536)?)
        .map_err(|e| e.to_string())?;
    for (name, path, limit) in [
        ("root", assets.join("trusted-root.json"), 1024 * 1024),
        ("verifier", helper.to_path_buf(), 16 * 1024 * 1024),
    ] {
        let bytes = read(&path, limit)?;
        let target = &manifest["trust"][name];
        if target["bytes"].as_u64() != Some(bytes.len() as u64)
            || target["sha256"].as_str() != Some(digest(&bytes)?.as_str())
        {
            return Err("packaged trust asset mismatch".into());
        }
    }
    Ok(())
}
fn stage_authenticated(
    profile: &Path,
    source: &Path,
    expected: u64,
    assets: &Path,
    helper: &Path,
    broker: bool,
) -> Result<Candidate, String> {
    // Held through helper exit. Its independently opened handle protects the
    // directory if the browser is killed before reaping it.
    let _fetch_lease = if broker {
        Some(crate::fetch_jobs::begin(profile, source).map_err(|e| e.to_string())?)
    } else {
        None
    };

    use std::io::Read;
    use std::process::{Command, Stdio};
    regular(helper)?;
    validate_trust_assets(assets, helper)?;
    let root = root(profile)?;
    let scratch = crate::admission_jobs::begin(&root).map_err(|e| e.to_string())?;
    if usage(&root)? + 32 * 1024 * 1024 > QUOTA {
        return Err("authenticated scratch quota exceeded".into());
    }
    let output = scratch.path.join("verified");
    let mut command = Command::new(helper);
    command.arg(assets.join("trusted-root.json"));
    if broker {
        command.arg("--broker");
    }
    command.args([source.to_path_buf(), root.join("trust"), output.clone()]);
    command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    // Test crash hooks are for the standalone harness, never inherited by the browser.
    command.env_remove("FENN_ADBLOCK_TRUST_TEST_CRASH");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    let stdout = child.stdout.take().ok_or("missing verifier pipe")?;
    let (send, receive) = std::sync::mpsc::channel();
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        let result = stdout.take(65537).read_to_end(&mut bytes);
        let _ = send.send((result, bytes));
    });
    let deadline = Instant::now() + std::time::Duration::from_secs(if broker { 75 } else { 30 });
    let reply = loop {
        if CANCEL.load(std::sync::atomic::Ordering::Acquire) || Instant::now() >= deadline {
            break Err(std::sync::mpsc::RecvTimeoutError::Timeout);
        }
        match receive.recv_timeout(std::time::Duration::from_millis(20)) {
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => continue,
            reply => break reply,
        }
    };
    if !matches!(&reply,Ok((Ok(_),bytes)) if bytes.len()<=65536) {
        let _ = child.kill();
        let _ = child.wait();
        let _ = reader.join();
        return Err("verifier timed out or exceeded output bound".into());
    }
    let (_, bytes) = reply.unwrap();
    let _ = reader.join();
    if !child.wait().map_err(|e| e.to_string())?.success() {
        return Err("TUF verification rejected".into());
    }
    let receipt: Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    let expires = receipt["expires_unix"]
        .as_u64()
        .ok_or("missing admission expiry")?;
    for name in ["descriptor.json", "rules.txt", "provenance.json"] {
        if receipt["targets"][name].as_str()
            != Some(digest(&read(&output.join(name), MAX_CONFIG)?)?.as_str())
        {
            return Err("verified target changed".into());
        }
    }
    let mut c = stage_assets(profile, &output, expected, Some(assets))?;
    if receipt["targets"]["descriptor.json"].as_str() != Some(c.digest.as_str()) {
        return Err("candidate differs from verified descriptor".into());
    }
    c.auth_expires = Some(expires);
    c.stats["authenticated"] = json!(true);
    c.stats["trustRootVersion"] = receipt["root_version"].clone();
    Ok(c)
}
fn commit(mut c: Candidate, point: &str) -> Result<Candidate, String> {
    if let Some(expires) = c.auth_expires {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_secs();
        if now >= expires {
            return Err("authenticated admission expired before commit".into());
        }
    }
    let old = state(&c.root)?;
    if old.generation != c.generation {
        return Err("stale commit generation".into());
    }
    let watermark = c.root.join("watermark.json");
    if watermark.exists() {
        let high: Watermark = record(&watermark)?;
        if c.sequence < high.sequence || (c.sequence == high.sequence && c.digest != high.digest) {
            return Err("commit sequence rollback/conflict".into());
        }
    }
    if !watermark.exists()
        && (old.generation != 0
            || c.root.join("state.json").exists()
            || c.root.join("state.previous.json").exists())
    {
        return Err("missing commit high-water record".into());
    }
    if old.active.as_ref() == Some(&c.digest) {
        c.stats["offlineGeneration"] = json!(old.generation);
        c.stats["offlineRecovery"] = json!("");
        return Ok(c);
    }
    fail("before-watermark", point)?;
    atomic(
        &c.root.join("watermark.json"),
        &encode(&Watermark {
            sequence: c.sequence,
            digest: c.digest.clone(),
        })?,
    )?;
    fail("after-watermark", point)?;
    atomic(&c.root.join("state.previous.json"), &encode(&old)?)?;
    fail("after-previous", point)?;
    let next = State {
        generation: old.generation.checked_add(1).ok_or("generation overflow")?,
        active: Some(c.digest.clone()),
        previous: old.active,
        quarantined: old.quarantined,
    };
    atomic(&c.root.join("state.json"), &encode(&next)?)?;
    fail("after-state", point)?;
    c.stats["offlineGeneration"] = json!(next.generation);
    c.stats["offlineRecovery"] = json!("");
    Ok(c)
}
pub fn busy() -> bool {
    WORK.lock().map(|w| w.is_some()).unwrap_or(true)
}
pub fn dispatch(v: &Value) -> Result<(Option<Engine>, Value), String> {
    match field(v, "op")? {
        "offline-cancel" => {
            discard();
            Ok((None, json!({})))
        }
        "offline-stage" | "offline-authenticated-stage" => {
            let mut work = WORK.lock().map_err(|_| "store job poisoned")?;
            if work.is_some() {
                return Err("store job busy".into());
            }
            CANDIDATE.lock().map_err(|_| "candidate poisoned")?.take();
            let profile = PathBuf::from(field(v, "profile")?);
            let source = PathBuf::from(field(v, "source")?);
            let generation = v["generation"].as_u64().ok_or("missing generation")?;
            let assets = v["assets"].as_str().map(PathBuf::from);
            CANCEL.store(false, std::sync::atomic::Ordering::Release);
            let broker = v["broker"].as_bool().unwrap_or(false);
            let authenticated = field(v, "op")? == "offline-authenticated-stage";
            let helper = v["helper"].as_str().map(PathBuf::from);
            let delay = v["verifyDelayMs"].as_u64().unwrap_or(0).min(2000);
            *work = Some(thread::spawn(move || {
                if authenticated {
                    thread::sleep(std::time::Duration::from_millis(delay));
                    stage_authenticated(
                        &profile,
                        &source,
                        generation,
                        assets.as_deref().ok_or("missing packaged assets")?,
                        helper.as_deref().ok_or("missing installed verifier")?,
                        broker,
                    )
                    .map(Outcome::Staged)
                } else {
                    stage_assets(&profile, &source, generation, assets.as_deref())
                        .map(Outcome::Staged)
                }
            }));
            Ok((None, json!({})))
        }
        "offline-commit" => {
            let mut work = WORK.lock().map_err(|_| "store job poisoned")?;
            if work.is_some() {
                return Err("store job busy".into());
            }
            let mut slot = CANDIDATE.lock().map_err(|_| "candidate poisoned")?;
            if slot.as_ref().map(|c| c.handle.as_str()) != Some(field(v, "handle")?) {
                return Err("stale candidate handle".into());
            }
            let c = slot.take().unwrap();
            let point = v["failpoint"].as_str().unwrap_or("").to_owned();
            let delay = v["delayMs"].as_u64().unwrap_or(0).min(2000);
            *work = Some(thread::spawn(move || {
                thread::sleep(std::time::Duration::from_millis(delay));
                commit(c, &point).map(Outcome::Committed)
            }));
            Ok((None, json!({})))
        }
        "offline-poll" => {
            let mut work = WORK.lock().map_err(|_| "store job poisoned")?;
            let Some(job) = work.as_ref() else {
                return Err("no store job".into());
            };
            if !job.is_finished() {
                return Ok((None, json!({"done":false})));
            }
            let outcome = work
                .take()
                .unwrap()
                .join()
                .map_err(|_| "store worker panicked")??;
            let accept = v["accept"].as_bool().unwrap_or(false);
            match outcome {
                Outcome::Staged(c) => {
                    let reply = json!({"done":true,"staged":accept,"handle":if accept{Some(c.handle.clone())}else{None},"stats":c.stats});
                    if accept {
                        *CANDIDATE.lock().map_err(|_| "candidate poisoned")? = Some(c);
                    }
                    Ok((None, reply))
                }
                Outcome::Committed(c) => Ok((
                    if accept { Some(c.engine) } else { None },
                    json!({"done":true,"accepted":accept,"committed":true,"stats":c.stats}),
                )),
            }
        }
        _ => Err("unknown offline operation".into()),
    }
}
pub fn discard() {
    CANCEL.store(true, std::sync::atomic::Ordering::Release);
    if let Ok(mut c) = CANDIDATE.lock() {
        c.take();
    }
}
pub fn shutdown() {
    CANCEL.store(true, std::sync::atomic::Ordering::Release);
    if let Ok(mut w) = WORK.lock() {
        if let Some(job) = w.take() {
            let _ = job.join();
        }
    }
    discard();
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Profile(PathBuf);
    impl Profile {
        fn new() -> Self {
            let p = std::env::temp_dir().join(format!(
                "fenn-offline-test-{}-{}",
                std::process::id(),
                SERIAL.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            fs::create_dir(&p).unwrap();
            fs::write(
                p.join("fenn-adblock-prototype.json"),
                br#"{"version":1,"rules":"||baseline.example^","resources":[]}"#,
            )
            .unwrap();
            Self(p)
        }
        fn bundle(&self, sequence: u64, rules: &str) -> PathBuf {
            let p = self.0.join(format!(
                "input-{}",
                SERIAL.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            fs::create_dir(&p).unwrap();
            let (_, catalog) = baseline(&self.0).unwrap();
            let provenance = b"{\"source\":\"local-test\"}";
            let d = Descriptor {
                schema: 1,
                sequence,
                stream: "fenn-offline-v1".into(),
                engine_contract: CONTRACT.into(),
                resource_catalog_sha256: catalog,
                preprocessor_contract: "resolved-v1".into(),
                rules: Target {
                    sha256: digest(rules.as_bytes()).unwrap(),
                    bytes: rules.len() as u64,
                },
                provenance: Target {
                    sha256: digest(provenance).unwrap(),
                    bytes: provenance.len() as u64,
                },
            };
            fs::write(p.join("rules.txt"), rules).unwrap();
            fs::write(p.join("provenance.json"), provenance).unwrap();
            fs::write(p.join("descriptor.json"), serde_json::to_vec(&d).unwrap()).unwrap();
            p
        }
    }
    impl Drop for Profile {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    #[test]
    fn packaged_assets_reject_corruption_without_profile_fallback() {
        let p = Profile::new();
        let dir = p.0.join("assets");
        fs::create_dir(&dir).unwrap();
        let bytes = br#"{"version":1,"rules":"||packaged.example^","resources":[]}"#;
        let resources = b"[]";
        let target = |b: &[u8]| json!({"bytes":b.len(),"sha256":digest(b).unwrap()});
        let manifest = json!({"schema":1,"development_only":true,"engine_contract":CONTRACT,
            "baseline":target(bytes),"resources":target(resources)});
        fs::write(
            dir.join("manifest.json"),
            serde_json::to_vec(&manifest).unwrap(),
        )
        .unwrap();
        fs::write(dir.join("baseline.json"), bytes).unwrap();
        fs::write(dir.join("resources.json"), resources).unwrap();
        assert_eq!(
            baseline_assets(&p.0, Some(&dir)).unwrap().0["rules"],
            "||packaged.example^"
        );
        assert!(startup_assets(&p.0, Some(&dir)).is_ok());
        fs::write(dir.join("resources.json"), b"[{}]").unwrap();
        assert!(startup_assets(&p.0, Some(&dir)).is_err());
        fs::write(dir.join("resources.json"), resources).unwrap();
        fs::write(dir.join("baseline.json"), b"corrupt").unwrap();
        assert!(startup_assets(&p.0, Some(&dir)).is_err());
        assert!(startup_assets(&p.0, Some(&p.0.join("missing"))).is_err());
    }
    #[test]
    fn authenticated_candidate_expiry_cannot_commit() {
        let p = Profile::new();
        let mut candidate = stage(&p.0, &p.bundle(1, "||ads.example^"), 0).unwrap();
        candidate.auth_expires = Some(0);
        assert!(commit(candidate, "").err().unwrap().contains("expired"));
        assert!(!root(&p.0).unwrap().join("state.json").exists());
    }
    #[test]
    fn packaged_root_and_helper_identities_are_required() {
        let p = Profile::new();
        let helper = p.0.join("helper.exe");
        fs::write(&helper, b"helper").unwrap();
        fs::write(p.0.join("trusted-root.json"), b"root").unwrap();
        let target = |bytes: &[u8]| json!({"bytes":bytes.len(),"sha256":digest(bytes).unwrap()});
        fs::write(
            p.0.join("manifest.json"),
            serde_json::to_vec(
                &json!({"trust":{"root":target(b"root"),"verifier":target(b"helper")}}),
            )
            .unwrap(),
        )
        .unwrap();
        assert!(validate_trust_assets(&p.0, &helper).is_ok());
        fs::write(&helper, b"changed").unwrap();
        assert!(validate_trust_assets(&p.0, &helper).is_err());
        fs::write(&helper, b"helper").unwrap();
        fs::write(p.0.join("trusted-root.json"), b"changed").unwrap();
        assert!(validate_trust_assets(&p.0, &helper).is_err());
    }
    #[test]
    fn hashing_and_strict_candidate_validation() {
        assert_eq!(
            digest(b"abc").unwrap(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        let p = Profile::new();
        let input = p.bundle(1, "||ads.example^");
        fs::write(input.join("rules.txt"), "modified").unwrap();
        assert!(stage(&p.0, &input, 0).is_err());
        let input = p.bundle(1, "!#include unknown.txt");
        assert!(stage(&p.0, &input, 0).is_err());
        let input = p.bundle(1, "||ads.example^$unknown-option");
        assert!(stage(&p.0, &input, 0).is_err());
        let input = p.bundle(1, "||ads.example^");
        let mut d: Value =
            serde_json::from_slice(&fs::read(input.join("descriptor.json")).unwrap()).unwrap();
        d["resource_catalog_sha256"] = json!("incorrect");
        fs::write(input.join("descriptor.json"), d.to_string()).unwrap();
        assert!(stage(&p.0, &input, 0).is_err());
        assert!(stage(&p.0, &p.bundle(1, "||ads.example^"), 99).is_err());
    }
    #[test]
    fn commit_failpoints_preserve_restart_authority_and_watermark() {
        for point in [
            "before-watermark",
            "after-watermark",
            "after-previous",
            "after-state",
        ] {
            let p = Profile::new();
            let a = commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
            let first = a.digest;
            let b = stage(&p.0, &p.bundle(2, "||two.example^"), 1).unwrap();
            let second = b.digest.clone();
            assert!(commit(b, point).is_err());
            let (_, stats) = startup(&p.0).unwrap();
            assert_eq!(
                stats["offlineDigest"],
                json!(if point == "after-state" {
                    second
                } else {
                    first
                })
            );
            let high: Watermark = record(&root(&p.0).unwrap().join("watermark.json")).unwrap();
            assert_eq!(
                high.sequence,
                if point == "before-watermark" { 1 } else { 2 }
            );
        }
    }
    #[test]
    fn corrupt_snapshot_candidate_and_state_recover_without_sequence_rollback() {
        let p = Profile::new();
        let a = commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
        let first = a.digest;
        let b = commit(stage(&p.0, &p.bundle(2, "||two.example^"), 1).unwrap(), "").unwrap();
        let second = b.digest;
        let r = root(&p.0).unwrap();
        let before = read(&r.join("state.json"), 65536).unwrap();
        let same = stage(&p.0, &r.join("bundles").join(&second), 2).unwrap();
        commit(same, "").unwrap();
        assert_eq!(read(&r.join("state.json"), 65536).unwrap(), before);
        for entry in fs::read_dir(r.join("snapshots")).unwrap() {
            fs::write(entry.unwrap().path(), b"broken").unwrap();
        }
        let (_, stats) = startup(&p.0).unwrap();
        assert_eq!(stats["offlineDigest"], json!(second));
        assert_eq!(stats["cacheHit"], false);
        fs::write(r.join("bundles").join(&second).join("rules.txt"), b"broken").unwrap();
        let (_, stats) = startup(&p.0).unwrap();
        assert_eq!(stats["offlineDigest"], json!(first));
        assert!(state(&r).unwrap().quarantined.contains(&second));
        let generation = state(&r).unwrap().generation;
        assert!(stage(&p.0, &p.bundle(1, "||one.example^"), generation).is_err());
        fs::write(r.join("state.json"), b"truncated").unwrap();
        let (_, stats) = startup(&p.0).unwrap();
        assert_eq!(stats["offlineDigest"], json!(first));
        fs::write(r.join("state.json"), b"truncated").unwrap();
        fs::write(r.join("state.previous.json"), b"truncated").unwrap();
        let (_, stats) = startup(&p.0).unwrap();
        assert!(stats["offlineDigest"].is_null());
        let high: Watermark = record(&r.join("watermark.json")).unwrap();
        assert_eq!(high.sequence, 2);
        let generation = state(&r).unwrap().generation;
        let candidate = stage(&p.0, &p.bundle(3, "||three.example^"), generation).unwrap();
        fs::remove_file(r.join("watermark.json")).unwrap();
        assert!(commit(candidate, "").is_err());
        assert!(stage(&p.0, &p.bundle(1, "||one.example^"), generation).is_err());
    }
    #[test]
    fn retention_bounds_updates_and_preserves_both_recovery_records() {
        let p = Profile::new();
        let r = root(&p.0).unwrap();
        let mut ids = Vec::new();
        for sequence in 1..=12 {
            let input = p.bundle(sequence, &format!("||ads{sequence}.example^"));
            let candidate = stage(&p.0, &input, sequence - 1).unwrap();
            ids.push(candidate.digest.clone());
            commit(candidate, "").unwrap();
            assert!(fs::read_dir(r.join("bundles")).unwrap().count() <= 4);
            assert!(fs::read_dir(r.join("snapshots")).unwrap().count() <= 4);
        }
        assert!(!r.join("bundles").join(&ids[0]).exists());
        // A damaged current record must still recover the older pair; then a
        // damaged recovered active bundle must still fall back one more time.
        fs::write(r.join("state.json"), b"torn").unwrap();
        assert_eq!(startup(&p.0).unwrap().1["offlineDigest"], ids[10]);
        fs::write(r.join("bundles").join(&ids[10]).join("rules.txt"), b"torn").unwrap();
        assert_eq!(startup(&p.0).unwrap().1["offlineDigest"], ids[9]);
        let generation = state(&r).unwrap().generation;
        assert!(stage(&p.0, &p.bundle(1, "||ads1.example^"), generation).is_err());
        assert_eq!(
            record::<Watermark>(&r.join("watermark.json"))
                .unwrap()
                .sequence,
            12
        );
    }

    #[test]
    fn retention_reclaims_abandoned_work_before_quota_and_preserves_authority() {
        let p = Profile::new();
        let r = root(&p.0).unwrap();
        commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
        let state_bytes = fs::read(r.join("state.json")).unwrap();
        let high_bytes = fs::read(r.join("watermark.json")).unwrap();
        let abandoned = stage(&p.0, &p.bundle(2, "||abandoned.example^"), 1).unwrap();
        let orphan = r.join("snapshots").join("a".repeat(64));
        File::create(&orphan).unwrap().set_len(QUOTA).unwrap();
        let candidate = stage(&p.0, &p.bundle(3, "||three.example^"), 1).unwrap();
        assert!(candidate.stats["storeReclaimedBytes"].as_u64().unwrap() >= QUOTA);
        assert!(!r.join("bundles").join(abandoned.digest).exists());
        assert!(!orphan.exists());
        assert_eq!(fs::read(r.join("state.json")).unwrap(), state_bytes);
        assert_eq!(fs::read(r.join("watermark.json")).unwrap(), high_bytes);
        commit(candidate, "").unwrap();
        assert_eq!(startup(&p.0).unwrap().1["cacheHit"], true);
    }

    #[test]
    fn retention_unknown_content_and_corrupt_recovery_skip_all_deletion() {
        let p = Profile::new();
        let r = root(&p.0).unwrap();
        commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
        let abandoned = stage(&p.0, &p.bundle(2, "||abandoned.example^"), 1).unwrap();
        let unknown = r
            .join("bundles")
            .join(&abandoned.digest)
            .join("owner-notes");
        fs::write(&unknown, b"keep").unwrap();
        let candidate = stage(&p.0, &p.bundle(3, "||three.example^"), 1).unwrap();
        assert!(candidate.stats["storeCleanupError"].is_string());
        assert_eq!(fs::read(&unknown).unwrap(), b"keep");
        assert!(r
            .join("bundles")
            .join(&abandoned.digest)
            .join("rules.txt")
            .exists());
        fs::remove_file(unknown).unwrap();
        fs::write(r.join("state.previous.json"), b"torn").unwrap();
        let candidate = stage(&p.0, &p.bundle(4, "||four.example^"), 1).unwrap();
        assert!(candidate.stats["storeCleanupError"].is_string());
        assert!(r.join("bundles").join(&abandoned.digest).exists());
        assert!(startup(&p.0).is_ok());
    }

    #[cfg(windows)]
    #[test]
    fn retention_rejects_junction_without_touching_external_content() {
        let p = Profile::new();
        let external = Profile::new();
        fs::write(external.0.join("rules.txt"), b"external sentinel").unwrap();
        let r = root(&p.0).unwrap();
        let link = r.join("bundles").join("b".repeat(64));
        assert!(std::process::Command::new("cmd.exe")
            .args(["/C", "mklink", "/J"])
            .arg(&link)
            .arg(&external.0)
            .output()
            .unwrap()
            .status
            .success());
        let (base, catalog) = baseline(&p.0).unwrap();
        assert!(reclaim(&r, &base, &catalog, &"c".repeat(64), &"d".repeat(64)).is_err());
        assert_eq!(
            fs::read(external.0.join("rules.txt")).unwrap(),
            b"external sentinel"
        );
        fs::remove_dir(link).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn retention_sharing_failure_keeps_active_and_allows_retry() {
        use std::os::windows::fs::OpenOptionsExt;
        let p = Profile::new();
        let r = root(&p.0).unwrap();
        commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
        let abandoned = stage(&p.0, &p.bundle(2, "||abandoned.example^"), 1).unwrap();
        let held = fs::OpenOptions::new()
            .read(true)
            .share_mode(1)
            .open(r.join("bundles").join(&abandoned.digest).join("rules.txt"))
            .unwrap();
        let candidate = stage(&p.0, &p.bundle(3, "||three.example^"), 1).unwrap();
        assert!(candidate.stats["storeCleanupError"].is_string());
        assert!(startup(&p.0).is_ok());
        drop(held);
        let candidate = stage(&p.0, &p.bundle(4, "||four.example^"), 1).unwrap();
        assert!(candidate.stats["storeCleanupError"].is_null());
        assert!(!r.join("bundles").join(abandoned.digest).exists());
    }

    #[test]
    fn stale_commit_quota_and_windows_sharing_failure() {
        let p = Profile::new();
        commit(stage(&p.0, &p.bundle(1, "||one.example^"), 0).unwrap(), "").unwrap();
        let candidate = stage(&p.0, &p.bundle(2, "||two.example^"), 1).unwrap();
        let r = root(&p.0).unwrap();
        let mut changed = state(&r).unwrap();
        changed.generation = 2;
        atomic(&r.join("state.json"), &encode(&changed).unwrap()).unwrap();
        assert!(commit(candidate, "").is_err());
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            let candidate = stage(&p.0, &p.bundle(3, "||three.example^"), 2).unwrap();
            let held = fs::OpenOptions::new()
                .read(true)
                .share_mode(3)
                .open(r.join("state.json"))
                .unwrap();
            assert!(commit(candidate, "").is_err());
            drop(held);
            assert_eq!(state(&r).unwrap().generation, 2);
        }
        let sparse = File::create(r.join("quota-fixture")).unwrap();
        sparse.set_len(QUOTA).unwrap();
        drop(sparse);
        assert!(stage(&p.0, &p.bundle(4, "||four.example^"), 2).is_err());
    }
}
