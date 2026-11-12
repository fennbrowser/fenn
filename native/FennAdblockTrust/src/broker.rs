//! File-backed request broker. Firefox supplies bytes; TUF retains authority.
use super::*;
use std::time::{Duration, Instant};
#[derive(Clone, Debug)]
struct Broker {
    root: PathBuf,
    base: Url,
    serial: Arc<tokio::sync::Mutex<u64>>,
    bytes: Arc<Mutex<u64>>,
    deadline: Instant,
}
#[async_trait::async_trait]
impl Transport for Broker {
    async fn fetch(&self, url: Url) -> std::result::Result<TransportStream, TransportError> {
        let fail = || TransportError::new(TransportErrorKind::Other, url.as_str());
        let relative = url
            .as_str()
            .strip_prefix(self.base.as_str())
            .ok_or_else(fail)?;
        let (kind, name) = relative.split_once('/').ok_or_else(fail)?;
        if name.is_empty()
            || name.len() > 200
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b'_')
        {
            return Err(fail());
        }
        let limit = match kind {
            "metadata" if name.ends_with(".json") => {
                if name == "timestamp.json" {
                    65536
                } else {
                    META
                }
            }
            "targets" => TARGETS
                .iter()
                .find(|(fixed, _)| {
                    name == *fixed
                        || name.strip_suffix(*fixed).is_some_and(|p| {
                            p.len() == 65
                                && p.ends_with('.')
                                && p[..64].bytes().all(|b| b.is_ascii_hexdigit())
                        })
                })
                .map(|(_, limit)| *limit)
                .ok_or_else(fail)?,
            _ => return Err(fail()),
        };
        let mut id = self.serial.lock().await;
        *id += 1;
        if *id > 64 || Instant::now() >= self.deadline {
            return Err(fail());
        }
        let request =
            serde_json::to_vec(&serde_json::json!({"id":*id,"path":relative,"limit":limit}))
                .map_err(|_| fail())?;
        let temporary = self.root.join(format!("request-{}.next", *id));
        fs::write(&temporary, request).map_err(|_| fail())?;
        fs::rename(temporary, self.root.join(format!("request-{}.json", *id)))
            .map_err(|_| fail())?;
        let reply = self.root.join(format!("reply-{}.json", *id));
        while !reply.exists() {
            if Instant::now() >= self.deadline {
                return Err(fail());
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        let result: serde_json::Value =
            serde_json::from_slice(&read(&reply, 4096).map_err(|_| fail())?).map_err(|_| fail())?;
        if result["id"].as_u64() != Some(*id) {
            return Err(fail());
        }
        match result["status"].as_str() {
            Some("not-found") => {
                return Err(TransportError::new(
                    TransportErrorKind::FileNotFound,
                    url.as_str(),
                ))
            }
            Some("ok") => {}
            _ => return Err(fail()),
        }
        let data = read(&self.root.join(format!("body-{}", *id)), limit).map_err(|_| fail())?;
        let mut bytes = self.bytes.lock().map_err(|_| fail())?;
        *bytes += data.len() as u64;
        if *bytes > 32 * META {
            return Err(fail());
        }
        Ok(Box::pin(futures::stream::once(std::future::ready(Ok(
            bytes::Bytes::from(data),
        )))))
    }
}
pub async fn verify_broker(
    anchor: &Path,
    source: &Path,
    state: &Path,
    output: &Path,
    budget: Duration,
) -> Result<serde_json::Value> {
    let _lease = crate::fetch_jobs::join(source)?;
    regular(source)?;
    let root = source.canonicalize()?;
    let base = Url::from_directory_path(&root).map_err(|_| "broker path")?;
    let broker = Broker {
        root,
        base: base.clone(),
        serial: Arc::new(tokio::sync::Mutex::new(0)),
        bytes: Arc::new(Mutex::new(0)),
        deadline: Instant::now() + budget.min(Duration::from_secs(60)),
    };
    super::verify_transport(
        anchor,
        base.join("metadata/")?,
        base.join("targets/")?,
        broker,
        "firefox-broker",
        state,
        output,
    )
    .await
}
