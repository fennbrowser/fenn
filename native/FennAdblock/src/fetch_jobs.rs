//! Shared browser/verifier lifetime lease. No PID or age can authorize deletion.
// Compiled into two binaries which intentionally use different entry points.
#![allow(dead_code)]
use std::{
    fs::{self, File, OpenOptions},
    io,
    path::{Path, PathBuf},
};
const LEASE: &str = "owner.lock";
pub(crate) fn plain(path: &Path) -> io::Result<fs::Metadata> {
    let m = fs::symlink_metadata(path)?;
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if m.file_attributes() & 0x400 != 0 {
            return Err(io::Error::other("reparse point in fetch job"));
        }
    }
    if m.file_type().is_symlink() {
        return Err(io::Error::other("symlink in fetch job"));
    }
    Ok(m)
}
pub(crate) fn open(path: &Path, create: bool, exclusive: bool) -> io::Result<File> {
    if !create && !plain(path)?.is_file() {
        return Err(io::Error::other("invalid fetch lease"));
    }
    let mut options = OpenOptions::new();
    options.read(true).write(true).create_new(create);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        // Shared owners deny deletion; recovery denies all competing read/write
        // opens but permits deleting its own open lease during flat cleanup.
        options.share_mode(if exclusive { 4 } else { 3 });
    }
    let file = options.open(path)?;
    #[cfg(not(windows))]
    {
        if exclusive {
            file.try_lock()?;
        } else {
            file.try_lock_shared()?;
        }
    }
    Ok(file)
}
pub fn join(job: &Path) -> io::Result<File> {
    if !plain(job)?.is_dir() {
        return Err(io::Error::other("invalid fetch directory"));
    }
    open(&job.join(LEASE), false, false)
}
pub(crate) fn entries(root: &Path, limit: usize) -> io::Result<Vec<PathBuf>> {
    let mut result = Vec::new();
    for entry in fs::read_dir(root)? {
        if result.len() == limit {
            return Err(io::Error::other("fetch directory scan bound"));
        }
        result.push(entry?.path());
    }
    Ok(result)
}
fn owned_name(name: &str) -> bool {
    if name == LEASE {
        return true;
    }
    for prefix in ["request-", "reply-", "body-"] {
        if let Some(suffix) = name.strip_prefix(prefix) {
            let number = if prefix == "body-" {
                suffix
            } else {
                match suffix
                    .strip_suffix(".json")
                    .or_else(|| suffix.strip_suffix(".next"))
                {
                    Some(n) => n,
                    None => return false,
                }
            };
            return number.parse::<u32>().is_ok_and(|n| (1..=64).contains(&n));
        }
    }
    false
}
pub fn recover(job: &Path) -> io::Result<bool> {
    if !plain(job)?.is_dir() {
        return Ok(false);
    }
    let lease = match open(&job.join(LEASE), false, true) {
        Ok(file) => file,
        // A missing legacy lease or a sharing/access failure is not death proof.
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            // Atomic empty-directory removal also recovers crashes before lease
            // creation or after its final deletion. Never recurse without proof.
            return Ok(fs::remove_dir(job).is_ok());
        }
        Err(_) => return Ok(false),
    };
    let files = entries(job, 260)?;
    let mut bytes = 0u64;
    for file in &files {
        let m = plain(file)?;
        bytes = bytes.saturating_add(m.len());
        if !m.is_file()
            || m.len() > 10 * 1024 * 1024
            || bytes > 34 * 1024 * 1024
            || !file
                .file_name()
                .and_then(|s| s.to_str())
                .is_some_and(owned_name)
        {
            return Ok(false);
        }
    }
    // Never recurse: all entries were validated under the exclusive lease.
    for file in files {
        if file.file_name().is_some_and(|n| n != LEASE) {
            fs::remove_file(file)?;
        }
    }
    fs::remove_file(job.join(LEASE))?;
    drop(lease);
    fs::remove_dir(job)?;
    Ok(true)
}
pub fn begin(profile: &Path, job: &Path) -> io::Result<File> {
    let root = profile.join("fenn-adblock-fetch");
    if !plain(&root)?.is_dir()
        || !plain(job)?.is_dir()
        || job
            .parent()
            .ok_or_else(|| io::Error::other("missing job parent"))?
            .canonicalize()?
            != root.canonicalize()?
    {
        return Err(io::Error::other("fetch job outside profile root"));
    }
    let lease = open(&job.join(LEASE), true, false)?;
    for other in entries(&root, 64)? {
        if other != job {
            let _ = recover(&other);
        }
    }
    if entries(&root, 64)?.len() > 8 {
        return Err(io::Error::other("Fetch scratch quota exceeded"));
    }
    Ok(lease)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn leases_and_unknown_files_prevent_recovery() {
        let root = std::env::temp_dir().join(format!(
            "fenn-fetch-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&root).unwrap();
        let owner = open(&root.join(LEASE), true, false).unwrap();
        let verifier = join(&root).unwrap();
        assert!(!recover(&root).unwrap());
        drop(owner);
        assert!(!recover(&root).unwrap());
        drop(verifier);
        fs::write(root.join("unowned"), b"keep").unwrap();
        assert!(!recover(&root).unwrap());
        fs::remove_file(root.join("unowned")).unwrap();
        fs::write(root.join("body-1"), b"partial").unwrap();
        assert!(recover(&root).unwrap());
        assert!(!root.exists());
        // Crash before creating / after unlinking the lease: only an atomic
        // empty-directory removal is permitted without ownership evidence.
        fs::create_dir(&root).unwrap();
        assert!(recover(&root).unwrap());
        fs::create_dir(&root).unwrap();
        fs::write(root.join("body-1"), b"legacy").unwrap();
        assert!(!recover(&root).unwrap());
        fs::remove_file(root.join("body-1")).unwrap();
        drop(open(&root.join(LEASE), true, false).unwrap());
        fs::create_dir(root.join("nested")).unwrap();
        fs::write(root.join("nested/keep"), b"keep").unwrap();
        assert!(!recover(&root).unwrap());
        assert_eq!(fs::read(root.join("nested/keep")).unwrap(), b"keep");
        fs::remove_file(root.join("nested/keep")).unwrap();
        fs::remove_dir(root.join("nested")).unwrap();
        assert!(recover(&root).unwrap());
    }
}
