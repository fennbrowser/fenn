//! Bounded, presentation-only toolbar overflow. Firefox supplies eligibility.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellToolbarSlot {
    /// 0 button, 1 address, 2 flexible space.
    pub kind: u32,
    /// 0 retained, 1 general overflow, 2 Firefox Extensions panel.
    pub overflow: u32,
}

pub fn plan(slots: &[ShellToolbarSlot], width: f64, required_menu: bool) -> Option<u64> {
    if slots.len() > 55 || !width.is_finite() || !(0.0..=100000.0).contains(&width)
        || slots.iter().any(|s| s.kind > 2 || s.overflow > 2 || (s.kind != 0 && s.overflow != 0)) {
        return None;
    }
    let used = |mask: u64| {
        let mut total = 16.0; // outer margins
        let mut previous_button = false;
        for (i, slot) in slots.iter().enumerate() {
            if mask & (1u64 << i) != 0 { continue; }
            let button = slot.kind == 0;
            if previous_button && button { total += 4.0; }
            total += match slot.kind { 0 => 32.0, 1 => 256.0, _ => 1.0 };
            previous_button = button;
        }
        let menu = required_menu || slots.iter().enumerate().any(|(i,s)| s.overflow == 1 && mask & (1u64 << i) != 0);
        if menu { if previous_button { total += 4.0; } total += 32.0; previous_button = true; }
        if previous_button { total += 4.0; }
        total + 32.0 // fixed application menu
    };
    let mut mask = 0;
    for i in (0..slots.len()).rev() {
        if used(mask) <= width { break; }
        if slots[i].overflow != 0 { mask |= 1u64 << i; }
    }
    Some(mask)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn row() -> Vec<ShellToolbarSlot> {
        vec![ShellToolbarSlot{kind:0,overflow:0},ShellToolbarSlot{kind:1,overflow:0},
             ShellToolbarSlot{kind:0,overflow:1},ShellToolbarSlot{kind:0,overflow:2}]
    }
    #[test] fn boundaries_and_restoration() {
        let r=row();assert_eq!(plan(&r,408.0,false),Some(0));
        assert_eq!(plan(&r,407.0,false),Some(8));
        assert_eq!(plan(&r,371.0,false),Some(12));
        assert_eq!(plan(&r,1000.0,false),Some(0));
        assert_eq!(plan(&r,0.0,false),Some(12)); // mandatory controls never hidden
    }
    #[test] fn menu_cost_and_extension_destination() {
        let r=row();assert_eq!(plan(&r,408.0,true),Some(8));
        // A general item needs a menu button; removing it alone saves no width.
        assert_eq!(plan(&r[..3],371.0,false),Some(4));
    }
    #[test] fn bounded_and_invalid_inputs() {
        assert_eq!(plan(&row(),f64::NAN,false),None);
        assert_eq!(plan(&row(),-1.0,false),None);
        assert_eq!(plan(&vec![ShellToolbarSlot{kind:0,overflow:2};56],500.0,false),None);
        assert_eq!(plan(&[ShellToolbarSlot{kind:2,overflow:1}],500.0,false),None);
        let r=vec![ShellToolbarSlot{kind:0,overflow:2};55];
        assert_eq!(plan(&r,0.0,false),Some((1u64<<55)-1));
    }
    #[test] fn spaces_shrink_before_buttons_and_width_is_monotone() {
        let r=vec![ShellToolbarSlot{kind:2,overflow:0},ShellToolbarSlot{kind:1,overflow:0},ShellToolbarSlot{kind:0,overflow:2}];
        assert_eq!(plan(&r,341.0,false),Some(0));
        let mut previous=plan(&r,0.0,false).unwrap();
        for width in 1..1000 {let mask=plan(&r,width as f64,false).unwrap();assert_eq!(mask & !previous,0);previous=mask;}
    }
}


#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShellToolbarDropSlot { pub left: f64, pub width: f64 }

/// Return a logical insertion index; len means append. Geometry is in DIPs.
pub fn drop_target(slots: &[ShellToolbarDropSlot], x: f64, rtl: bool) -> Option<usize> {
    if slots.len()>55 || !x.is_finite() || x.abs()>100000.0 || slots.iter().any(|s|
        !s.left.is_finite() || s.left.abs()>100000.0 || !s.width.is_finite() || !(0.0..=100000.0).contains(&s.width)) { return None; }
    Some(slots.iter().position(|s| if rtl { x>s.left+s.width/2.0 } else { x<s.left+s.width/2.0 }).unwrap_or(slots.len()))
}
#[cfg(test)]
#[test]
fn toolbar_drop_positions_cover_edges_gaps_and_rtl() {
    let rows=[ShellToolbarDropSlot{left:8.0,width:32.0},ShellToolbarDropSlot{left:48.0,width:240.0}];
    assert_eq!(drop_target(&rows,0.0,false),Some(0));
    assert_eq!(drop_target(&rows,24.0,false),Some(1));
    assert_eq!(drop_target(&rows,400.0,false),Some(2));
    let rtl=[rows[1],rows[0]];
    assert_eq!(drop_target(&rtl,400.0,true),Some(0));
    assert_eq!(drop_target(&rtl,168.0,true),Some(1));
    assert_eq!(drop_target(&rtl,0.0,true),Some(2));
    assert_eq!(drop_target(&rows,f64::NAN,false),None);
    assert_eq!(drop_target(&[ShellToolbarDropSlot{left:0.0,width:-1.0}],1.0,false),None);
    assert_eq!(drop_target(&vec![rows[0];56],1.0,false),None);
}
