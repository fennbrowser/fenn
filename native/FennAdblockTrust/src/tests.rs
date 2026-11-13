use super::*;
use aws_lc_rs::signature::{Ed25519KeyPair, KeyPair};
use serde_json::{json, Value};
struct Key {
    pair: Ed25519KeyPair,
    public: Value,
    id: String,
}
fn hex(b: &[u8]) -> String {
    b.iter().map(|b| format!("{b:02x}")).collect()
}
impl Key {
    fn new() -> Self {
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&aws_lc_rs::rand::SystemRandom::new()).unwrap();
        let pair = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
        let public = json!({"keytype":"ed25519","scheme":"ed25519","keyval":{"public":hex(pair.public_key().as_ref())}});
        let id = hex_hash(&serde_json::to_vec(&public).unwrap());
        Self { pair, public, id }
    }
}
fn signed(value: Value, keys: &[&Key]) -> Vec<u8> {
    // Fixture metadata contains only ASCII strings and integers: sorted compact
    // serde JSON equals TUF canonical JSON for this deliberately limited input.
    let raw = serde_json::to_vec(&value).unwrap();
    serde_json::to_vec(&json!({"signed":value,"signatures":keys.iter().map(|k|json!({"keyid":k.id,"sig":hex(k.pair.sign(&raw).as_ref())})).collect::<Vec<_>>()})).unwrap()
}
fn root(key: &Key, version: u64, signers: &[&Key]) -> Vec<u8> {
    let role = json!({"keyids":[key.id],"threshold":1});
    signed(
        json!({"_type":"root","spec_version":"1.0.0","version":version,"expires":"2100-01-01T00:00:00Z","consistent_snapshot":false,
        "keys":{key.id.clone():key.public},"roles":{"root":role,"targets":role,"snapshot":role,"timestamp":role}}),
        signers,
    )
}
struct Fixture {
    temp: tempfile::TempDir,
    key: Key,
}
impl Fixture {
    fn new() -> Self {
        let f = Self {
            temp: tempfile::tempdir().unwrap(),
            key: Key::new(),
        };
        fs::create_dir_all(f.path("repo/metadata")).unwrap();
        fs::create_dir(f.path("repo/targets")).unwrap();
        fs::write(f.path("anchor.json"), root(&f.key, 1, &[&f.key])).unwrap();
        for (name, _) in TARGETS {
            fs::write(f.path(&format!("repo/targets/{name}")), b"fixture").unwrap();
        }
        f.publish(&f.key, 1, None);
        f
    }
    fn path(&self, p: &str) -> PathBuf {
        self.temp.path().join(p)
    }
    fn record(&self) -> PathBuf {
        let anchor = fs::read(self.path("anchor.json")).unwrap();
        self.path(&format!("state/anchor-{}", hex_hash(&anchor)))
    }
    fn write(&self, name: &str, data: &[u8]) {
        fs::write(self.path(&format!("repo/metadata/{name}.json")), data).unwrap();
    }
    fn publish(&self, key: &Key, version: u64, expired: Option<&str>) {
        let expiry = |role| {
            if expired == Some(role) {
                "2000-01-01T00:00:00Z"
            } else {
                "2100-01-01T00:00:00Z"
            }
        };
        let entries = TARGETS
            .iter()
            .map(|(name, _)| {
                let bytes = fs::read(self.path(&format!("repo/targets/{name}"))).unwrap();
                (
                    name.to_string(),
                    json!({"length":bytes.len(),"hashes":{"sha256":hex_hash(&bytes)}}),
                )
            })
            .collect::<std::collections::BTreeMap<_, _>>();
        let targets = signed(
            json!({"_type":"targets","spec_version":"1.0.0","version":version,"expires":expiry("targets"),"targets":entries}),
            &[key],
        );
        let meta = |data: &[u8]| json!({"version":version,"length":data.len(),"hashes":{"sha256":hex_hash(data)}});
        let snapshot = signed(
            json!({"_type":"snapshot","spec_version":"1.0.0","version":version,"expires":expiry("snapshot"),"meta":{"targets.json":meta(&targets)}}),
            &[key],
        );
        let timestamp = signed(
            json!({"_type":"timestamp","spec_version":"1.0.0","version":version,"expires":expiry("timestamp"),"meta":{"snapshot.json":meta(&snapshot)}}),
            &[key],
        );
        self.write("targets", &targets);
        self.write("snapshot", &snapshot);
        self.write("timestamp", &timestamp);
    }
    fn run(&self, output: &str) -> Result<Value> {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
            .block_on(verify(
                &self.path("anchor.json"),
                &self.path("repo"),
                &self.path("state"),
                &self.path(output),
            ))
    }
    fn rejects(&self, output: &str) {
        let error = self.run(output).expect_err("candidate must reject");
        assert!(!self.path(output).exists());
        println!("expected rejection: {error}");
    }
}
#[test]
fn valid_and_repeat_verify_exact_bytes() {
    let f = Fixture::new();
    let receipt = f.run("accepted").unwrap();
    assert_eq!(receipt["root_version"], 1);
    for (name, _) in TARGETS {
        assert_eq!(
            fs::read(f.path(&format!("accepted/{name}"))).unwrap(),
            b"fixture"
        );
    }
    f.run("repeat").unwrap();
    assert!(f.run("accepted").is_err());
}
#[test]
fn expired_roles_reject() {
    for role in ["timestamp", "snapshot", "targets"] {
        let f = Fixture::new();
        f.publish(&f.key, 1, Some(role));
        f.rejects("rejected");
    }
    let f = Fixture::new();
    let mut r: Value = serde_json::from_slice(&root(&f.key, 1, &[&f.key])).unwrap();
    r["signed"]["expires"] = json!("2000-01-01T00:00:00Z");
    fs::write(
        f.path("anchor.json"),
        signed(r["signed"].clone(), &[&f.key]),
    )
    .unwrap();
    f.rejects("expired-root");
}
#[test]
fn unknown_signer_and_tampered_metadata_reject() {
    let f = Fixture::new();
    f.publish(&Key::new(), 1, None);
    f.rejects("unknown-key");
    let f = Fixture::new();
    let path = f.path("repo/metadata/timestamp.json");
    let mut value: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    value["signed"]["version"] = json!(2);
    fs::write(path, serde_json::to_vec(&value).unwrap()).unwrap();
    f.rejects("tampered");
}
#[test]
fn changed_target_never_publishes_partial_output() {
    for name in ["descriptor.json", "rules.txt", "provenance.json"] {
        let f = Fixture::new();
        fs::write(f.path(&format!("repo/targets/{name}")), b"altered").unwrap();
        f.rejects("bad-target");
    }
}
#[test]
fn persisted_timestamp_rollback_rejects() {
    let f = Fixture::new();
    f.publish(&f.key, 2, None);
    f.run("newer").unwrap();
    f.publish(&f.key, 1, None);
    f.rejects("rollback");
}
#[test]
fn root_rotation_requires_old_and_new_authority() {
    for (old, new) in [(true, false), (false, true), (true, true)] {
        let f = Fixture::new();
        let next = Key::new();
        let mut signers = Vec::new();
        if old {
            signers.push(&f.key);
        }
        if new {
            signers.push(&next);
        }
        f.write("2.root", &root(&next, 2, &signers));
        f.publish(&next, 2, None);
        if old && new {
            assert_eq!(f.run("rotated").unwrap()["root_version"], 2);
            // Do not require the remote root chain again after persisting rotation.
            fs::remove_file(f.path("repo/metadata/2.root.json")).unwrap();
            f.run("restart").unwrap();
            f.publish(&f.key, 3, None);
            f.rejects("retired-key");
        } else {
            f.rejects("invalid-rotation");
        }
    }
}
#[test]
fn missing_state_and_clock_rollback_fail_closed() {
    for name in [
        "root.json",
        "timestamp.json",
        "snapshot.json",
        "targets.json",
        "latest_known_time.json",
    ] {
        let f = Fixture::new();
        f.run("valid").unwrap();
        let path = f.record().join("state.json");
        let mut record: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        record["payload"]["files"]
            .as_object_mut()
            .unwrap()
            .remove(name);
        fs::write(path, serde_json::to_vec(&record).unwrap()).unwrap();
        f.rejects("missing-state");
    }
    let f = Fixture::new();
    f.run("valid").unwrap();
    let tx =
        state::Transaction::begin(&f.record(), &fs::read(f.path("anchor.json")).unwrap()).unwrap();
    fs::write(
        tx.work.path().join("latest_known_time.json"),
        b"\"2099-01-01T00:00:00Z\"",
    )
    .unwrap();
    tx.commit().unwrap();
    drop(tx);
    f.rejects("clock-rollback");
}
#[test]
fn bounds_and_single_writer_reject() {
    let f = Fixture::new();
    fs::write(
        f.path("repo/metadata/timestamp.json"),
        vec![b' '; META as usize + 1],
    )
    .unwrap();
    f.rejects("huge-metadata");
    let f = Fixture::new();
    fs::write(f.path("repo/targets/descriptor.json"), vec![b'x'; 65537]).unwrap();
    f.publish(&f.key, 1, None);
    f.rejects("huge-descriptor");
    let f = Fixture::new();
    let _tx =
        state::Transaction::begin(&f.record(), &fs::read(f.path("anchor.json")).unwrap()).unwrap();
    f.rejects("busy");
}
#[test]
fn changed_bootstrap_keeps_each_anchor_record() {
    let f = Fixture::new();
    f.run("valid").unwrap();
    let old_anchor = fs::read(f.path("anchor.json")).unwrap();
    let old_record = f.record().join("state.json");
    let old_bytes = fs::read(&old_record).unwrap();
    // The repository rotates its root and a later browser ships that root.
    let next = Key::new();
    let new_anchor = root(&next, 2, &[&f.key, &next]);
    f.write("2.root", &new_anchor);
    f.publish(&next, 2, None);
    fs::write(f.path("anchor.json"), &new_anchor).unwrap();
    assert_eq!(f.run("new-anchor").unwrap()["root_version"], 2);
    assert_eq!(fs::read(&old_record).unwrap(), old_bytes);
    assert!(f.record().join("state.json").exists());
    // An unrelated root cannot adopt the repository or alter either record.
    let other = Key::new();
    fs::write(f.path("anchor.json"), root(&other, 1, &[&other])).unwrap();
    f.rejects("unrelated-anchor");
    assert_eq!(fs::read(&old_record).unwrap(), old_bytes);
    // A downgrade resumes the original anchor's own record.
    fs::write(f.path("anchor.json"), &old_anchor).unwrap();
    assert_eq!(f.run("downgrade").unwrap()["root_version"], 2);
}
#[test]
fn legacy_record_is_adopted_with_its_rollback_history() {
    let f = Fixture::new();
    f.publish(&f.key, 2, None);
    f.run("newer").unwrap();
    // Recreate the former single-anchor layout.
    let legacy = f.path("state/state.json");
    fs::rename(f.record().join("state.json"), &legacy).unwrap();
    fs::remove_dir_all(f.record()).unwrap();
    let legacy_bytes = fs::read(&legacy).unwrap();
    f.publish(&f.key, 1, None);
    f.rejects("legacy-rollback");
    assert!(f.record().join("state.json").exists());
    assert_eq!(fs::read(&legacy).unwrap(), legacy_bytes);
    // Without adoption the same rollback would bootstrap and be accepted.
    let fresh = Fixture::new();
    fresh.publish(&fresh.key, 1, None);
    fresh.run("fresh").unwrap();
}
#[test]
fn legacy_record_corruption_fails_closed_and_other_anchor_is_left_alone() {
    let f = Fixture::new();
    fs::create_dir(f.path("state")).unwrap();
    fs::write(f.path("state/state.json"), b"truncated").unwrap();
    f.rejects("corrupt-legacy");
    assert!(!f.record().exists());
    let f = Fixture::new();
    f.run("valid").unwrap();
    fs::rename(f.record().join("state.json"), f.path("state/state.json")).unwrap();
    fs::remove_dir_all(f.record()).unwrap();
    let legacy_bytes = fs::read(f.path("state/state.json")).unwrap();
    let next = Key::new();
    let new_anchor = root(&next, 2, &[&f.key, &next]);
    f.write("2.root", &new_anchor);
    f.publish(&next, 2, None);
    fs::write(f.path("anchor.json"), &new_anchor).unwrap();
    assert_eq!(f.run("new-anchor").unwrap()["root_version"], 2);
    assert_eq!(fs::read(f.path("state/state.json")).unwrap(), legacy_bytes);
}
#[test]
fn anchor_records_are_bounded_but_existing_records_stay_usable() {
    let f = Fixture::new();
    f.run("valid").unwrap();
    for n in 0..crate::MAX_ANCHORS - 1 {
        fs::create_dir(f.path(&format!("state/anchor-{n:064x}"))).unwrap();
    }
    f.run("existing").unwrap();
    let other = Key::new();
    fs::write(f.path("anchor.json"), root(&other, 1, &[&other])).unwrap();
    let error = f.run("over-bound").unwrap_err().to_string();
    assert!(error.contains("anchor record bound"), "{error}");
    assert!(!f.record().exists());
}
#[test]
fn rejected_refresh_preserves_verified_root_rotation() {
    let f = Fixture::new();
    let next = Key::new();
    f.write("2.root", &root(&next, 2, &[&f.key, &next]));
    // New root is valid; subsequent timestamp is signed by the retired key.
    f.publish(&f.key, 2, None);
    f.rejects("retired-timestamp");
    fs::remove_file(f.path("repo/metadata/2.root.json")).unwrap();
    f.publish(&next, 2, None);
    assert_eq!(f.run("recovered").unwrap()["root_version"], 2);
}
#[test]
fn metadata_hash_and_snapshot_rollback_reject() {
    let f = Fixture::new();
    let path = f.path("repo/metadata/targets.json");
    let mut bytes = fs::read(&path).unwrap();
    bytes.push(b' ');
    fs::write(path, bytes).unwrap();
    f.rejects("metadata-length");
    let f = Fixture::new();
    f.publish(&f.key, 2, None);
    f.run("newer").unwrap();
    f.publish(&f.key, 1, None);
    let p = f.path("repo/metadata/timestamp.json");
    let mut v: Value = serde_json::from_slice(&fs::read(&p).unwrap()).unwrap();
    v["signed"]["version"] = json!(3);
    fs::write(p, signed(v["signed"].clone(), &[&f.key])).unwrap();
    f.rejects("snapshot-rollback");
}
#[test]
fn skipped_root_and_duplicate_signatures_cannot_rotate() {
    let f = Fixture::new();
    let next = Key::new();
    f.write("2.root", &root(&next, 3, &[&f.key, &next]));
    f.publish(&next, 3, None);
    f.rejects("skipped-root");
    let f = Fixture::new();
    let mut value: Value = serde_json::from_slice(&root(&next, 2, &[&f.key, &next])).unwrap();
    value["signed"]["roles"]["root"]["threshold"] = json!(2);
    f.write(
        "2.root",
        &signed(value["signed"].clone(), &[&f.key, &next, &next]),
    );
    f.publish(&next, 2, None);
    f.rejects("duplicate-signature");
}
#[test]
fn filesystem_transport_rejects_network_and_outside_paths() {
    let f = Fixture::new();
    let transport = Local {
        metadata: f.path("repo/metadata").canonicalize().unwrap(),
        targets: f.path("repo/targets").canonicalize().unwrap(),
        budget: Arc::new(Mutex::new((0, 0))),
    };
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    for url in [
        Url::parse("https://example.invalid/timestamp.json").unwrap(),
        Url::from_file_path(f.path("anchor.json")).unwrap(),
    ] {
        assert!(runtime.block_on(transport.fetch(url)).is_err());
    }
}
// Explicit test-only bridge to a generated real corpus. No private key is saved.
#[test]
#[ignore = "requires FENN_TRUST_TEST_BUNDLE and FENN_TRUST_TEST_OUTPUT"]
fn published_bundle_round_trip() {
    let input = PathBuf::from(std::env::var_os("FENN_TRUST_TEST_BUNDLE").expect("fixture bundle"));
    let output =
        PathBuf::from(std::env::var_os("FENN_TRUST_TEST_OUTPUT").expect("new fixture output"));
    let f = Fixture::new();
    for (name, _) in TARGETS {
        fs::copy(input.join(name), f.path(&format!("repo/targets/{name}"))).unwrap();
    }
    f.publish(&f.key, 1, None);
    f.run("verified").unwrap();
    fs::create_dir(&output).unwrap();
    fs::create_dir(output.join("bundle")).unwrap();
    for (name, _) in TARGETS {
        assert_eq!(
            fs::read(input.join(name)).unwrap(),
            fs::read(f.path(&format!("verified/{name}"))).unwrap()
        );
        fs::copy(
            f.path(&format!("verified/{name}")),
            output.join("bundle").join(name),
        )
        .unwrap();
    }
    fs::copy(
        f.path("verified/verification.json"),
        output.join("verification.json"),
    )
    .unwrap();
    fs::copy(f.path("anchor.json"), output.join("test-root.json")).unwrap();
    for folder in ["metadata", "targets"] {
        fs::create_dir_all(output.join("repository").join(folder)).unwrap();
        for file in fs::read_dir(f.path(&format!("repo/{folder}"))).unwrap() {
            let file = file.unwrap();
            fs::copy(
                file.path(),
                output
                    .join("repository")
                    .join(folder)
                    .join(file.file_name()),
            )
            .unwrap();
        }
    }
}
