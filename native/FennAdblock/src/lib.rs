//! Experimental Gecko-side engine. Only JSON owned across the ABI; no browser
//! pointers, callbacks, shell dependency, or unwinding across FFI.
use adblock::{
    lists::{FilterSet, ParseOptions},
    resources::{InMemoryResourceStorage, Resource},
    Engine,
};
use serde_json::{json, Value};
use std::{
    ffi::{c_char, CString},
    fs::{self, File},
    io::{Read, Write},
    panic::{catch_unwind, AssertUnwindSafe},
    path::Path,
    slice,
    sync::Mutex,
    thread::{self, JoinHandle},
    time::Instant,
};
pub mod capabilities;
mod admission_jobs;
mod fetch_jobs;
mod offline;

const MAX_INPUT: usize = 16 * 1024 * 1024;
const MAX_OUTPUT: usize = 4 * 1024 * 1024;
static ENGINE: Mutex<Option<Engine>> = Mutex::new(None);
type Prepared = Result<(Engine, Value), String>;
static JOB: Mutex<Option<JoinHandle<Prepared>>> = Mutex::new(None);
const MAX_CONFIG: usize = 10 * 1024 * 1024;
const MAX_CACHE: usize = 64 * 1024 * 1024;
// Changing adapter semantics requires a new prefix. Features and dependency
// upgrades invalidate automatically, including changes to the pinned engine.
const CACHE_PREFIX: &[u8] = b"Fenn-adblock-cache-v3\0";

fn validate_rules(rules: &str) -> Result<(), String> {
    if rules
        .lines()
        .any(|line| line.trim_start().starts_with("!#"))
    {
        return Err("rules contain unresolved preprocessing directives".into());
    }
    Ok(())
}

fn install_resources(engine: &mut Engine, resources: Vec<Resource>) -> Result<(), String> {
    let mut storage = InMemoryResourceStorage::default();
    for resource in resources {
        let name = resource.name.clone();
        storage
            .add_resource(resource)
            .map_err(|e| format!("resource {name}: {e:?}"))?;
    }
    engine.use_resource_storage(storage);
    Ok(())
}

fn bounded_read(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
    let file = File::open(path).map_err(|e| e.to_string())?;
    let mut data = Vec::new();
    file.take((limit + 1) as u64)
        .read_to_end(&mut data)
        .map_err(|e| e.to_string())?;
    if data.len() > limit {
        return Err("file exceeds bound".into());
    }
    Ok(data)
}

fn cache_identity(config: &[u8]) -> Result<Vec<u8>, String> {
    let mut key = CACHE_PREFIX.to_vec();
    for part in [
        include_bytes!("../Cargo.toml").as_slice(),
        include_bytes!("../Cargo.lock").as_slice(),
        config,
    ] {
        key.extend_from_slice(&(part.len() as u64).to_le_bytes());
        key.extend_from_slice(part);
    }
    let mut header = CACHE_PREFIX.to_vec();
    header.extend_from_slice(offline::digest(&key)?.as_bytes());
    Ok(header)
}

fn prepare(profile: &Path, bypass_cache: bool, delay_ms: u64) -> Prepared {
    let start = Instant::now();
    if delay_ms != 0 {
        thread::sleep(std::time::Duration::from_millis(delay_ms.min(2000)));
    }
    let config_bytes = bounded_read(&profile.join("fenn-adblock-prototype.json"), MAX_CONFIG)?;
    prepare_config(
        config_bytes,
        profile.join("fenn-adblock-prototype.cache"),
        bypass_cache,
        start,
    )
}

