//! TUF admission with filesystem transport and an explicit loopback HTTP test mode.
use std::{
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};
use tough::{IntoVec, Transport, TransportError, TransportErrorKind, TransportStream};
use url::Url;
type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
#[path = "../../FennAdblock/src/admission_jobs.rs"]
mod admission_jobs;
mod broker;
mod cost;
#[path = "../../FennAdblock/src/fetch_jobs.rs"]
mod fetch_jobs;
mod http;
mod state;
#[cfg(test)]
mod tests;
const META: u64 = 1024 * 1024;
const TARGETS: [(&str, u64); 3] = [
    ("descriptor.json", 65536),
    ("rules.txt", 10 * 1024 * 1024),
    ("provenance.json", META),
];
fn regular(path: &Path) -> std::io::Result<()> {
    let meta = fs::symlink_metadata(path)?;
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if meta.file_attributes() & 0x400 != 0 {
            return Err(std::io::Error::other("reparse point rejected"));
        }
    }
    if meta.file_type().is_symlink() {
        return Err(std::io::Error::other("symlink rejected"));
    }
    Ok(())
}
fn read(path: &Path, limit: u64) -> std::io::Result<Vec<u8>> {
    use std::io::Read;
    regular(path)?;
    let mut bytes = Vec::new();
    fs::File::open(path)?
        .take(limit + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err(std::io::Error::other("input bound exceeded"));
    }
    Ok(bytes)
}
#[derive(Clone, Debug)]
struct Local {
    metadata: PathBuf,
    targets: PathBuf,
    budget: Arc<Mutex<(u64, u64)>>,
}
#[async_trait::async_trait]
impl Transport for Local {
    async fn fetch(&self, url: Url) -> std::result::Result<TransportStream, TransportError> {
        let fail = || TransportError::new(TransportErrorKind::Other, url.as_str());
        if url.scheme() != "file" {
            return Err(fail());
        }
        let path = url.to_file_path().map_err(|_| fail())?;
        let parent = path
            .parent()
            .ok_or_else(fail)?
            .canonicalize()
            .map_err(|_| fail())?;
        let limit = if parent == self.metadata {
            META
        } else if parent == self.targets {
            10 * META
        } else {
            return Err(fail());
        };
        let mut budget = self.budget.lock().map_err(|_| fail())?;
        budget.0 += 1;
        if budget.0 > 64 {
            return Err(fail());
        }
        let data = read(&path, limit).map_err(|e| {
            TransportError::new_with_cause(
                if e.kind() == std::io::ErrorKind::NotFound {
                    TransportErrorKind::FileNotFound
                } else {
                    TransportErrorKind::Other
                },
                url.as_str(),
                e,
            )
        })?;
        budget.1 += data.len() as u64;
        if budget.1 > 32 * META {
            return Err(fail());
        }
        Ok(Box::pin(futures::stream::once(std::future::ready(Ok(
            bytes::Bytes::from(data),
        )))))
    }
}
fn hex_hash(bytes: &[u8]) -> String {
    aws_lc_rs::digest::digest(&aws_lc_rs::digest::SHA256, bytes)
        .as_ref()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}
