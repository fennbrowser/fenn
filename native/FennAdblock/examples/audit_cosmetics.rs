//! Structured inventory for Gecko-side capability auditing; no engine state mutation.
use adblock::lists::{parse_filter, ParseOptions, ParsedLine};
use serde_json::{json, Value};
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let config: Value = serde_json::from_slice(&std::fs::read(&args[1]).unwrap()).unwrap();
    let mut cosmetics = Vec::new();
    let mut rejected = Vec::new();
    let mut network = 0;
    for (i, line) in config["rules"].as_str().unwrap().lines().enumerate() {
        if line.trim().is_empty() || line.starts_with('!') || line.starts_with('[') { continue; }
        match parse_filter(line, false, ParseOptions::default()) {
            Ok(ParsedLine::Network(_)) => network += 1,
            Ok(ParsedLine::Cosmetic(c)) => cosmetics.push(json!({"line":i+1,"rule":line,"selector":c.selector,"action":c.action,"scriptlet":c.mask.contains(adblock::filters::cosmetic::CosmeticFilterMask::SCRIPT_INJECT),"exception":c.mask.contains(adblock::filters::cosmetic::CosmeticFilterMask::UNHIDE)})),
            Err(e) => rejected.push(json!({"line":i+1,"rule":line,"reason":e.to_string()})),
        }
    }
    std::fs::write(&args[2],serde_json::to_vec(&json!({"network":network,"cosmetics":cosmetics,"rejected":rejected})).unwrap()).unwrap();
}
