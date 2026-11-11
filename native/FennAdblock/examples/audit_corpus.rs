//! Offline acceptance inventory. Parsing success is not browser feature coverage.
use adblock::{
    lists::{parse_filter, ParseOptions, ParsedLine},
    resources::{InMemoryResourceStorage, Resource},
};
use serde_json::{json, Value};
use std::collections::BTreeMap;
fn main() {
    let path = std::env::args().nth(1).expect("configuration path");
    let input: Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
    let mut counts = BTreeMap::<String, usize>::new();
    let mut samples = BTreeMap::<String, Vec<String>>::new();
    for line in input["rules"].as_str().unwrap().lines() {
        if line.trim().is_empty() || line.starts_with('!') || line.starts_with('[') {
            continue;
        }
        let category = match parse_filter(line, false, ParseOptions::default()) {
            Ok(ParsedLine::Network(_)) => "network".to_string(),
            Ok(ParsedLine::Cosmetic(filter)) => {
                if line.contains("##+js(") || line.contains("#@#+js(") {
                    "scriptlet"
                } else if filter.plain_css_selector().is_none() {
                    "non_plain_cosmetic"
                } else if !filter.has_hostname_constraint() {
                    "generic_css"
                } else {
                    "domain_css"
                }
                .to_string()
            }
            Err(error) => format!("rejected: {error}"),
        };
        *counts.entry(category.clone()).or_default() += 1;
        let items = samples.entry(category).or_default();
        if items.len() < 3 {
            items.push(line.into());
        }
    }
    let mut storage = InMemoryResourceStorage::default();
    let mut rejected = Vec::new();
    let mut accepted = 0;
    for value in input["resources"].as_array().unwrap() {
        let resource: Resource = serde_json::from_value(value.clone()).unwrap();
        if let Err(error) = storage.add_resource(resource) {
            rejected.push(json!({"name":value["name"],"error":format!("{error:?}")}));
        } else {
            accepted += 1;
        }
    }
    println!("{}",serde_json::to_string_pretty(&json!({"counts":counts,"samples":samples,"accepted_resources":accepted,"rejected_resources":rejected})).unwrap());
}