fn prepare_config(
    config_bytes: Vec<u8>,
    cache_path: std::path::PathBuf,
    bypass_cache: bool,
    start: Instant,
) -> Prepared {
    let config: Value = serde_json::from_slice(&config_bytes).map_err(|e| e.to_string())?;
    if config["version"] != 1 {
        return Err("unsupported configuration version".into());
    }
    let rules = field(&config, "rules")?;
    validate_rules(rules)?;
    let resources: Vec<Resource> =
        serde_json::from_value(config["resources"].clone()).map_err(|e| e.to_string())?;
    let identity = cache_identity(&config_bytes)?;
    let mut engine = Engine::default();
    let cache_hit = !bypass_cache
        && bounded_read(&cache_path, MAX_CACHE)
            .ok()
            .filter(|bytes| bytes.starts_with(&identity))
            .is_some_and(|bytes| engine.deserialize(&bytes[identity.len()..]).is_ok());
    let engine_start = Instant::now();
    let mut cache_error = None;
    if !cache_hit {
        let mut filters = FilterSet::new(false);
        filters.add_filter_list(rules.to_string(), ParseOptions::default());
        engine = Engine::new_with_filter_set(filters);
    }
    let build_ms = engine_start.elapsed().as_secs_f64() * 1000.0;
    // Engine serialization deliberately excludes resources. Restore them on hits
    // as well as misses, from the exact same configuration bound into identity.
    install_resources(&mut engine, resources)?;
    if !cache_hit && !bypass_cache {
        let write = || -> Result<(), String> {
            let mut bytes = identity;
            bytes.extend_from_slice(&engine.serialize());
            if bytes.len() > MAX_CACHE {
                return Err("snapshot exceeds bound".into());
            }
            let temp = cache_path.with_extension("cache.tmp");
            let mut file = File::create(&temp).map_err(|e| e.to_string())?;
            file.write_all(&bytes)
                .and_then(|_| file.sync_all())
                .map_err(|e| e.to_string())?;
            drop(file);
            fs::rename(&temp, &cache_path).map_err(|e| e.to_string())
        };
        cache_error = write().err(); // Cache failure must not lose a valid engine.
    }
    Ok((
        engine,
        json!({"cacheHit":cache_hit,"prepareMs":start.elapsed().as_secs_f64()*1000.0,
        "buildMs":build_ms,"cacheError":cache_error}),
    ))
}

fn field<'a>(v: &'a Value, key: &str) -> Result<&'a str, String> {
    v[key]
        .as_str()
        .ok_or_else(|| format!("missing string: {key}"))
}

