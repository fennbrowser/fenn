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
    /// The pointer has left the strip's bounded margin: release detaches the
    /// whole group into a new window instead of reordering it.
    pub detaching: bool,
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
    pub fn update_viewport(&mut self, x:f64, y:f64, inside:bool, start:f64, end:f64,
        slots:&[ShellGroupDragSlot], layout:&[crate::strip::ShellStripSlot])->bool {
        if !finite(start) || !finite(end) || end < start { return false; }
        if !self.update(x,y,inside,slots) { return false; }
        if self.state.phase != 2 || !inside || x < end { return true; }
        // The trailing controls/blank area means after the last visible block,
        // not after every browser tab. Keep an offscreen successor's identity
        // even when WinUI has not realized its control.
        let Some(last) = slots.iter().rposition(|s| s.group != self.state.group && s.start < end && s.end > start) else {
            self.state.valid = false; return true;
        };
        let units:Vec<_> = layout.iter().filter(|s| s.tab == 0 || s.group == 0).collect();
        let Some(position) = units.iter().position(|s| s.group == slots[last].group && s.tab == slots[last].tab) else {
            self.state.valid = false; return true;
        };
        let next = units.iter().skip(position+1).find(|s| s.group != self.state.group);
        let (group,tab) = next.map_or((0,0),|s| (s.group,s.tab));
        self.target = slots.iter().enumerate().skip(last+1).find(|(_,s)| s.group != self.state.group)
            .map_or(slots.len(),|(i,_)| i);
        self.state.before_group = group; self.state.before_tab = tab;
        self.state.marker = slots[last].end.min(end);
        self.state.valid = units.iter().position(|s| s.group == self.state.group)
            .is_some_and(|source| source != position+1);
        true
    }
    /// Classify tear-off with the same bounded margin a single tab uses.
    /// A group holding every tab still detaches: Firefox's new window adopts
    /// it and the emptied source window closes, as with its own group drag.
    /// `foreign`: another window's strip claims the pointer, whatever this
    /// strip's own geometry says (windows can overlap or sit side by side).
    pub fn classify(&mut self, x:f64, y:f64, width:f64, height:f64, foreign:bool)->bool {
        if ![x,y,width,height].into_iter().all(finite) || width<0. || height<0. { return false; }
        self.state.detaching = self.state.phase==2 && (foreign || height>0. &&
            (x < -height || x > width+height || y < -height || y > height*2.));
        if self.state.detaching { self.state.valid=false; }
        true
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

/// Destination of another window's whole-group drag. Insertion is between
/// whole blocks (an ungrouped tab or a group), never inside a group. After the
/// drop the gap is held until Firefox rejects the adopt or publishes.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellForeignGroup {
    pub active: bool,
    /// Measured block the group lands before; the block count means the end.
    pub index: u32,
    pub before_group: u32,
    pub before_tab: u32,
    pub request: u64,
    /// For a held request: 0 pending, 1 accepted, 2 rejected or cancelled.
    pub result: u32,
}
#[derive(Default)]
pub struct ForeignGroup { state: ShellForeignGroup, count: usize }
impl ForeignGroup {
    pub fn state(&self) -> ShellForeignGroup { self.state }
    pub fn hover(&mut self, x:f64, slots:&[ShellGroupDragSlot], layout:&[crate::strip::ShellStripSlot]) -> bool {
        if self.state.request != 0 || !finite(x) || slots.len() > 10000 { return false; }
        let mut ids=std::collections::HashSet::new();
        if slots.iter().any(|s| (s.group==0)==(s.tab==0) || !finite(s.start) || !finite(s.end) || s.end<s.start || !ids.insert((s.group,s.tab))) { return false; }
        let (index,group,tab)=match slots.iter().position(|s| x < (s.start+s.end)/2.) {
            Some(i) => (i,slots[i].group,slots[i].tab),
            // Past the last measured block: keep an unrealized successor's identity.
            None => {
                let units:Vec<_>=layout.iter().filter(|s| s.tab==0 || s.group==0).collect();
                let next=slots.last().and_then(|last| units.iter().position(|s| s.group==last.group && s.tab==last.tab))
                    .and_then(|i| units.get(i+1));
                (slots.len(),next.map_or(0,|s| s.group),next.map_or(0,|s| s.tab))
            }
        };
        self.count=slots.len();
        self.state=ShellForeignGroup { active:true, index:index as u32, before_group:group, before_tab:tab, ..Default::default() };
        true
    }
    pub fn offset(&self, index:usize, width:f64)->Option<f64> {
        if !finite(width) || width<0. || index>=self.count { return None; }
        Some(if self.state.active && index>=self.state.index as usize { width } else { 0. })
    }
    pub fn hold(&mut self, request:u64)->bool {
        if !self.state.active || request==0 || self.state.request!=0 { return false; }
        self.state.request=request; self.state.result=0; true
    }
    pub fn complete(&mut self, request:u64, accepted:bool) {
        if self.state.request==request && request!=0 && self.state.result==0 { self.state.result=if accepted {1} else {2}; }
    }
    /// Ends a live preview, or a held one only when `force` (a new layout)
    /// or once Firefox has rejected it.
    pub fn end(&mut self, force:bool)->bool {
        if self.state.request!=0 && !force && self.state.result!=2 { return false; }
        *self=Default::default(); true
    }
}

