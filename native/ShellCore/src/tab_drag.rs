//! Local tab gesture and reorder feedback. Coordinates are measured DIPs;
//! WinUI owns capture, layout, row transforms and animation.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellDragRow { pub left: f64, pub width: f64 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellTabDrag {
    /// 0 idle, 1 armed, 2 dragging.
    pub phase: u32,
    pub tab: u32,
    pub original: u32,
    pub projected: u32,
    pub start_x: f64,
    pub start_y: f64,
    pub detaching: u32,
    pub awaiting: u32,
    pub request: u64,
    pub landing_offset: f64,
}
#[derive(Default)]
pub struct TabDrag {
    state: ShellTabDrag,
    rows: Vec<ShellDragRow>,
    before: u32,
    join_anchor: Option<u32>,
    join_destination: Option<(u32,u32,bool)>,
}
pub const MAX_ROWS: usize = 10_000;
fn coordinate(v: f64) -> bool { v.is_finite() && v.abs() <= 1_000_000_000.0 }
impl TabDrag {
    pub fn state(&self) -> ShellTabDrag { ShellTabDrag { landing_offset: self.landing(), ..self.state } }
    pub fn arm(&mut self, tab: u32, x: f64, y: f64) -> bool {
        if tab == 0 || !coordinate(x) || !coordinate(y) { return false; }
        *self = Self::default();
        self.state = ShellTabDrag { phase: 1, tab, start_x: x, start_y: y, ..Default::default() };
        true
    }
    pub fn ready(&self, tab: u32, x: f64, y: f64, continuation: bool) -> bool {
        self.state.phase == 1 && self.state.tab == tab && coordinate(x) && coordinate(y) &&
            (continuation || (x-self.state.start_x).abs().max((y-self.state.start_y).abs()) >= 4.0)
    }
    pub fn promote(&mut self, original: u32, rows: &[ShellDragRow]) -> bool {
        if self.state.phase != 1 || rows.len() > MAX_ROWS || original as usize >= rows.len() ||
            rows.iter().any(|r| !coordinate(r.left) || !coordinate(r.width) || r.width < 0.0) { return false; }
        self.rows = rows.to_vec();
        self.state.original = original;
        self.state.projected = original;
        self.state.detaching = 0;
        self.state.phase = 2;
        true
    }
    pub fn update(&mut self, x: f64, y: f64, width: f64, height: f64, project: bool) -> bool {
        if self.state.phase != 2 || ![x,y,width,height].into_iter().all(coordinate) || width < 0.0 || height < 0.0 { return false; }
        let detaching = height > 0.0 && (x < -height || x > width+height || y < -height || y > height*2.0);
        if detaching && self.state.detaching == 0 { self.close_gap(); }
        self.state.detaching = detaching as u32;
        if detaching || !project { return true; }
        self.join_anchor = None;
        self.join_destination = None;
        let original = self.state.original as usize;
        let row = self.rows[original];
        let min = -row.left;
        let max = width-row.width-row.left;
        let offset = (x-self.state.start_x).clamp(min.min(max), min.max(max));
        let center = row.left+row.width/2.0+offset;
        let before = self.rows.iter().enumerate().filter(|(i,r)| *i != original && r.left+r.width/2.0 < center).count() as u32;
        self.state.projected = if before < self.state.original { before } else { before+1 };
        true
    }
    pub fn join(&mut self, index: u32, anchor: u32) -> bool {
        if self.state.phase != 2 || self.state.detaching != 0 ||
            index as usize > self.rows.len() || anchor > index { return false; }
        self.state.projected = index;
        self.join_anchor = Some(anchor);
        true
    }
    pub fn join_group(&mut self, layout: &[crate::strip::ShellStripSlot], group: u32, tab: u32, before: bool) -> bool {
        if group == 0 || crate::group_drop::target(layout,self.state.tab,tab,if tab == 0 { group } else { 0 }) != group {
            return false;
        }
        let (mut index, mut anchor, mut insertion) = (0, None, None);
        for slot in layout {
            if slot.tab == 0 {
                if slot.group == group { anchor = Some(index); if tab == 0 { insertion = Some(index); } }
                continue;
            }
            if (tab == 0 && slot.group == group) || slot.tab == tab {
                insertion = Some(index + u32::from(tab == 0 || !before));
            }
            index += 1;
        }
        match (anchor,insertion) {
            (Some(anchor),Some(index)) if self.join(index,anchor) => {
                self.join_destination = Some((group,tab,before)); true
            },
            _ => false,
        }
    }
    pub fn close_gap(&mut self) {
        self.join_anchor = None;
        self.join_destination = None;
        if self.state.phase == 2 { self.state.projected = self.state.original; }
    }
    /// Expanded disclosures target the leading slot; collapsed ones append.
    pub fn header_hit(&self, layout: &[crate::strip::ShellStripSlot], group: u32, collapsed: bool) -> Option<(u32,u32,bool)> {
        if self.state.phase != 2 || self.state.detaching != 0 ||
            crate::group_drop::target(layout,self.state.tab,0,group) != group || group == 0 { return None; }
        if !collapsed {
            if let Some(first) = layout.iter().find(|s| s.group == group && s.tab != 0) {
                return Some((group,first.tab,true));
            }
        }
        Some((group,0,false))
    }
    /// Hit the final preview geometry, never an intermediate animation frame.
    /// The open gap retains its destination across its full width.
    pub fn join_hit(&self, layout: &[crate::strip::ShellStripSlot], x: f64) -> Option<(u32,u32,bool)> {
        if self.state.phase != 2 || self.state.detaching != 0 || !coordinate(x) { return None; }
        let tabs:Vec<_> = layout.iter().filter(|s| s.tab != 0).collect();
        if tabs.len() != self.rows.len() { return None; }
        let source_group = tabs.iter().find(|s| s.tab == self.state.tab)?.group;
        let width = self.rows[self.state.original as usize].width;
        let left = |i:usize| self.rows[i].left + if self.join_destination.is_some() {
            self.slot_offset(i as u32,false,false,false).unwrap_or(0.0)
        } else { 0.0 };
        if let Some((group,tab,before)) = self.join_destination {
            let target = if tab != 0 { tabs.iter().position(|s| s.tab == tab) }
                else { tabs.iter().rposition(|s| s.group == group) };
            if let Some(i) = target {
                let start = if tab != 0 && before { left(i)-width } else { left(i)+self.rows[i].width };
                if x >= start && x <= start+width { return self.join_destination; }
            }
        }
        for (i,slot) in tabs.iter().enumerate() {
            if slot.group == 0 || slot.group == source_group { continue; }
            let start=left(i);
            if x >= start && x <= start+self.rows[i].width {
                return Some((slot.group,slot.tab,x < start+self.rows[i].width/2.0));
            }
        }
        None
    }
    pub fn offset(&self, index: u32) -> Option<f64> {
        let i = index as usize;
        if i >= self.rows.len() { return None; }
        Some(if index < self.state.original && index >= self.state.projected {
            self.rows[i+1].left-self.rows[i].left
        } else if index > self.state.original && index < self.state.projected {
            self.rows[i-1].left-self.rows[i].left
        } else { 0.0 })
    }
    pub fn landing(&self) -> f64 {
        let index = self.state.projected.saturating_sub((self.state.projected > self.state.original) as u32) as usize;
        match (self.rows.get(index), self.rows.get(self.state.original as usize)) {
            (Some(to),Some(from)) => to.left-from.left, _ => 0.0,
        }
    }
    // Group headers occupy their own layout space. Moving a tab transfers only
    // its width; a distance between neighboring tabs can include a whole header.
    // A header is anchored immediately before `index` (len for trailing groups).
    pub fn slot_offset(&self, index: u32, header: bool, source_header: bool, join_header: bool) -> Option<f64> {
        if index as usize > self.rows.len() || (!header && index as usize == self.rows.len()) { return None; }
        let width = self.rows.get(self.state.original as usize)?.width;
        // A join gap belongs after the destination header, including when the
        // tab index stays unchanged and only the header must cross the source.
        if header && join_header && self.join_anchor == Some(index) {
            return Some(if self.state.original < index { -width } else { 0.0 });
        }
        // An internal reorder lands after its own header, not before the group.
        if header && source_header && self.join_anchor.is_none() && index == self.state.projected { return Some(0.0); }
        Some(if self.state.projected < self.state.original && (index < self.state.original || (header && index == self.state.original)) && index >= self.state.projected {
            width
        } else if index > self.state.original && index < self.state.projected {
            -width
        } else { 0.0 })
    }
    pub fn end(&mut self, cancel: bool) {
        self.state.phase = 0;
        self.join_anchor = None;
        self.join_destination = None;
        if cancel { self.state.detaching = 0; self.state.awaiting = 0; self.state.request = 0; }
    }
    pub fn commit(&mut self, request: u64, before: u32) -> bool {
        if self.state.phase != 2 { return false; }
        self.state.phase = 0;
        self.state.awaiting = (request != 0) as u32;
        self.state.request = request;
        self.before = before;
        true
    }
    pub fn complete(&mut self, request: u64, accepted: bool) {
        if self.state.request == request && !accepted {
            self.state.awaiting = 0;
            self.state.request = 0;
        }
    }
    pub fn publish(&mut self, ids: &[u32]) -> bool {
        if self.state.phase == 2 { return false; }
        let landed = self.state.awaiting == 0 || match ids.iter().position(|id| *id == self.state.tab) {
            None => true,
            Some(i) => if self.before == 0 { i+1 == ids.len() } else { ids.get(i+1) == Some(&self.before) },
        };
        if landed { self.state.awaiting = 0; self.state.request = 0; }
        landed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn drag(original: u32) -> TabDrag {
        let mut d = TabDrag::default();
        assert!(d.arm(original+1, 50.0+original as f64*100.0, 16.0));
        assert!(d.promote(original, &[ShellDragRow{left:0.0,width:100.0},
            ShellDragRow{left:100.0,width:100.0},ShellDragRow{left:200.0,width:100.0}]));
        d
    }
    #[test]
    fn grouped_slots_move_headers_with_members_without_consuming_header_width() {
        let mut d=TabDrag::default();
        d.arm(1,50.0,16.0);
        d.promote(0,&[ShellDragRow{left:0.0,width:100.0},
            ShellDragRow{left:140.0,width:100.0},ShellDragRow{left:240.0,width:100.0}]);
        d.update(210.0,16.0,500.0,40.0,true);
        assert_eq!(d.slot_offset(1,true,false,false),Some(-100.0));
        assert_eq!(d.slot_offset(1,false,false,false),Some(-100.0));
        assert_eq!(d.slot_offset(2,false,false,false),Some(0.0));
        assert_eq!(d.slot_offset(3,true,false,false),Some(0.0));
        assert_eq!(d.slot_offset(3,false,false,false),None);
        d.close_gap();
        assert_eq!(d.slot_offset(1,true,false,false),Some(0.0));
        assert_eq!(d.slot_offset(0,true,false,false),Some(0.0));
        d.arm(3,290.0,16.0);
        d.promote(2,&[ShellDragRow{left:0.0,width:100.0},
            ShellDragRow{left:140.0,width:100.0},ShellDragRow{left:280.0,width:120.0}]);
        d.update(20.0,16.0,500.0,40.0,true);
        assert_eq!(d.slot_offset(1,true,false,false),Some(120.0));
        assert_eq!(d.slot_offset(1,false,false,false),Some(120.0));
        assert_eq!(d.slot_offset(2,true,false,false),Some(120.0));
        assert_eq!(d.slot_offset(2,false,false,false),Some(0.0));
        d.update(100.0,16.0,500.0,40.0,true);
        assert_eq!(d.state().projected,1);
        assert_eq!(d.slot_offset(1,true,true,false),Some(0.0));
        assert_eq!(d.slot_offset(1,true,false,false),Some(120.0));
        assert_eq!(d.slot_offset(1,false,false,false),Some(120.0));
        d.close_gap();
        assert_eq!(d.slot_offset(2,true,false,false),Some(0.0));
    }
    #[test]
    fn direct_group_join_opens_a_gap_and_retains_the_destination_header() {
        let mut d = drag(2);
        assert!(d.join(1,1));
        assert_eq!(d.slot_offset(1,false,false,false),Some(100.0));
        assert_eq!(d.slot_offset(1,true,false,true),Some(0.0));
        // Another header at the same tab anchor is not the destination.
        assert_eq!(d.slot_offset(1,true,false,false),Some(100.0));
        assert!(d.update(150.0,16.0,400.0,40.0,false));
        assert_eq!(d.state().projected,1);
        assert_eq!(d.slot_offset(1,true,false,true),Some(0.0));
        // Leaving the group restores ordinary reorder boundary behavior.
        assert!(d.update(140.0,16.0,400.0,40.0,true));
        assert_eq!(d.slot_offset(1,true,false,true),Some(100.0));
        d = drag(0);
        assert!(d.join(1,1));
        assert_eq!(d.slot_offset(1,false,false,false),Some(0.0));
        assert_eq!(d.slot_offset(1,true,false,true),Some(-100.0));
        assert!(d.join(2,1));
        assert_eq!(d.slot_offset(1,false,false,false),Some(-100.0));
        assert!(!d.join(4,1));
        assert!(!d.join(1,2));
        assert_eq!(d.state().projected,2);
        d.close_gap();
        assert_eq!(d.slot_offset(1,true,false,true),Some(0.0));
        d.update(150.0,81.0,400.0,40.0,false);
        assert!(!d.join(2,1));
        d.end(true);
        assert!(!d.join(2,1));
    }
    #[test]
    fn join_target_follows_preview_and_retains_the_full_first_gap() {
        use crate::strip::ShellStripSlot as Slot;
        let layout=[Slot{group:0,tab:1},Slot{group:7,tab:0},Slot{group:7,tab:2},Slot{group:7,tab:3},Slot{group:0,tab:4}];
        for original in [0,3] {
            let mut d=TabDrag::default();
            d.arm(original+1,50.0+original as f64*100.0,16.0);
            d.promote(original,&[ShellDragRow{left:0.0,width:100.0},ShellDragRow{left:132.0,width:100.0},
                ShellDragRow{left:232.0,width:100.0},ShellDragRow{left:332.0,width:100.0}]);
            assert_eq!(d.header_hit(&layout,7,false),Some((7,2,true)));
            assert_eq!(d.header_hit(&layout,7,true),Some((7,0,false)));
            assert_eq!(d.header_hit(&layout,99,false),None);
            let (group,tab,before)=d.header_hit(&layout,7,false).unwrap();
            assert!(d.join_group(&layout,group,tab,before));
            assert_eq!(d.state().projected,1); // never skip both members
            d.close_gap();
            assert_eq!(d.join_hit(&layout,140.0),Some((7,2,true)));
            assert!(d.join_group(&layout,7,2,true));
            let gap=if original==0 { 32.0 } else { 132.0 };
            for x in [gap+1.0,gap+50.0,gap+99.0] {
                assert_eq!(d.join_hit(&layout,x),Some((7,2,true)));
            }
            // Crossing the shifted first member's midpoint selects slot two.
            let midpoint=gap+150.0;
            assert_eq!(d.join_hit(&layout,midpoint-1.0),Some((7,2,true)));
            assert_eq!(d.join_hit(&layout,midpoint+1.0),Some((7,2,false)));
            assert!(d.join_group(&layout,7,2,false));
            assert_eq!(d.join_hit(&layout,midpoint+1.0),Some((7,2,false)));
            assert_eq!(d.join_hit(&layout,gap+49.0),Some((7,2,true)));
            d.close_gap();
            assert_eq!(d.join_hit(&layout,140.0),Some((7,2,true)));
            assert_eq!(d.join_hit(&layout,f64::NAN),None);
        }
    }
    #[test]
    fn group_join_resolves_live_members_and_collapsed_headers() {
        use crate::strip::ShellStripSlot as Slot;
        let layout = [Slot{group:0,tab:1}, Slot{group:7,tab:0},
            Slot{group:7,tab:2}, Slot{group:7,tab:3}, Slot{group:9,tab:0}];
        let mut d = drag(0);
        assert!(d.join_group(&layout,7,2,true));
        assert_eq!(d.state().projected,1);
        assert!(d.join_group(&layout,7,2,false));
        assert_eq!(d.state().projected,2);
        assert!(d.join_group(&layout,7,0,false));
        assert_eq!(d.state().projected,3);
        assert!(d.join_group(&layout,9,0,false));
        assert_eq!(d.slot_offset(3,true,false,true),Some(-100.0));
        for (group,tab) in [(0,2),(7,999),(9,2),(99,0),(7,1)] {
            assert!(!d.join_group(&layout,group,tab,true));
            assert_eq!(d.state().projected,3);
        }
        d = drag(1);
        assert!(!d.join_group(&layout,7,3,true));
    }
    #[test]
    fn threshold_either_axis_and_continuation() {
        let mut d = TabDrag::default();
        d.arm(1,10.0,20.0);
        assert!(!d.ready(1,13.9,23.9,false));
        assert!(d.ready(1,14.0,20.0,false));
        assert!(d.ready(1,10.0,16.0,false));
        assert!(d.ready(1,10.0,20.0,true));
        assert!(!d.ready(2,14.0,20.0,true));
        assert!(!d.ready(1,f64::NAN,20.0,true));
        d.end(true);
        assert!(!d.ready(1,14.0,20.0,true));
    }
    #[test]
    fn projection_boundaries_offsets_and_landing() {
        let mut d = drag(0);
        d.update(150.0,16.0,300.0,40.0,true);
        assert_eq!(d.state().projected,1); // exact midpoint stays before the other row
        d.update(151.0,16.0,300.0,40.0,true);
        assert_eq!(d.state().projected,2);
        assert_eq!(d.offset(1),Some(-100.0));
        assert_eq!(d.offset(2),Some(0.0));
        assert_eq!(d.landing(),100.0);
        d.update(330.0,16.0,300.0,40.0,true);
        assert_eq!(d.state().projected,2); // clamp reaches final midpoint exactly
        let mut d = drag(2);
        d.update(0.0,16.0,300.0,40.0,true);
        assert_eq!(d.state().projected,0);
        assert_eq!(d.offset(0),Some(100.0));
        assert_eq!(d.offset(1),Some(100.0));
        assert_eq!(d.landing(),-200.0);
    }
    #[test]
    fn detach_tolerance_on_all_edges_and_reentry() {
        for (x,y) in [(-41.0,16.0),(341.0,16.0),(150.0,-41.0),(150.0,81.0)] {
            let mut d=drag(0); d.update(151.0,16.0,300.0,40.0,true);
            d.update(x,y,300.0,40.0,false);
            assert_eq!(d.state().detaching,1);
            assert_eq!(d.state().projected,0);
            d.update(151.0,16.0,300.0,40.0,true);
            assert_eq!(d.state().detaching,0);
            assert_eq!(d.state().projected,2);
        }
        let mut d=drag(0);
        for (x,y) in [(-40.0,16.0),(340.0,16.0),(150.0,-40.0),(150.0,80.0)] {
            d.update(x,y,300.0,40.0,false); assert_eq!(d.state().detaching,0);
        }
    }
    #[test]
    fn variable_width_reversed_geometry_and_narrow_strip() {
        let mut d=TabDrag::default(); d.arm(1,40.0,16.0);
        d.promote(0,&[ShellDragRow{left:0.0,width:80.0},ShellDragRow{left:90.0,width:120.0},ShellDragRow{left:225.0,width:60.0}]);
        d.update(200.0,16.0,400.0,40.0,true);
        assert_eq!(d.state().projected,2); assert_eq!(d.offset(1),Some(-90.0));
        assert_eq!(d.landing(),90.0);
        d.update(400.0,16.0,400.0,40.0,true);
        assert_eq!(d.state().projected,3); assert_eq!(d.landing(),225.0);
        d.arm(1,255.0,16.0);
        d.promote(0,&[ShellDragRow{left:225.0,width:60.0},ShellDragRow{left:90.0,width:120.0},ShellDragRow{left:0.0,width:80.0}]);
        d.update(100.0,16.0,400.0,40.0,true);
        assert_eq!(d.state().projected,2); assert_eq!(d.offset(1),Some(135.0));
        assert_eq!(d.landing(),-135.0);
        d.arm(1,50.0,16.0); d.promote(0,&[ShellDragRow{left:0.0,width:100.0}]);
        assert!(d.update(0.0,16.0,40.0,40.0,true));
        assert_eq!(d.state().projected,1); assert_eq!(d.landing(),0.0);
    }
    #[test]
    fn grouping_can_hold_projection_and_close_gap_without_ending() {
        let mut d=drag(0); d.update(151.0,16.0,300.0,40.0,true);
        d.update(10.0,16.0,300.0,40.0,false);
        assert_eq!(d.state().projected,2);
        d.close_gap(); assert_eq!(d.state().projected,0); assert_eq!(d.state().phase,2);
    }
    #[test]
    fn completion_is_not_order_confirmation_and_new_gesture_rejects_old_outcomes() {
        let mut d=drag(0); d.update(151.0,16.0,300.0,40.0,true); d.commit(10,3);
        d.complete(10,true); assert_eq!(d.state().awaiting,1);
        assert!(!d.publish(&[1,2,3])); assert!(d.publish(&[2,1,3]));
        assert_eq!(d.state().request,0);
        d=drag(0); d.commit(11,0);
        assert!(!d.publish(&[1,2,3])); assert!(d.publish(&[2,3,1]));
        d=drag(0); d.commit(12,3); assert!(d.publish(&[2,3]));
        d=drag(0); d.commit(13,3); d.complete(12,false); assert_eq!(d.state().awaiting,1);
        d.arm(2,150.0,16.0); d.complete(13,false); assert_eq!(d.state().phase,1);
        assert_eq!(d.state().tab,2);
    }
    #[test]
    fn malformed_geometry_and_points_do_not_mutate_state() {
        let mut d=drag(0);
        assert!(!d.update(f64::NAN,0.0,300.0,40.0,true));
        assert!(!d.update(20.0,0.0,-1.0,40.0,true));
        assert!(!d.arm(0,0.0,0.0)); assert_eq!(d.state().phase,2);
        d.arm(1,0.0,0.0);
        assert!(!d.promote(0,&[]));
        assert!(!d.promote(0,&[ShellDragRow{left:0.0,width:-1.0}]));
        assert!(!d.promote(0,&vec![ShellDragRow::default(); MAX_ROWS+1]));
        assert_eq!(d.state().phase,1); assert_eq!(d.offset(0),None);
        d.promote(0,&[ShellDragRow::default()]); d.update(0.0,0.0,0.0,0.0,true);
        assert_eq!(d.offset(0),Some(0.0)); assert_eq!(d.landing(),0.0);
        d.commit(0,0); assert_eq!(d.state().awaiting,0);
    }
}