fn dispatch(v: Value) -> Result<Value, String> {
    if field(&v, "op")?.starts_with("offline-") {
        if JOB.lock().map_err(|_| "job lock poisoned")?.is_some() {
            return Err("preparation busy".into());
        }
        let (engine, reply) = offline::dispatch(&v)?;
        if let Some(engine) = engine {
            *ENGINE.lock().map_err(|_| "engine lock poisoned")? = Some(engine);
        }
        return Ok(reply);
    }
    match field(&v, "op")? {
        "begin" => {
            if offline::busy() {
                return Err("offline preparation busy".into());
            }
            offline::discard();
            let mut job = JOB.lock().map_err(|_| "job lock poisoned")?;
            if job.is_some() {
                return Err("preparation already running".into());
            }
            let profile = std::path::PathBuf::from(field(&v, "profile")?);
            let bypass = v["bypassCache"].as_bool().unwrap_or(false);
            let delay = v["delayMs"].as_u64().unwrap_or(0);
            let store = v["offlineStore"].as_bool().unwrap_or(false);
            let assets = v["assets"].as_str().map(std::path::PathBuf::from);
            *job = Some(
                thread::Builder::new()
                    .name("FennAdblockPrepare".into())
                    .spawn(move || {
                        if store {
                            offline::startup_assets(&profile, assets.as_deref())
                        } else {
                            prepare(&profile, bypass, delay)
                        }
                    })
                    .map_err(|e| e.to_string())?,
            );
            return Ok(json!({}));
        }
        "poll" => {
            let mut job = JOB.lock().map_err(|_| "job lock poisoned")?;
            let Some(handle) = job.as_ref() else {
                return Err("no preparation".into());
            };
            if !handle.is_finished() {
                return Ok(json!({"done":false}));
            }
            let (engine, stats) = job
                .take()
                .unwrap()
                .join()
                .map_err(|_| "preparation panicked")??;
            let accept = v["accept"].as_bool().unwrap_or(false);
            if accept {
                *ENGINE.lock().map_err(|_| "engine lock poisoned")? = Some(engine);
            }
            return Ok(json!({"done":true,"accepted":accept,"stats":stats}));
        }
        "shutdown" => {
            offline::shutdown();
            if let Some(handle) = JOB.lock().map_err(|_| "job lock poisoned")?.take() {
                let _ = handle.join(); // Never unload DLL code while its worker runs.
            }
            *ENGINE.lock().map_err(|_| "engine lock poisoned")? = None;
            return Ok(json!({}));
        }
        _ => {}
    }
    let mut slot = ENGINE.lock().map_err(|_| "engine lock poisoned")?;
    match field(&v, "op")? {
        "load" => {
            validate_rules(field(&v, "rules")?)?;
            let mut filters = FilterSet::new(false);
            filters.add_filter_list(field(&v, "rules")?.to_string(), ParseOptions::default());
            let mut candidate = Engine::new_with_filter_set(filters);
            let resources: Vec<Resource> = serde_json::from_value(v["resources"].clone())
                .map_err(|e| format!("resources: {e}"))?;
            install_resources(&mut candidate, resources)?;
            *slot = Some(candidate);
            Ok(json!({"engine":"adblock-rust", "version":"0.13.3"}))
        }
        "clear" => {
            offline::discard();
            *slot = None;
            Ok(json!({}))
        }
        "check" => {
            let engine = slot.as_ref().ok_or("engine unavailable")?;
            let request = adblock::request::Request::preparsed(
                field(&v, "url")?,
                field(&v, "host")?,
                field(&v, "source")?,
                field(&v, "kind")?,
                v["thirdParty"].as_bool().ok_or("missing thirdParty")?,
                field(&v, "method")?,
            );
            let result = engine.check_network_request_subset(&request, false, true);
            Ok(
                json!({"blocked":result.should_block(), "redirect":result.redirect,
                "rewrittenUrl":result.rewritten_url, "important":result.important,
                "exception":result.exception.is_some()}),
            )
        }
        "csp" => {
            let engine = slot.as_ref().ok_or("engine unavailable")?;
            let request = adblock::request::Request::new(
                field(&v, "url")?,
                field(&v, "source")?,
                field(&v, "kind")?,
                field(&v, "method")?,
            )
            .map_err(|e| format!("invalid CSP request: {e:?}"))?;
            Ok(json!({"policy":engine.get_csp_directives(&request)}))
        }
        "capabilities" => Ok(capabilities::report(field(&v, "rules")?)),
        "cosmetic" => {
            let engine = slot.as_ref().ok_or("engine unavailable")?;
            let mut result = serde_json::to_value(engine.url_cosmetic_resources(field(&v, "url")?))
                .map_err(|e| e.to_string())?;
            result["style_policy"] = capabilities::policy();
            Ok(result)
        }
        "generic" => {
            let engine = slot.as_ref().ok_or("engine unavailable")?;
            let tokens = |key: &str| -> Result<Vec<String>, String> {
                let values = v[key].as_array().ok_or("tokens must be arrays")?;
                if values.len() > 256 {
                    return Err("too many cosmetic tokens".into());
                }
                values
                    .iter()
                    .map(|value| {
                        let s = value.as_str().ok_or("token must be a string")?;
                        if s.is_empty() || s.chars().count() > 256 {
                            return Err("invalid cosmetic token length".into());
                        }
                        Ok(s.to_owned())
                    })
                    .collect()
            };
            let classes = tokens("classes")?;
            let ids = tokens("ids")?;
            // Recompute policy from the authoritative URL; never accept exceptions
            // or generichide decisions from a content message.
            let resources = engine.url_cosmetic_resources(field(&v, "url")?);
            let selectors = if resources.generichide {
                vec![]
            } else {
                engine.hidden_class_id_selectors(classes, ids, &resources.exceptions)
            };
            Ok(json!({"selectors":selectors}))
        }
        _ => Err("unsupported operation".into()),
    }
}

/// Caller supplies readable bytes for the duration of the call and frees the
/// returned allocation exactly once with fenn_adblock_free. Null means invalid
/// bounds. Allocation failure follows Rust's abort behavior. ABI v2 is checked
/// before service activation.
#[no_mangle]
pub extern "C" fn fenn_adblock_abi() -> u32 {
    2
}

