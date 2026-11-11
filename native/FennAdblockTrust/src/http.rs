//! Explicit loopback test transport. Not a production endpoint or TLS policy.
use super::*;
use std::time::{Duration, Instant};
#[derive(Clone, Debug)]
pub struct Http {
    client: reqwest::Client,
    base: Url,
    budget: Arc<Mutex<(u64, u64)>>,
    deadline: Instant,
}
impl Http {
    pub fn new(base: Url) -> Result<Self> {
        if base.scheme() != "http"
            || !matches!(base.host_str(), Some("127.0.0.1" | "[::1]"))
            || base.port().is_none()
            || !base.username().is_empty()
            || base.password().is_some()
            || base.query().is_some()
            || base.fragment().is_some()
            || !base.path().ends_with('/')
            || base.path().contains('%')
        {
            return Err(
                "test HTTP requires literal loopback, explicit port, and a directory URL".into(),
            );
        }
        let client = reqwest::Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .retry(reqwest::retry::never())
            .no_gzip()
            .no_brotli()
            .no_deflate()
            .no_zstd()
            .connect_timeout(Duration::from_secs(1))
            .timeout(Duration::from_secs(2))
            .referer(false)
            .build()?;
        Ok(Self {
            client,
            base,
            budget: Arc::new(Mutex::new((0, 0))),
            deadline: Instant::now() + Duration::from_secs(20),
        })
    }
    fn limit(&self, url: &Url) -> Option<u64> {
        if url.origin() != self.base.origin()
            || !url.username().is_empty()
            || url.password().is_some()
            || url.query().is_some()
            || url.fragment().is_some()
        {
            return None;
        }
        let relative = url.path().strip_prefix(self.base.path())?;
        let (kind, name) = relative.split_once('/')?;
        if name.is_empty()
            || name.len() > 200
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b'_')
        {
            return None;
        }
        match kind {
            "metadata" if name.ends_with(".json") => Some(if name == "timestamp.json" {
                65536
            } else {
                META
            }),
            "targets" => TARGETS
                .iter()
                .find(|(fixed, _)| {
                    name == *fixed
                        || name.strip_suffix(*fixed).is_some_and(|prefix| {
                            prefix.len() == 65
                                && prefix.ends_with('.')
                                && prefix[..64].bytes().all(|b| b.is_ascii_hexdigit())
                        })
                })
                .map(|(_, limit)| *limit),
            _ => None,
        }
    }
    async fn fetch_inner(
        &self,
        url: &Url,
        limit: u64,
    ) -> std::result::Result<Vec<u8>, (TransportErrorKind, bool)> {
        let reject = (TransportErrorKind::Other, false);
        {
            let mut budget = self.budget.lock().map_err(|_| reject)?;
            budget.0 += 1;
            if budget.0 > 64 || Instant::now() >= self.deadline {
                return Err(reject);
            }
        }
        let remaining = self
            .deadline
            .saturating_duration_since(Instant::now())
            .min(Duration::from_secs(2));
        let mut response = self
            .client
            .get(url.clone())
            .header("Accept-Encoding", "identity")
            .timeout(remaining)
            .send()
            .await
            .map_err(|_| (TransportErrorKind::Other, true))?;
        let status = response.status().as_u16();
        if status == 404 {
            return Err((TransportErrorKind::FileNotFound, false));
        }
        if status != 200 {
            return Err((
                TransportErrorKind::Other,
                matches!(status, 408 | 429 | 500 | 502 | 503 | 504),
            ));
        }
        if response.headers().contains_key("content-encoding")
            || response.content_length().is_some_and(|len| len > limit)
        {
            return Err(reject);
        }
        let mut data = Vec::new();
        while let Some(chunk) = response
            .chunk()
            .await
            .map_err(|_| (TransportErrorKind::Other, true))?
        {
            // Include discarded retry bytes in the total session budget.
            let mut budget = self.budget.lock().map_err(|_| reject)?;
            budget.1 = budget.1.saturating_add(chunk.len() as u64);
            if budget.1 > 32 * META
                || data.len() as u64 + chunk.len() as u64 > limit
                || Instant::now() >= self.deadline
            {
                return Err(reject);
            }
            data.extend_from_slice(&chunk);
        }
        Ok(data)
    }
}
#[async_trait::async_trait]
impl Transport for Http {
    async fn fetch(&self, url: Url) -> std::result::Result<TransportStream, TransportError> {
        let limit = self
            .limit(&url)
            .ok_or_else(|| TransportError::new(TransportErrorKind::Other, url.as_str()))?;
        for attempt in 0..2 {
            match self.fetch_inner(&url, limit).await {
                Ok(data) => {
                    return Ok(Box::pin(futures::stream::once(std::future::ready(Ok(
                        bytes::Bytes::from(data),
                    )))))
                }
                Err((_, true))
                    if attempt == 0
                        && Instant::now() + Duration::from_millis(100) < self.deadline =>
                {
                    tokio::time::sleep(Duration::from_millis(100)).await
                }
                Err((kind, _)) => return Err(TransportError::new(kind, url.as_str())),
            }
        }
        unreachable!()
    }
}
pub async fn verify_http(
    anchor: &Path,
    source: &str,
    state: &Path,
    output: &Path,
) -> Result<serde_json::Value> {
    let base = Url::parse(source)?;
    let transport = Http::new(base.clone())?;
    super::verify_transport(
        anchor,
        base.join("metadata/")?,
        base.join("targets/")?,
        transport,
        "loopback-http-test",
        state,
        output,
    )
    .await
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exhausted_request_budget_and_deadline_do_not_retry() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let base = Url::parse("http://127.0.0.1:1/repo/").unwrap();
        let url = base.join("metadata/timestamp.json").unwrap();
        let mut t = Http::new(base).unwrap();
        t.budget.lock().unwrap().0 = 64;
        assert!(runtime.block_on(t.fetch(url.clone())).is_err());
        assert_eq!(t.budget.lock().unwrap().0, 65);
        t.budget.lock().unwrap().0 = 0;
        t.deadline = Instant::now() - Duration::from_secs(1);
        assert!(runtime.block_on(t.fetch(url)).is_err());
        assert_eq!(t.budget.lock().unwrap().0, 1);
    }
    #[test]
    fn endpoint_and_path_confinement() {
        for url in [
            "https://127.0.0.1:8080/",
            "http://example.com:8080/",
            "http://localhost:8080/",
            "http://127.0.0.1/",
            "http://u:p@127.0.0.1:8080/",
            "http://127.0.0.1:8080/?q=1",
        ] {
            assert!(Http::new(Url::parse(url).unwrap()).is_err());
        }
        let t = Http::new(Url::parse("http://127.0.0.1:8080/repo/").unwrap()).unwrap();
        for url in [
            "http://127.0.0.1:8081/repo/metadata/root.json",
            "http://127.0.0.1:8080/metadata/root.json",
            "http://127.0.0.1:8080/repo/targets/other.txt",
            "http://127.0.0.1:8080/repo/metadata/sub/root.json",
        ] {
            assert!(t.limit(&Url::parse(url).unwrap()).is_none());
        }
        assert_eq!(
            t.limit(&Url::parse("http://127.0.0.1:8080/repo/targets/descriptor.json").unwrap()),
            Some(65536)
        );
    }
}
