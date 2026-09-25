//! Whole-group local reorder: logical-axis geometry supplied by WinUI.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellGroupDragSlot { pub group: u32, pub tab: u32, pub start: f64, pub end: f64 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellGroupDrag {
    pub phase: u32,
    pub group: u32,
    pub before_group: u32,
    pub before_tab: u32,
    pub valid: bool,
    pub marker: f64,
    pub awaiting: bool,
    pub request: u64,
}
#[derive(Default)]
pub struct GroupDrag { state: ShellGroupDrag, x: f64, y: f64, source:usize, target:usize, count:usize }
fn finite(n: f64) -> bool { n.is_finite() && n.abs() <= 1_000_000_000.0 }
impl GroupDrag {
    pub fn state(&self) -> ShellGroupDrag { self.state }
    pub fn arm(&mut self, group: u32, x: f64, y: f64) -> bool {
        if group == 0 || !finite(x) || !finite(y) { return false; }
        self.state = ShellGroupDrag { phase:1, group, ..Default::default() };
        self.x=x; self.y=y; true
    }
    pub fn update(&mut self, x:f64, y:f64, inside:bool, slots:&[ShellGroupDragSlot]) -> bool {
        if self.state.phase == 0 || !finite(x) || !finite(y) || slots.is_empty() || slots.len()>10000 { return false; }
        let mut ids=std::collections::HashSet::new();
        if slots.iter().any(|s| (s.group==0)==(s.tab==0) || !finite(s.start) || !finite(s.end) || s.end<s.start || !ids.insert((s.group,s.tab))) { return false; }
        let Some(source)=slots.iter().position(|s| s.group==self.state.group) else { return false; };
        self.source=source; self.count=slots.len();
        if self.state.phase==1 && (x-self.x).abs().max((y-self.y).abs())>=4.0 { self.state.phase=2; }
        self.state.valid=false;
        if self.state.phase!=2 || !inside { return true; }
        let target=slots.iter().enumerate().find(|(i,s)| *i!=source && x<(s.start+s.end)/2.0);
        let (index,group,tab,marker)=match target {
            Some((i,s)) => (i,s.group,s.tab,s.start),
            None => (slots.len(),0,0,slots.last().unwrap().end),
        };
        self.state.before_group=group; self.state.before_tab=tab; self.state.marker=marker;
        self.target=index;
        self.state.valid=index!=source+1;
        true
    }
    pub fn offset(&self, index:usize, width:f64)->Option<f64> {
        if !finite(width) || width<0. || index>=self.count { return None; }
        if self.state.phase!=2 || !self.state.valid { return Some(0.); }
        Some(if self.target<self.source && index>=self.target && index<self.source { width }
            else if self.target>self.source && index>self.source && index<self.target { -width }
            else { 0. })
    }
    pub fn wait(&mut self, request:u64, group:u32, before_group:u32, before_tab:u32) {
        self.state=ShellGroupDrag { group,before_group,before_tab,awaiting:request!=0,request,..Default::default() };
    }
    pub fn complete(&mut self, request:u64, accepted:bool) {
        if self.state.request==request && !accepted { self.state.awaiting=false; }
    }
    pub fn publish(&mut self, slots:&[crate::strip::ShellStripSlot]) {
        if !self.state.awaiting { return; }
        let units:Vec<_>=slots.iter().filter(|s| s.tab==0 || s.group==0).collect();
        let landed=match units.iter().position(|s| s.group==self.state.group) {
            None=>true,
            Some(i)=>match units.get(i+1) {
                Some(next)=> next.group==self.state.before_group && next.tab==self.state.before_tab,
                None=> self.state.before_group==0 && self.state.before_tab==0,
            }
        };
        if landed { self.state.awaiting=false; }
    }
    pub fn end(&mut self) { self.state=Default::default(); }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn slots()->[ShellGroupDragSlot;3] { [ShellGroupDragSlot{group:1,start:0.,end:160.,tab:0},ShellGroupDragSlot{group:2,start:160.,end:192.,tab:0},ShellGroupDragSlot{group:0,tab:9,start:192.,end:280.}] }
    #[test] fn gap_shifts_whole_blocks_in_both_directions_and_resets() {
        let mut d=GroupDrag::default(); d.arm(1,10.,10.);
        d.update(280.,10.,true,&slots());
        assert_eq!((0..3).map(|i|d.offset(i,40.).unwrap()).collect::<Vec<_>>(),vec![0.,-40.,-40.]);
        d.update(170.,10.,true,&slots());
        assert_eq!(d.offset(1,40.),Some(0.)); // adjacent/no-op
        d.arm(2,170.,10.); d.update(0.,10.,true,&slots());
        assert_eq!((0..3).map(|i|d.offset(i,40.).unwrap()).collect::<Vec<_>>(),vec![40.,0.,0.]);
        d.update(0.,80.,false,&slots()); assert_eq!(d.offset(0,40.),Some(0.));
        d.end(); assert_eq!(d.offset(0,40.),Some(0.));
        assert_eq!(d.offset(3,40.),None);
        assert_eq!(d.offset(0,f64::NAN),None);
        assert_eq!(d.offset(0,-1.),None);
    }
    #[test] fn accepted_request_waits_for_authoritative_order_and_ignores_old_outcomes() {
        use crate::strip::ShellStripSlot as Slot;
        let mut d=GroupDrag::default(); d.wait(10,1,0,9);
        d.complete(10,true); assert!(d.state().awaiting);
        d.publish(&[Slot{group:0,tab:9},Slot{group:1,tab:0}]); assert!(d.state().awaiting);
        d.publish(&[Slot{group:1,tab:0},Slot{group:1,tab:3},Slot{group:0,tab:9}]); assert!(!d.state().awaiting);
        d.wait(11,1,0,0); d.complete(10,false); assert!(d.state().awaiting);
        d.publish(&[Slot{group:0,tab:9},Slot{group:1,tab:0}]); assert!(!d.state().awaiting);
        d.wait(12,1,2,0); d.complete(12,false); assert!(!d.state().awaiting);
        d.wait(13,1,2,0); d.publish(&[]); assert!(!d.state().awaiting);
    }
    #[test] fn threshold_boundaries_and_noop() {
        let mut d=GroupDrag::default(); d.arm(1,10.,10.);
        d.update(13.,10.,true,&slots()); assert_eq!(d.state().phase,1);
        d.update(14.,10.,true,&slots()); assert_eq!(d.state().phase,2); assert!(!d.state().valid);
        d.update(180.,10.,true,&slots()); assert_eq!(d.state().before_tab,9); assert!(d.state().valid);
        d.update(280.,10.,true,&slots()); assert_eq!(d.state().before_tab,0); assert!(d.state().valid);
        d.update(280.,70.,false,&slots()); assert!(!d.state().valid);
        d.end(); assert_eq!(d.state().phase,0);
    }
    #[test] fn reverse_motion_negative_axis_and_malformed_input() {
        let mut d=GroupDrag::default(); d.arm(2,-100.,10.);
        let s=slots().map(|s| ShellGroupDragSlot{start:s.start-300.,end:s.end-300.,..s});
        assert!(d.update(-300.,10.,true,&s)); assert_eq!(d.state().before_group,1); assert!(d.state().valid);
        assert!(!d.update(f64::NAN,0.,true,&s));
        assert!(!d.update(0.,0.,true,&[s[0],s[0]]));
        assert!(!d.update(0.,0.,true,&[s[0]]));
        assert!(!d.arm(0,0.,0.)); assert!(!d.arm(1,f64::INFINITY,0.));
    }
}
