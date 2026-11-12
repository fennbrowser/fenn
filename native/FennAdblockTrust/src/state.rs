//! One durable trust record; library scratch writes never mutate the selected record.
use super::*;
use std::collections::BTreeMap;
use std::io::Write;
const FILES: [&str; 5] = [
    "root.json",
    "timestamp.json",
    "snapshot.json",
    "targets.json",
    "latest_known_time.json",
];
pub struct Transaction {
    _lock: fs::File,
    pub work: tempfile::TempDir,
    path: PathBuf,
    anchor: String,
}
fn atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let next = path.with_extension("next");
    if next.exists() {
        regular(&next)?;
        fs::remove_file(&next)?;
    }
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&next)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        extern "system" {
            fn MoveFileExW(a: *const u16, b: *const u16, flags: u32) -> i32;
        }
        let a: Vec<u16> = next.as_os_str().encode_wide().chain(Some(0)).collect();
        let b: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(a.as_ptr(), b.as_ptr(), 9) } == 0 {
            return Err(std::io::Error::last_os_error().into());
        }
    }
    #[cfg(not(windows))]
    {
        fs::rename(next, path)?;
        fs::File::open(path.parent().unwrap())?.sync_all()?;
    }
    Ok(())
}
fn clean_work(path: &Path) -> Result<()> {
    if !crate::fetch_jobs::plain(path)?.is_dir() {
        return Err("invalid trust scratch directory".into());
    }
    let entries = crate::fetch_jobs::entries(path, 5)?;
    for file in &entries {
        let m = crate::fetch_jobs::plain(file)?;
        if !m.is_file()
            || m.len() > META
            || !file
                .file_name()
                .and_then(|s| s.to_str())
                .is_some_and(|s| FILES.contains(&s))
        {
            return Err("unknown or oversized trust scratch".into());
        }
    }
    for file in entries {
        fs::remove_file(file)?;
    }
    fs::remove_dir(path)?;
    Ok(())
}
impl Transaction {
    pub fn begin(state: &Path, anchor: &[u8]) -> Result<Self> {
        fs::create_dir_all(state)?;
        regular(state)?;
        let mut options = fs::OpenOptions::new();
        options.read(true).write(true).create(true);
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            options.share_mode(0);
        }
        let lockpath = state.join("writer.lock");
        if lockpath.exists() {
            regular(&lockpath)?;
        }
        let lock = options.open(lockpath)?;
        #[cfg(not(windows))]
        lock.try_lock()?;
        let path = state.join("state.json");
        let identity = hex_hash(anchor);
        let mut files = BTreeMap::<String, String>::new();
        if path.exists() {
            let record: serde_json::Value = serde_json::from_slice(&read(&path, 8 * META)?)?;
            let payload = &record["payload"];
            if record["sha256"] != hex_hash(&serde_json::to_vec(payload)?)
                || payload["schema"] != 1
                || payload["anchor"] != identity
            {
                return Err("trust record integrity/anchor mismatch".into());
            }
            files = serde_json::from_value(payload["files"].clone())?;
            if !files.contains_key("root.json")
                || files.keys().any(|k| !FILES.contains(&k.as_str()))
            {
                return Err("invalid trust record files".into());
            }
        } else {
            if fs::read_dir(state)?.count() != 1 {
                return Err("missing trust record in established directory".into());
            }
            files.insert("root.json".into(), String::from_utf8(anchor.to_vec())?);
        }
        // Only after validating the selected record and acquiring the writer
        // lock: no active verifier can own any of these disposable directories.
        for scratch in crate::fetch_jobs::entries(state, 64)? {
            if scratch
                .file_name()
                .and_then(|s| s.to_str())
                .is_some_and(|s| s.starts_with("trust-work-"))
            {
                let _ = clean_work(&scratch);
            }
        }
        // Unknown/legacy directories remain bounded and never become authority.
        if fs::read_dir(state)?.count() > 32 {
            return Err("trust scratch quota requires cleanup".into());
        }
        let work = tempfile::Builder::new()
            .prefix("trust-work-")
            .tempdir_in(state)?;
        for (name, data) in files {
            if data.len() as u64 > META {
                return Err("cached metadata bound".into());
            }
            fs::write(work.path().join(name), data)?;
        }
        let tx = Self {
            _lock: lock,
            work,
            path,
            anchor: identity,
        };
        if !tx.path.exists() {
            tx.commit()?;
        }
        Ok(tx)
    }
    pub fn commit(&self) -> Result<()> {
        self.commit_at("")
    }
    fn commit_at(&self, point: &str) -> Result<()> {
        let mut files = BTreeMap::new();
        for name in FILES {
            let path = self.work.path().join(name);
            if !path.exists() {
                continue;
            }
            let bytes = read(&path, META)?;
            let _: serde_json::Value = serde_json::from_slice(&bytes)?;
            files.insert(name, String::from_utf8(bytes)?);
        }
        if !files.contains_key("root.json") {
            return Err("missing working root".into());
        }
        let payload = serde_json::json!({"schema":1,"anchor":self.anchor,"files":files});
        let record = serde_json::json!({"sha256":hex_hash(&serde_json::to_vec(&payload)?),"payload":payload});
        if point == "before-replace" {
            return Err("injected before replacement".into());
        }
        if std::env::var("FENN_ADBLOCK_TRUST_TEST_CRASH").as_deref() == Ok("before-replace") {
            std::process::exit(91);
        }
        let bytes = serde_json::to_vec(&record)?;
        if bytes.len() as u64 > 8 * META {
            return Err("trust record size bound".into());
        }
        atomic(&self.path, &bytes)?;
        if std::env::var("FENN_ADBLOCK_TRUST_TEST_CRASH").as_deref() == Ok("after-replace") {
            std::process::exit(92);
        }
        if point == "after-replace" {
            return Err("injected after replacement".into());
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dead_scratch_recovery_preserves_authority_and_live_writer() {
        let dir = tempfile::tempdir().unwrap();
        let state = dir.path().join("state");
        let tx = Transaction::begin(&state, b"{}").unwrap();
        let selected = fs::read(state.join("state.json")).unwrap();
        let live = tx.work.path().to_path_buf();
        assert!(Transaction::begin(&state, b"{}").is_err());
        assert!(live.exists());
        drop(tx);
        for n in 0..40 {
            let dead = state.join(format!("trust-work-{n}"));
            fs::create_dir(&dead).unwrap();
            fs::write(dead.join("timestamp.json"), b"truncated").unwrap();
        }
        let tx = Transaction::begin(&state, b"{}").unwrap();
        assert_eq!(fs::read(state.join("state.json")).unwrap(), selected);
        assert_eq!(fs::read_dir(&state).unwrap().count(), 3);
        drop(tx);
        let unknown = state.join("trust-work-unknown");
        fs::create_dir(&unknown).unwrap();
        fs::write(unknown.join("keep"), b"keep").unwrap();
        drop(Transaction::begin(&state, b"{}").unwrap());
        assert!(unknown.join("keep").exists());
        let dead = state.join("trust-work-dead");
        fs::create_dir(&dead).unwrap();
        fs::write(dead.join("root.json"), b"{}").unwrap();
        fs::write(state.join("state.json"), b"corrupt").unwrap();
        assert!(Transaction::begin(&state, b"{}").is_err());
        assert!(dead.exists());
    }
    #[test]
    fn interrupted_selection_and_released_lock() {
        let dir = tempfile::tempdir().unwrap();
        let state = dir.path().join("state");
        for point in ["before-replace", "after-replace"] {
            let tx = Transaction::begin(&state, b"{}").unwrap();
            fs::write(tx.work.path().join("timestamp.json"), b"{\"version\":2}").unwrap();
            assert!(tx.commit_at(point).is_err());
            drop(tx);
            let tx = Transaction::begin(&state, b"{}").unwrap();
            assert_eq!(
                tx.work.path().join("timestamp.json").exists(),
                point == "after-replace"
            );
        }
    }
}
