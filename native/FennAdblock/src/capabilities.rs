//! Shared publisher/runtime reporting; never use parser acceptance as effectiveness.
use adblock::filters::cosmetic::{CosmeticFilterAction, CosmeticFilterOperator};
use adblock::lists::{parse_filter, ParseOptions, ParsedLine};
use serde_json::{json, Value};
use std::collections::BTreeMap;

pub fn policy() -> Value {
    serde_json::from_str(include_str!("../../../configs/adblock/capabilities.json")).unwrap()
}

pub fn report(rules: &str) -> Value {
    let contract = policy();
    let properties = contract["style_properties"].as_array().unwrap();
    let mut categories: BTreeMap<&str, Value> = BTreeMap::new();
    let mut accepted = 0;
    let mut rejected = 0;
    for (index, line) in rules.lines().enumerate() {
        if line.trim().is_empty() || line.starts_with('!') || line.starts_with('[') {
            continue;
        }
        let mut labels = Vec::new();
        match parse_filter(line, false, ParseOptions::default()) {
            Err(_) => {
                rejected += 1;
                labels.push("parser-rejected");
            }
            Ok(parsed) => {
                accepted += 1;
                match parsed {
                    ParsedLine::Cosmetic(c) => {
                        if c.selector.iter().any(|op| matches!(op, CosmeticFilterOperator::CssSelector(css) if css.contains(":has-text("))) {
                            labels.push("nested-text-subset-needs-gecko-validation");
                        }
                        match c.action {
                            Some(CosmeticFilterAction::Style(style)) => {
                                labels.push("style-needs-gecko-validation");
                                if style.trim().trim_end_matches(';').split(';').any(
                                    |declaration| {
                                        declaration
                                            .split_once(':')
                                            .map(|(name, _)| {
                                                !properties.iter().any(|p| {
                                                    p.as_str()
                                                        == Some(
                                                            name.trim()
                                                                .to_ascii_lowercase()
                                                                .as_str(),
                                                        )
                                                })
                                            })
                                            .unwrap_or(true)
                                    },
                                ) {
                                    labels.push("style-property-restricted");
                                }
                            }
                            Some(_) => labels.push("dom-action-bounded-gecko-validation"),
                            None => labels.push("cosmetic-candidate-not-effectiveness"),
                        }
                        if c.selector
                            .iter()
                            .any(|op| !matches!(op, CosmeticFilterOperator::CssSelector(_)))
                        {
                            if c.selector.iter().all(|op| {
                                matches!(
                                    op,
                                    CosmeticFilterOperator::CssSelector(_)
                                        | CosmeticFilterOperator::HasText(_)
                                        | CosmeticFilterOperator::Upward(_)
                                        | CosmeticFilterOperator::MatchesAttr(_)
                                        | CosmeticFilterOperator::MatchesPath(_)
                                        | CosmeticFilterOperator::MatchesCss(_)
                                        | CosmeticFilterOperator::MatchesCssBefore(_)
                                        | CosmeticFilterOperator::MatchesCssAfter(_)
                                )
                            }) {
                                labels.push("procedural-bounded-gecko-validation");
                                if c.selector.iter().any(|op| matches!(op, CosmeticFilterOperator::HasText(arg) if arg.starts_with('/'))) {
                                    labels.push("procedural-regex-subset-needs-gecko-validation");
                                }
                            } else {
                                labels.push("procedural-not-integrated");
                            }
                        }
                    }
                    ParsedLine::Network(_) => {
                        labels.push("network-candidate-not-effectiveness");
                        if let Some((_, options)) = line.rsplit_once('$') {
                            for option in options.split(',') {
                                let name = option
                                    .split('=')
                                    .next()
                                    .unwrap_or("")
                                    .trim_start_matches('~');
                                match name {
                                    "websocket" | "object" => {
                                        labels.push("request-type-mapped-gecko-validation")
                                    }
                                    "document" | "doc" => {
                                        labels.push("document-blocking-restricted")
                                    }
                                    "redirect" | "redirect-rule" => {
                                        labels.push("redirect-context-restricted")
                                    }
                                    "removeparam" => labels.push("rewrite-context-restricted"),
                                    "csp" => labels.push("csp-policy-restricted"),
                                    "tag" => labels.push("tags-not-integrated"),
                                    _ => {}
                                }
                            }
                        }
                    }
                }
            }
        }
        labels.sort_unstable();
        labels.dedup();
        for label in labels {
            let entry = categories
                .entry(label)
                .or_insert_with(|| json!({"count":0,"examples":[]}));
            entry["count"] = json!(entry["count"].as_u64().unwrap() + 1);
            let examples = entry["examples"].as_array_mut().unwrap();
            if examples.len() < 5 {
                examples.push(
                    json!({"line":index+1,"rule":line.chars().take(512).collect::<String>()}),
                );
            }
        }
    }
    json!({"contract":policy(),"parser_accepted":accepted,"parser_rejected":rejected,"categories":categories})
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn acceptance_is_not_execution() {
        let r = report("a.example##.ad:style(display:none)\na.example##.ad:remove()\na.example##.ad:has-text(ad)\n/x$websocket\n@@/x$document\na.example##^script\n! comment");
        assert_eq!(r["parser_accepted"], 5);
        assert_eq!(r["parser_rejected"], 1);
        for name in [
            "style-needs-gecko-validation",
            "dom-action-bounded-gecko-validation",
            "procedural-bounded-gecko-validation",
            "request-type-mapped-gecko-validation",
            "document-blocking-restricted",
        ] {
            assert_eq!(r["categories"][name]["count"], 1);
        }
    }
}
