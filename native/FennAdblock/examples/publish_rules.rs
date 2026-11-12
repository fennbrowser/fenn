//! Deterministic publisher input, using the same parser as runtime acceptance.
use adblock::{
    lists::{parse_filter, ParseOptions},
    resources::{InMemoryResourceStorage, Resource},
};
use serde_json::{json, Value};
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut config: Value = serde_json::from_slice(&std::fs::read(&args[1]).unwrap()).unwrap();
    assert_eq!(config["version"], 1);
    let capabilities = fenn_adblock::capabilities::report(config["rules"].as_str().unwrap());
    let mut rules = String::new();
    let mut excluded = Vec::new();
    for (index, line) in config["rules"].as_str().unwrap().lines().enumerate() {
        assert!(!line.trim_start().starts_with("!#"), "unresolved directive");
        if !line.trim().is_empty() && !line.starts_with('!') && !line.starts_with('[') {
            if let Err(error) = parse_filter(line, false, ParseOptions::default()) {
                excluded.push(json!({"line":index+1,"rule":line,"reason":error.to_string()}));
                continue;
            }
        }
        rules.push_str(line);
        rules.push('\n');
    }
    let mut storage = InMemoryResourceStorage::default();
    for value in config["resources"].as_array().unwrap() {
        let resource: Resource = serde_json::from_value(value.clone()).unwrap();
        storage
            .add_resource(resource)
            .expect("invalid or duplicate resource");
    }
    config["rules"] = json!(rules);
    std::fs::write(
        &args[2],
        serde_json::to_vec(
            &json!({"configuration":config,"excluded":excluded,"capabilities":capabilities}),
        )
        .unwrap(),
    )
    .unwrap();
}