async fn verify(
    anchor: &Path,
    repository: &Path,
    state: &Path,
    output: &Path,
) -> Result<serde_json::Value> {
    regular(repository)?;
    regular(&repository.join("metadata"))?;
    regular(&repository.join("targets"))?;
    let metadata = repository.join("metadata").canonicalize()?;
    let targets = repository.join("targets").canonicalize()?;
    verify_transport(
        anchor,
        Url::from_directory_path(&metadata).map_err(|_| "metadata path")?,
        Url::from_directory_path(&targets).map_err(|_| "targets path")?,
        Local {
            metadata,
            targets,
            budget: Arc::new(Mutex::new((0, 0))),
        },
        "filesystem-only",
        state,
        output,
    )
    .await
}
const MAX_ANCHORS: usize = 8;
/// Each shipped trust root keeps its own TUF record. A browser that ships a new
/// root starts from it instead of failing forever on the old record, and a
/// downgrade returns to the older root's record with its version progress.
/// Bundle rollback stays bounded by the offline store's sequence watermark.
pub(crate) fn anchor_state(state: &Path, identity: &str) -> Result<PathBuf> {
    fs::create_dir_all(state)?;
    regular(state)?;
    let keyed = state.join(format!("anchor-{identity}"));
    if !keyed.exists() {
        adopt_legacy(state, identity, &keyed)?;
    }
    if !keyed.exists() {
        let anchors = fetch_jobs::entries(state, 64)?
            .into_iter()
            .filter(|p| {
                p.file_name()
                    .and_then(|s| s.to_str())
                    .is_some_and(|s| s.starts_with("anchor-"))
            })
            .count();
        if anchors >= MAX_ANCHORS {
            return Err("trust anchor record bound exceeded".into());
        }
    }
    Ok(keyed)
}
/// Records from the former single-anchor layout keep their rollback and freeze
/// progress: a valid record for this anchor is adopted with one directory rename.
/// A corrupt record still fails closed; another anchor's record is left alone.
fn adopt_legacy(state: &Path, identity: &str, keyed: &Path) -> Result<()> {
    let legacy = state.join("state.json");
    if !legacy.exists() {
        return Ok(());
    }
    let bytes = read(&legacy, 8 * META)?;
    let record: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|_| "legacy trust record integrity mismatch")?;
    let payload = &record["payload"];
    if record["sha256"] != hex_hash(&serde_json::to_vec(payload)?) || payload["schema"] != 1 {
        return Err("legacy trust record integrity mismatch".into());
    }
    if payload["anchor"] != identity {
        return Ok(());
    }
    let staging = tempfile::Builder::new()
        .prefix("adopt-")
        .tempdir_in(state)?;
    {
        use std::io::Write;
        let mut file = fs::File::create(staging.path().join("state.json"))?;
        file.write_all(&bytes)?;
        file.sync_all()?;
    }
    let staged = staging.keep();
    if let Err(error) = fs::rename(&staged, keyed) {
        let _ = fs::remove_dir_all(&staged);
        if !keyed.exists() {
            return Err(error.into());
        }
    }
    Ok(())
}
async fn verify_transport<T: Transport + Send + Sync + 'static>(
    anchor: &Path,
    metadata: Url,
    targets: Url,
    transport: T,
    label: &str,
    state: &Path,
    output: &Path,
) -> Result<serde_json::Value> {
    let _admission_lease = admission_jobs::join_output(output)?;
    let anchor = read(anchor, META)?;
    if output.exists() {
        return Err("output already exists".into());
    }
    let identity = hex_hash(&anchor);
    let tx = state::Transaction::begin(&anchor_state(state, &identity)?, &anchor)?;
    let trusted = read(&tx.work.path().join("root.json"), META)?;
    let repository = tough::RepositoryLoader::new(&trusted, metadata, targets)
        .transport(transport)
        .datastore(tx.work.path())
        .expiration_enforcement(tough::ExpirationEnforcement::Safe)
        .limits(tough::Limits {
            max_root_size: META,
            max_targets_size: META,
            max_timestamp_size: 65536,
            max_snapshot_size: META,
            max_root_updates: 16,
        })
        .load()
        .await;
    // Preserve verified root/time/version progress even if later refresh fails.
    // Invalid/truncated scratch JSON cannot replace the previous trust record.
    tx.commit()?;
    let repository = repository?;
    if repository.targets().signed.delegations.is_some() {
        return Err("delegated targets outside prototype scope".into());
    }
    let mut verified = Vec::new();
    for (name, limit) in TARGETS {
        let name = tough::TargetName::new(name)?;
        let target = repository
            .targets()
            .signed
            .targets
            .get(&name)
            .ok_or("missing fixed target")?;
        if target.length > limit {
            return Err("declared target exceeds bound".into());
        }
        // Consume completely: the stream's terminal checksum must succeed.
        let data = repository
            .read_target(&name)
            .await?
            .ok_or("missing target")?
            .into_vec()
            .await?;
        verified.push((name.raw().to_owned(), data));
    }
    let parent = output.parent().ok_or("output parent")?;
    fs::create_dir_all(parent)?;
    let temporary = tempfile::Builder::new()
        .prefix("output-")
        .tempdir_in(parent)?;
    for (name, data) in &verified {
        fs::write(temporary.path().join(name), data)?;
    }
    let receipt = serde_json::json!({"schema":1,"verifier":concat!("tough-", env!("FENN_TOUGH_VERSION")),"transport":label,
        "root_version":repository.root().signed.version.get(),"timestamp_version":repository.timestamp().signed.version.get(),
        "snapshot_version":repository.snapshot().signed.version.get(),"targets_version":repository.targets().signed.version.get(),
        "expires_unix":([repository.root().signed.expires,repository.timestamp().signed.expires,repository.snapshot().signed.expires,repository.targets().signed.expires].iter().map(|t|t.as_second()).min()),
        "anchor_sha256":identity,"targets":verified.iter().map(|(n,d)|(n.clone(),hex_hash(d))).collect::<std::collections::BTreeMap<_,_>>()});
    fs::write(
        temporary.path().join("verification.json"),
        serde_json::to_vec_pretty(&receipt)?,
    )?;

    if std::env::var("FENN_ADBLOCK_TRUST_TEST_CRASH").as_deref() == Ok("after-output-write") {
        std::process::exit(93);
    }
    fs::rename(temporary.path(), output)?;
    if std::env::var("FENN_ADBLOCK_TRUST_TEST_CRASH").as_deref() == Ok("after-output-rename") {
        std::process::exit(94);
    }
    Ok(receipt)
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 1 && args[0] == "--network-cost" {
        println!("{}", cost::network_cost());
        return;
    }
    let http = args.len() == 5 && args[1] == "--test-http";
    let broker = (args.len() == 5)
        .then(|| args[1].to_str())
        .flatten()
        .and_then(|a| match a {
            "--broker" => Some(60_000),
            _ => a
                .strip_prefix("--broker=")
                .and_then(|ms| ms.parse::<u64>().ok())
                .filter(|ms| (1000..=60_000).contains(ms)),
        });
    if args.len() != 4 && !http && broker.is_none() {
        eprintln!(
            "usage: fenn-adblock-trust TRUSTED_ROOT [--test-http|--broker[=MS]] REPOSITORY STATE NEW_OUTPUT"
        );
        std::process::exit(2);
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let result = if http {
        runtime.block_on(http::verify_http(
            Path::new(&args[0]),
            &args[2].to_string_lossy(),
            Path::new(&args[3]),
            Path::new(&args[4]),
        ))
    } else if let Some(budget) = broker {
        runtime.block_on(broker::verify_broker(
            Path::new(&args[0]),
            Path::new(&args[2]),
            Path::new(&args[3]),
            Path::new(&args[4]),
            std::time::Duration::from_millis(budget),
        ))
    } else {
        runtime.block_on(verify(
            Path::new(&args[0]),
            Path::new(&args[1]),
            Path::new(&args[2]),
            Path::new(&args[3]),
        ))
    };
    match result {
        Ok(receipt) => println!("{}", receipt),
        Err(error) => {
            eprintln!("verification rejected: {error}");
            std::process::exit(1);
        }
    }
}