/// A group leaving this strip for another window. It stays hidden from the
/// release until Firefox publishes a layout without it, or the move fails.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellGroupDeparture {
    pub group: u32,
    /// This strip's own detach request, or 0 when another window's strip
    /// queued the adopt and reports a rejection itself.
    pub request: u64,
    /// 0 pending, 1 accepted (still waiting for removal), 2 failed: restore.
    pub result: u32,
}
#[derive(Default)]
pub struct GroupDeparture { state: ShellGroupDeparture }
impl GroupDeparture {
    pub fn state(&self) -> ShellGroupDeparture { self.state }
    pub fn begin(&mut self, group:u32, request:u64) -> bool {
        if group==0 || self.state.group!=0 { return false; }
        self.state=ShellGroupDeparture { group, request, result:0 }; true
    }
    pub fn complete(&mut self, request:u64, accepted:bool) {
        if self.state.group!=0 && self.state.request==request && request!=0 && self.state.result==0 {
            self.state.result=if accepted {1} else {2};
        }
    }
    /// Another window's strip reports that Firefox refused its adopt.
    pub fn reject(&mut self, group:u32) -> bool {
        if self.state.group==0 || self.state.group!=group || self.state.result==2 { return false; }
        self.state.result=2; true
    }
    /// True once a published layout no longer contains the group.
    pub fn publish(&mut self, layout:&[crate::strip::ShellStripSlot]) -> bool {
        self.state.group!=0 && !layout.iter().any(|s| s.group==self.state.group)
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
    #[test] fn trailing_area_inserts_before_offscreen_successor_without_realization() {
        use crate::strip::ShellStripSlot as S;
        let layout=[S{group:1,tab:0},S{group:1,tab:3},S{group:0,tab:9},S{group:0,tab:10},S{group:2,tab:0}];
        let measured=[ShellGroupDragSlot{group:1,tab:0,start:0.,end:40.},
            ShellGroupDragSlot{group:0,tab:9,start:40.,end:140.}];
        let mut d=GroupDrag::default(); d.arm(1,10.,10.);
        for x in [120.,150.,400.] {
            assert!(d.update_viewport(x,10.,true,0.,120.,&measured,&layout));
            assert_eq!(d.state().before_tab,10);
            assert!(d.state().valid);
            assert_eq!(d.offset(1,40.),Some(-40.));
        }
        // A fully offscreen measured row must not become the last visible tab.
        let mut all=measured.to_vec(); all.push(ShellGroupDragSlot{group:0,tab:10,start:140.,end:240.});
        d.update_viewport(400.,10.,true,0.,120.,&all,&layout);
        assert_eq!(d.state().before_tab,10);
        assert_eq!(d.offset(2,40.),Some(0.));
        d.update_viewport(400.,10.,true,0.,200.,&all,&layout);
        assert_eq!(d.state().before_group,2); // includes partially visible last row
        d.update_viewport(400.,10.,true,0.,200.,&all,&layout[..4]);
        assert_eq!((d.state().before_group,d.state().before_tab),(0,0));
        d.update_viewport(400.,80.,false,0.,200.,&all,&layout);
        assert!(!d.state().valid);
        assert!(!d.update_viewport(400.,10.,true,200.,0.,&all,&layout));
    }
    #[test] fn trailing_area_preserves_scrolled_group_boundaries_and_noops() {
        use crate::strip::ShellStripSlot as S;
        let layout=[S{group:2,tab:0},S{group:2,tab:8},S{group:1,tab:0},S{group:0,tab:9}];
        let measured=[ShellGroupDragSlot{group:2,tab:0,start:-80.,end:140.},
            ShellGroupDragSlot{group:1,tab:0,start:140.,end:180.}];
        let mut d=GroupDrag::default(); d.arm(1,150.,10.);
        d.update_viewport(300.,10.,true,0.,120.,&measured,&layout);
        assert_eq!(d.state().before_tab,9);
        assert!(!d.state().valid); // already immediately after the visible group
        d.update_viewport(300.,10.,true,200.,250.,&measured,&layout);
        assert!(!d.state().valid); // no visible destination
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
    #[test] fn detaching_uses_tab_margin_and_closes_gap() {
        let mut d=GroupDrag::default(); d.arm(1,10.,10.);
        assert!(d.classify(10.,10.,300.,40.,false)); assert!(!d.state().detaching); // armed only
        d.update(280.,10.,true,&slots()); assert!(d.state().valid);
        for (x,y) in [(280.,80.),(280.,79.9),(-40.,10.),(340.,10.),(280.,-40.)] {
            d.update(x,y,false,&slots()); d.classify(x,y,300.,40.,false);
            assert!(!d.state().detaching,"{x},{y}");
        }
        for (x,y) in [(280.,80.1),(-40.1,10.),(340.1,10.),(280.,-40.1)] {
            d.update(x,y,false,&slots()); d.classify(x,y,300.,40.,false);
            assert!(d.state().detaching,"{x},{y}"); assert!(!d.state().valid);
            assert_eq!(d.offset(1,40.),Some(0.));
        }
        // Back over the strip reorders again.
        d.update(280.,10.,true,&slots()); d.classify(280.,10.,300.,40.,false);
        assert!(!d.state().detaching); assert!(d.state().valid);
        d.classify(280.,200.,300.,0.,false); assert!(!d.state().detaching);
        d.update(280.,10.,true,&slots()); d.classify(280.,10.,300.,40.,true);
        assert!(d.state().detaching && !d.state().valid); // another strip wins
        assert!(!d.classify(f64::NAN,0.,300.,40.,false));
        assert!(!d.classify(0.,0.,-1.,40.,false));
        d.end(); assert!(!d.state().detaching);
    }
    #[test] fn foreign_group_inserts_between_whole_blocks_and_holds_until_answered() {
        use crate::strip::ShellStripSlot as S;
        // Destination: tab 9, group 2 (members merged into one block), tab 10.
        let blocks=[ShellGroupDragSlot{group:0,tab:9,start:0.,end:100.},
            ShellGroupDragSlot{group:2,tab:0,start:100.,end:260.},
            ShellGroupDragSlot{group:0,tab:10,start:260.,end:360.}];
        let layout=[S{group:0,tab:9},S{group:2,tab:0},S{group:2,tab:4},S{group:0,tab:10},S{group:0,tab:11}];
        let mut f=ForeignGroup::default();
        assert_eq!(f.offset(0,40.),None);
        assert!(f.hover(10.,&blocks,&layout)); assert_eq!((f.state().index,f.state().before_group,f.state().before_tab),(0,0,9));
        // Inside the group block: before or after the whole group, never between members.
        assert!(f.hover(170.,&blocks,&layout)); assert_eq!((f.state().before_group,f.state().before_tab),(2,0));
        assert!(f.hover(190.,&blocks,&layout)); assert_eq!((f.state().index,f.state().before_tab),(2,10));
        assert_eq!((0..3).map(|i| f.offset(i,40.).unwrap()).collect::<Vec<_>>(),vec![0.,0.,40.]);
        // Past the last realized block: before the offscreen successor, else the end.
        assert!(f.hover(400.,&blocks,&layout)); assert_eq!((f.state().index,f.state().before_tab),(3,11));
        assert!(f.hover(400.,&blocks,&layout[..4])); assert_eq!((f.state().before_group,f.state().before_tab),(0,0));
        assert!(f.hover(0.,&[],&layout)); assert_eq!(f.state().index,0);
        assert!(f.hover(10.,&blocks,&layout));
        // Held: no retargeting, an unforced end keeps the gap, a rejection releases it.
        assert!(!f.hold(0)); assert!(f.hold(7)); assert!(!f.hold(8));
        assert!(!f.hover(300.,&blocks,&layout)); assert!(!f.end(false));
        f.complete(6,false); assert_eq!(f.state().result,0);
        f.complete(7,false); assert_eq!(f.state().result,2);
        assert!(f.end(false)); assert!(!f.state().active);
        assert!(!f.hold(9)); // nothing to hold once ended
        assert!(f.hover(10.,&blocks,&layout)); assert!(f.hold(9));
        f.complete(9,true); f.complete(9,false); assert_eq!(f.state().result,1);
        assert!(!f.end(false)); assert!(f.end(true));
        assert!(!f.hover(f64::NAN,&blocks,&layout));
        assert!(!f.hover(0.,&[blocks[0],blocks[0]],&layout));
        assert!(!f.hover(0.,&[ShellGroupDragSlot{group:1,tab:1,start:0.,end:1.}],&layout));
    }
    #[test] fn departure_waits_for_removal_and_restores_on_failure() {
        use crate::strip::ShellStripSlot as S;
        let present=[S{group:1,tab:0},S{group:1,tab:3},S{group:0,tab:9}];
        let removed=[S{group:0,tab:9}];
        let mut d=GroupDeparture::default();
        assert!(!d.begin(0,5)); assert!(!d.publish(&removed)); // nothing departing
        assert!(d.begin(1,5)); assert!(!d.begin(2,6));
        d.complete(4,false); assert_eq!(d.state().result,0); // another request
        d.complete(5,true); assert_eq!(d.state().result,1);
        d.complete(5,false); assert_eq!(d.state().result,1); // first answer stands
        assert!(!d.publish(&present)); assert!(d.publish(&removed));
        d.end(); assert_eq!(d.state().group,0);
        // Our own request rejected.
        assert!(d.begin(1,7)); d.complete(7,false); assert_eq!(d.state().result,2); d.end();
        // Another window's strip queued the adopt: only its rejection counts.
        assert!(d.begin(1,0)); d.complete(0,false); assert_eq!(d.state().result,0);
        assert!(!d.reject(2)); assert!(d.reject(1)); assert!(!d.reject(1)); assert_eq!(d.state().result,2);
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