#[no_mangle]
pub unsafe extern "C" fn fenn_adblock_call(data: *const u8, len: usize) -> *mut c_char {
    if data.is_null() || len > MAX_INPUT {
        return std::ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let v: Value =
            serde_json::from_slice(slice::from_raw_parts(data, len)).map_err(|e| e.to_string())?;
        dispatch(v)
    }));
    let value = match result {
        Ok(Ok(value)) => json!({"ok":true,"value":value}),
        Ok(Err(error)) => json!({"ok":false,"error":error}),
        Err(_) => json!({"ok":false,"error":"engine panic"}),
    };
    let output = value.to_string();
    let output = if output.len() > MAX_OUTPUT {
        r#"{"ok":false,"error":"result exceeds bound"}"#.to_string()
    } else {
        output
    };
    CString::new(output).unwrap().into_raw()
}

#[no_mangle]
pub unsafe extern "C" fn fenn_adblock_free(data: *mut c_char) {
    if !data.is_null() {
        drop(CString::from_raw(data));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    static TEST_LOCK: Mutex<()> = Mutex::new(());
    #[test]
    fn csp_policies_and_exceptions() {
        let _guard = TEST_LOCK.lock().unwrap();
        dispatch(json!({"op":"load","rules":"||policy.example^$csp=script-src 'none'\n||policy.example^$csp=img-src 'none'\n@@||policy.example/exempt^$csp\n@@||policy.example/images^$csp=script-src 'none'","resources":[]})).unwrap();
        let query = |path: &str, kind: &str| {
            dispatch(json!({"op":"csp","url":format!("https://policy.example/{path}"),"source":"https://policy.example/","kind":kind,"method":"get"})).unwrap()["policy"].clone()
        };
        let policy = query("page", "document");
        assert!(policy.as_str().unwrap().contains("script-src 'none'"));
        assert!(policy.as_str().unwrap().contains("img-src 'none'"));
        assert_eq!(query("images", "subdocument"), json!("img-src 'none'"));
        assert!(query("exempt", "document").is_null());
        assert!(query("page", "script").is_null());
    }
    #[test]
    fn generic_exceptions_and_bounds() {
        let _guard = TEST_LOCK.lock().unwrap();
        dispatch(json!({"op":"load","rules":"##.ad\n###banner\nexcept.example#@#.ad\n@@||nohide.example^$generichide", "resources":[]})).unwrap();
        let query = |host: &str| {
            dispatch(json!({"op":"generic","url":format!("https://{host}/"),"classes":["ad"],"ids":["banner"]})).unwrap()["selectors"].as_array().unwrap().clone()
        };
        assert_eq!(query("normal.example").len(), 2);
        assert_eq!(query("except.example"), vec![json!("#banner")]);
        assert!(query("nohide.example").is_empty());
        assert!(dispatch(json!({"op":"generic","url":"https://normal.example/","classes":vec!["ad";257],"ids":[]})).is_err());
        assert!(dispatch(
            json!({"op":"generic","url":"https://normal.example/","classes":[false],"ids":[]})
        )
        .is_err());
    }
    #[test]
    fn inputs_require_resolved_directives_and_valid_unique_resources() {
        assert!(validate_rules("!#if env_firefox\n||ads.example^\n!#endif").is_err());
        assert!(validate_rules("!#include other.txt").is_err());
        assert!(validate_rules("! Ordinary comment\n||ads.example^").is_ok());
        let value = json!({"name":"probe.js","kind":{"mime":"application/javascript"},"content":"Z2xvYmFsVGhpcy5wcm9iZT10cnVlOw=="});
        let mut engine = Engine::default();
        assert!(install_resources(
            &mut engine,
            serde_json::from_value(json!([value, value])).unwrap()
        )
        .is_err());
        let bad = json!({"name":"bad.js","kind":{"mime":"application/javascript"},"content":"not base64!"});
        assert!(
            install_resources(&mut engine, serde_json::from_value(json!([bad])).unwrap()).is_err()
        );
    }
    #[test]
    fn network_exceptions_and_failed_candidate() {
        let _guard = TEST_LOCK.lock().unwrap();
        dispatch(json!({"op":"load","rules":"||ads.example^\n@@||ads.example/allowed.js", "resources":[]})).unwrap();
        let check = |path| {
            dispatch(json!({"op":"check","url":format!("https://ads.example/{path}"),"host":"ads.example","source":"publisher.example","kind":"script","method":"GET","thirdParty":true})).unwrap()
        };
        assert_eq!(check("ad.js")["blocked"], true);
        assert_eq!(check("allowed.js")["blocked"], false);
        assert!(dispatch(json!({"op":"load","rules":"", "resources":false})).is_err());
        assert_eq!(check("ad.js")["blocked"], true);
        dispatch(json!({"op":"clear"})).unwrap();
        assert!(dispatch(json!({"op":"cosmetic","url":"https://publisher.example"})).is_err());
    }
    #[test]
    fn snapshot_identity_corruption_resources_and_async_discard() {
        let _guard = TEST_LOCK.lock().unwrap();
        let profile =
            std::env::temp_dir().join(format!("fenn-adblock-cache-test-{}", std::process::id()));
        fs::create_dir_all(&profile).unwrap();
        let config = profile.join("fenn-adblock-prototype.json");
        let cache = profile.join("fenn-adblock-prototype.cache");
        let _ = fs::remove_file(&cache);
        let value = json!({"version":1,"rules":"||ads.example^\npublisher.example##+js(probe)",
            "resources":[{"name":"probe.js","kind":{"mime":"application/javascript"},"content":"Z2xvYmFsVGhpcy5wcm9iZT10cnVlOw=="}]});
        fs::write(&config, value.to_string()).unwrap();
        let (cold, cold_stats) = prepare(&profile, false, 0).unwrap();
        assert_eq!(cold_stats["cacheHit"], false);
        let (warm, warm_stats) = prepare(&profile, false, 0).unwrap();
        assert_eq!(
            warm_stats["cacheHit"], true,
            "cold={cold_stats} warm={warm_stats}"
        );
        assert_eq!(
            cold.url_cosmetic_resources("https://publisher.example")
                .injected_script,
            warm.url_cosmetic_resources("https://publisher.example")
                .injected_script
        );
        assert!(warm
            .url_cosmetic_resources("https://publisher.example")
            .injected_script
            .contains("globalThis.probe"));
        let mut bytes = fs::read(&cache).unwrap();
        *bytes.last_mut().unwrap() ^= 0xff;
        fs::write(&cache, bytes).unwrap();
        assert_eq!(prepare(&profile, false, 0).unwrap().1["cacheHit"], false);
        fs::write(&config, value.to_string() + " ").unwrap();
        assert_eq!(prepare(&profile, false, 0).unwrap().1["cacheHit"], false);
        // A canceled candidate is never published. Existing matching is not
        // blocked behind worker construction or disk writes.
        dispatch(json!({"op":"load","rules":"||existing.example^","resources":[]})).unwrap();
        dispatch(json!({"op":"begin","profile":profile,"delayMs":100})).unwrap();
        assert!(dispatch(json!({"op":"begin","profile":profile})).is_err());
        assert_eq!(
            dispatch(json!({"op":"poll","accept":false})).unwrap()["done"],
            false
        );
        let request = json!({"op":"check","url":"https://existing.example/ad.js","host":"existing.example","source":"publisher.example","kind":"script","method":"GET","thirdParty":true});
        assert_eq!(dispatch(request.clone()).unwrap()["blocked"], true);
        loop {
            let result = dispatch(json!({"op":"poll","accept":false})).unwrap();
            if result["done"] == true {
                assert_eq!(result["accepted"], false);
                break;
            }
            thread::sleep(std::time::Duration::from_millis(5));
        }
        assert_eq!(dispatch(request).unwrap()["blocked"], true);
        dispatch(json!({"op":"shutdown"})).unwrap();
        fs::remove_dir_all(profile).unwrap();
    }
    #[test]
    fn ffi_rejects_bounds_and_invalid_json() {
        unsafe {
            assert!(fenn_adblock_call(std::ptr::null(), 0).is_null());
            assert!(fenn_adblock_call(b"x".as_ptr(), MAX_INPUT + 1).is_null());
            let p = fenn_adblock_call(b"x".as_ptr(), 1);
            assert!(!p.is_null());
            let value: Value =
                serde_json::from_slice(std::ffi::CStr::from_ptr(p).to_bytes()).unwrap();
            assert_eq!(value["ok"], false);
            fenn_adblock_free(p);
        }
    }
}
