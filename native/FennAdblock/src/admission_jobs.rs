//! Authenticated-output scratch has independent browser and verifier leases.
#![allow(dead_code)]
use crate::fetch_jobs::{entries, open, plain};
use std::{
    fs, io,
    path::{Path, PathBuf},
};
pub struct Admission {
    pub path: PathBuf,
    lease: Option<fs::File>,
}
impl Drop for Admission {
    fn drop(&mut self) {
        self.lease.take();
        let _ = recover(&self.path);
    }
}
fn output_files(path: &Path) -> io::Result<Vec<PathBuf>> {
    if !plain(path)?.is_dir() {
        return Err(io::Error::other("invalid output scratch directory"));
    }
    let files = entries(path, 4)?;
    for file in &files {
        let m = plain(file)?;
        let limit = match file.file_name().and_then(|s| s.to_str()) {
            Some("rules.txt") => 10 * 1024 * 1024,
            Some("provenance.json") => 1024 * 1024,
            Some("descriptor.json" | "verification.json") => 65536,
            _ => return Err(io::Error::other("unknown output scratch file")),
        };
        if !m.is_file() || m.len() > limit {
            return Err(io::Error::other("output scratch bound/type"));
        }
    }
    Ok(files)
}
pub fn recover(job: &Path) -> io::Result<bool> {
    if !plain(job)?.is_dir() {
        return Ok(false);
    }
    let lease = match open(&job.join("owner.lock"), false, true) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(fs::remove_dir(job).is_ok()),
        Err(_) => return Ok(false),
    };
    let mut directories = Vec::new();
    for child in entries(job, 3)? {
        let name = child.file_name().and_then(|s| s.to_str()).unwrap_or("");
        if name == "owner.lock" {
            continue;
        }
        if name != "verified"
            && !(name.starts_with("output-")
                && name[7..].bytes().all(|b| b.is_ascii_alphanumeric()))
        {
            return Ok(false);
        }
        directories.push((child.clone(), output_files(&child)?));
    }
    // Validate the whole bounded tree before deleting anything; no recursion.
    for (dir, files) in directories {
        for file in files {
            fs::remove_file(file)?;
        }
        fs::remove_dir(dir)?;
    }
    fs::remove_file(job.join("owner.lock"))?;
    drop(lease);
    fs::remove_dir(job)?;
    Ok(true)
}
pub fn begin(store: &Path) -> io::Result<Admission> {
    let root = store.join("admission-jobs");
    fs::create_dir_all(&root)?;
    if !plain(&root)?.is_dir() {
        return Err(io::Error::other("invalid admission root"));
    }
    for job in entries(&root, 64)? {
        let _ = recover(&job);
    }
    if entries(&root, 64)?.len() >= 8 {
        return Err(io::Error::other("admission scratch quota"));
    }
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_nanos();
    let path = root.join(format!("admission-{}-{stamp}", std::process::id()));
    fs::create_dir(&path)?;
    let lease = open(&path.join("owner.lock"), true, false)?;
    Ok(Admission {
        path,
        lease: Some(lease),
    })
}
pub fn join_output(output: &Path) -> io::Result<Option<fs::File>> {
    let parent = output
        .parent()
        .ok_or_else(|| io::Error::other("missing output parent"))?;
    if parent
        .parent()
        .and_then(Path::file_name)
        .is_some_and(|s| s == "admission-jobs")
    {
        if output.file_name().is_none_or(|s| s != "verified") {
            return Err(io::Error::other("invalid admission output"));
        }
        return crate::fetch_jobs::join(parent).map(Some);
    }
    Ok(None) // Standalone caller owns its chosen destination.
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn live_owners_unknown_content_and_dead_quota() {
        let store = std::env::temp_dir().join(format!(
            "fenn-admission-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&store).unwrap();
        let job = begin(&store).unwrap();
        let path = job.path.clone();
        let helper = join_output(&path.join("verified")).unwrap();
        assert!(!recover(&path).unwrap());
        drop(job);
        assert!(path.exists());
        assert!(!recover(&path).unwrap());
        drop(helper);
        fs::create_dir(path.join("verified")).unwrap();
        fs::write(path.join("verified/unknown"), b"keep").unwrap();
        assert!(recover(&path).is_err());
        assert!(path.join("owner.lock").exists());
        fs::remove_file(path.join("verified/unknown")).unwrap();
        fs::write(path.join("verified/rules.txt"), b"partial").unwrap();
        assert!(recover(&path).unwrap());
        for n in 0..40 {
            let dead = store.join("admission-jobs").join(format!("dead-{n}"));
            fs::create_dir(&dead).unwrap();
            drop(open(&dead.join("owner.lock"), true, false).unwrap());
            fs::create_dir(dead.join("output-test")).unwrap();
            fs::write(dead.join("output-test/descriptor.json"), b"partial").unwrap();
        }
        let job = begin(&store).unwrap();
        assert_eq!(entries(&store.join("admission-jobs"), 64).unwrap().len(), 1);
        drop(job);
        fs::remove_dir(store.join("admission-jobs")).unwrap();
        fs::remove_dir(store).unwrap();
    }
}
