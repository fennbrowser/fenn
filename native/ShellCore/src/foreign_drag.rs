//! Destination preview and durable transfer bookkeeping. Native code retains
//! control objects and resolves weak peers; no HWNDs or callbacks live here.
use std::collections::{BTreeMap, BTreeSet};
pub const MAX_ROWS: usize = 10_000;
pub const MAX_TRANSFERS: usize = 256;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ShellForeignRow { pub id: u32, pub left: f64, pub width: f64 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellForeignPreview {
    pub active: u32,
    pub projected: u32,
    pub width: f64,
    pub request: u64,
}
#[derive(Default)]
pub struct ForeignPreview { state: ShellForeignPreview, rows: Vec<ShellForeignRow> }
fn coordinate(v: f64) -> bool { v.is_finite() && v.abs() <= 1_000_000_000.0 }
fn valid_rows(rows: &[ShellForeignRow]) -> bool {
    rows.len() <= MAX_ROWS && rows.iter().all(|r| r.id!=0 && coordinate(r.left) && coordinate(r.width) && r.width>=0.0)
        && rows.iter().map(|r|r.id).collect::<BTreeSet<_>>().len()==rows.len()
}
impl ForeignPreview {
    pub fn state(&self) -> ShellForeignPreview { self.state }
    pub fn begin(&mut self, rows: &[ShellForeignRow]) -> bool {
        if !valid_rows(rows) { return false; }
        self.rows=rows.to_vec();
        self.state=ShellForeignPreview{active:1,projected:u32::MAX,width:rows.first().map_or(0.0,|r|r.width),request:0};
        true
    }
    pub fn hover(&mut self, x: f64) -> Option<bool> {
        if !coordinate(x) || self.state.active==0 || self.state.request!=0 { return None; }
        let mut projected=0;
        // Preserve the existing uniform-gap preview's first-row-width rule.
        for (i,row) in self.rows.iter().enumerate() { if row.left+self.state.width/2.0<x {projected=i as u32+1;} }
        let changed=self.state.projected!=projected; self.state.projected=projected;Some(changed)
    }
    pub fn end(&mut self, force: bool) -> bool {
        if self.state.active==0 || (self.state.request!=0 && !force) {return false;}
        *self=Default::default();true
    }
    pub fn bind(&mut self, request: u64) {
        if request==0 {self.end(true);} else if self.state.active!=0 {self.state.request=request;}
    }
    pub fn complete(&mut self, request: u64, accepted: bool) {
        if self.state.request==request && !accepted {self.end(true);}
    }
    pub fn layout(&mut self, ids: &[u32]) -> bool {
        if self.state.active==0 || self.rows.iter().map(|r|r.id).eq(ids.iter().copied()) {return false;}
        let refresh=self.state.request==0; self.end(true);refresh
    }
    pub fn external_index(rows: &[ShellForeignRow], x: f64) -> Option<u32> {
        if !coordinate(x) || !valid_rows(rows) {return None;}
        let mut index=0;
        // Whole-window merge retains per-row widths, unlike the preview gap.
        for (i,row) in rows.iter().enumerate() {if row.left+row.width/2.0<x {index=i as u32+1;}}
        Some(index)
    }
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellTransfer {
    pub ticket: u64,
    pub source_endpoint: u64,
    pub source_token: u64,
    pub request: u64,
    pub tab: u32,
    /// 0 pending, 1 accepted (wait for publication), 2 rejected/cancelled.
    pub result: u32,
}
#[derive(Default)]
pub struct Transfers { next: u64, records: BTreeMap<u64,ShellTransfer> }
impl Transfers {
    pub fn can_reserve(&self) -> bool {self.records.len()<MAX_TRANSFERS && self.next<u64::MAX}
    pub fn reserve(&mut self, endpoint:u64, token:u64, tab:u32) -> ShellTransfer {
        assert!(self.can_reserve());self.next+=1;
        let value=ShellTransfer{ticket:self.next,source_endpoint:endpoint,source_token:token,tab,..Default::default()};
        self.records.insert(value.ticket,value);value
    }
    pub fn bind(&mut self,ticket:u64,request:u64) -> bool {
        if request!=0 && self.records.values().any(|r|r.request==request) {return false;}
        let Some(r)=self.records.get_mut(&ticket) else {return false;};
        if r.request!=0 || r.result!=0 {return false;}
        r.request=request;if request==0 {r.result=2;}true
    }
    pub fn complete(&mut self,request:u64,accepted:bool) {
        for r in self.records.values_mut() {if r.request==request && r.result==0 {r.result=if accepted {1} else {2};}}
    }
    pub fn cancel(&mut self) {for r in self.records.values_mut() {if r.result==0 {r.result=2;}}}
    pub fn next_effect(&self,after:u64) -> ShellTransfer {self.records.range((std::ops::Bound::Excluded(after),std::ops::Bound::Unbounded)).map(|(_,r)|r).find(|r|r.result!=0).copied().unwrap_or_default()}
    pub fn ack(&mut self,ticket:u64) -> bool {
        if self.records.get(&ticket).is_some_and(|r|r.result!=0) {self.records.remove(&ticket);true} else {false}
    }
    pub fn get(&self,ticket:u64) -> Option<ShellTransfer> {self.records.get(&ticket).copied()}
}
#[derive(Default)]
pub struct Departures { next:u64, tabs:BTreeMap<u32,u64> }
impl Departures {
    pub fn can_begin(&self,tab:u32) -> bool {tab!=0 && self.next<u64::MAX && (self.tabs.contains_key(&tab) || self.tabs.len()<MAX_ROWS)}
    pub fn begin(&mut self,tab:u32) -> u64 {assert!(self.can_begin(tab));self.next+=1;self.tabs.insert(tab,self.next);self.next}
    pub fn matches(&self,tab:u32,token:u64) -> bool {self.tabs.get(&tab)==Some(&token)}
    pub fn clear(&mut self,tab:u32,token:u64) {if self.matches(tab,token) {self.tabs.remove(&tab);}}
    pub fn removed(&mut self,tab:u32) {self.tabs.remove(&tab);}
}

#[cfg(test)]
mod tests {
    use super::*;
    fn rows()->Vec<ShellForeignRow>{vec![ShellForeignRow{id:1,left:0.0,width:100.0},ShellForeignRow{id:2,left:100.0,width:40.0}]}
    #[test]
    fn frozen_projection_midpoint_ties_and_distinct_merge_widths(){
        let mut p=ForeignPreview::default();assert!(p.begin(&rows()));
        assert_eq!(p.hover(50.0),Some(true));assert_eq!(p.state().projected,0);
        assert_eq!(p.hover(51.0),Some(true));assert_eq!(p.state().projected,1);
        assert_eq!(p.hover(130.0),Some(false));assert_eq!(p.state().width,100.0);
        assert_eq!(ForeignPreview::external_index(&rows(),130.0),Some(2));
        assert_eq!(p.hover(151.0),Some(true));assert_eq!(p.state().projected,2);
        assert_eq!(p.hover(151.0),Some(false));
    }
    #[test]
    fn empty_and_reversed_geometry_keep_existing_index_contract(){
        let mut p=ForeignPreview::default();assert!(p.begin(&[]));p.hover(500.0);
        assert_eq!(p.state().projected,0);assert_eq!(p.state().width,0.0);
        let mut r=rows();r.reverse();p.begin(&r);p.hover(60.0);
        assert_eq!(p.state().projected,2);assert_eq!(ForeignPreview::external_index(&r,60.0),Some(2));
    }
    #[test]
    fn exit_preserves_admission_but_changed_order_invalidates_exact_snapshot(){
        let mut p=ForeignPreview::default();p.begin(&rows());p.hover(60.0);
        assert!(!p.layout(&[1,2]));assert!(p.layout(&[2,1]));assert_eq!(p.state().active,0);
        p.begin(&rows());p.hover(60.0);p.bind(9);assert!(!p.end(false));
        assert!(!p.layout(&[1,2]));assert_eq!(p.state().request,9);
        assert!(!p.layout(&[1,3]));assert_eq!(p.state().active,0);
    }
    #[test]
    fn stale_rejection_cannot_clear_new_preview_and_success_waits_for_layout(){
        let mut p=ForeignPreview::default();p.begin(&rows());p.hover(60.0);p.bind(9);
        p.complete(9,true);assert_eq!(p.state().request,9);
        p.end(true);p.begin(&rows());p.hover(160.0);p.bind(10);
        p.complete(9,false);assert_eq!(p.state().request,10);
        p.complete(10,false);assert_eq!(p.state().active,0);
        p.begin(&rows());p.bind(0);assert_eq!(p.state().active,0);
    }
    #[test]
    fn invalid_geometry_is_rejected_without_mutation(){
        let mut p=ForeignPreview::default();p.begin(&rows());p.hover(60.0);
        for r in [vec![ShellForeignRow{id:0,..rows()[0]}],vec![rows()[0];2],vec![ShellForeignRow{width:-1.0,..rows()[0]}],vec![ShellForeignRow{left:f64::NAN,..rows()[0]}],vec![rows()[0];MAX_ROWS+1]] {
            assert!(!p.begin(&r));assert_eq!(p.state().projected,1);
        }
        assert_eq!(p.hover(f64::INFINITY),None);assert_eq!(p.state().projected,1);
    }
    #[test]
    fn bounded_records_keep_outcomes_until_ack_and_do_not_evict(){
        let mut t=Transfers::default();
        for token in 1..=MAX_TRANSFERS as u64 {let r=t.reserve(10,token,1);assert!(t.bind(r.ticket,token));}
        assert!(!t.can_reserve());t.complete(1,false);
        let effect=t.next_effect(0);assert_eq!(effect.result,2);assert_eq!(effect.request,1);
        assert_eq!(t.next_effect(0).ticket,effect.ticket);assert!(!t.can_reserve());
        assert!(!t.ack(2));assert!(t.ack(effect.ticket));assert!(t.can_reserve());
        let new=t.reserve(20,1,1);assert!(new.ticket>effect.ticket);assert!(!t.ack(effect.ticket));
    }
    #[test]
    fn cancellation_preserves_known_acceptance_and_unbound_slots_become_effects(){
        let mut t=Transfers::default();let accepted=t.reserve(10,1,1);t.bind(accepted.ticket,8);t.complete(8,true);
        let waiting=t.reserve(11,2,1);t.bind(waiting.ticket,9);let reserved=t.reserve(12,3,1);
        t.cancel();assert_eq!(t.next_effect(0).result,1);
        assert_eq!(t.next_effect(accepted.ticket).result,2);assert_eq!(t.next_effect(waiting.ticket).ticket,reserved.ticket);
        t.complete(9,true);assert_eq!(t.get(waiting.ticket).unwrap().result,2);
    }
    #[test]
    fn tokens_are_exact_across_new_departures_and_removed_rows(){
        let mut d=Departures::default();let old=d.begin(7);let new=d.begin(7);
        assert!(!d.matches(7,old));d.clear(7,old);assert!(d.matches(7,new));
        assert!(!d.matches(8,new));d.removed(7);assert!(!d.matches(7,new));
        assert!(d.begin(7)>new);d.next=u64::MAX;assert!(!d.can_begin(7));
        let mut t=Transfers::default();t.next=u64::MAX;assert!(!t.can_reserve());
    }
}
