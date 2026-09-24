//! Resolve hits against the accepted layout without inventing tab IDs for headers.
use crate::strip::ShellStripSlot;

pub fn target(layout: &[ShellStripSlot], source: u32, tab: u32, header: u32) -> u32 {
    if source == 0 || source == tab || (tab == 0) == (header == 0) { return 0; }
    let Some(from) = layout.iter().find(|s| s.tab == source) else { return 0; };
    let group = if header != 0 {
        layout.iter().find(|s| s.tab == 0 && s.group == header).map(|s| s.group)
    } else {
        layout.iter().find(|s| s.tab == tab).map(|s| s.group)
    }.unwrap_or(0);
    if group == from.group { 0 } else { group }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn members_headers_and_empty_groups_are_distinct_targets() {
        let layout = [ShellStripSlot{group:0,tab:1}, ShellStripSlot{group:1,tab:0},
            ShellStripSlot{group:1,tab:2}, ShellStripSlot{group:9,tab:0}];
        assert_eq!(target(&layout,1,2,0),1);
        assert_eq!(target(&layout,1,0,1),1);
        assert_eq!(target(&layout,1,0,9),9);
        assert_eq!(target(&layout,2,0,9),9);
        for (source, tab, header) in [(2,2,0),(2,0,1),(1,1,0),(2,1,0),
            (0,2,0),(99,2,0),(1,99,0),(1,0,99),(1,2,1),(1,0,0)] {
            assert_eq!(target(&layout,source,tab,header),0);
        }
    }
}
