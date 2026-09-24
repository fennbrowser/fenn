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
    pub fn close_gap(&mut self) { if self.state.phase == 2 { self.state.projected = self.state.original; } }
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
    pub fn end(&mut self, cancel: bool) {
        self.state.phase = 0;
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
