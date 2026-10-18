//! Bounded, presentation-only toolbar layout. Firefox supplies eligibility.

#[repr(C)]
pub struct ShellToolbarPaletteOrder {
    pub order: [u32; 59],
    pub len: u32,
}

/// Fill each row with the earliest remaining item that fits. Composite spans
/// stay indivisible; singles can pass a composite to fill the preceding row.
/// This permutation is presentation-only and never changes Firefox placement.
pub fn palette_order(spans: &[u32], columns: u32) -> Option<ShellToolbarPaletteOrder> {
    if spans.len() > 59 || !(1..=16).contains(&columns)
        || spans.iter().any(|&span| ![1, 3].contains(&span) || span > columns) {
        return None;
    }
    let mut remaining: Vec<usize> = (0..spans.len()).collect();
    let mut result = ShellToolbarPaletteOrder { order: [0; 59], len: 0 };
    let mut space = columns;
    while !remaining.is_empty() {
        if let Some(index) = remaining.iter().position(|&i| spans[i] <= space) {
            let item = remaining.remove(index);
            result.order[result.len as usize] = item as u32;
            result.len += 1;
            space -= spans[item];
        } else {
            space = columns;
        }
    }
    Some(result)
}

#[cfg(test)]
#[test]
fn palette_singles_fill_gaps_without_splitting_groups() {
    let spans = [1,1,1,1,1,3,3,1,1,1];
    let order = palette_order(&spans,7).unwrap();
    assert_eq!(&order.order[..order.len as usize], &[0,1,2,3,4,7,8,5,6,9]);
    let groups = palette_order(&[3,3,3],7).unwrap();
    assert_eq!(&groups.order[..3], &[0,1,2]);
}

#[cfg(test)]
#[test]
fn palette_packing_preserves_every_item_and_fills_available_cells() {
    for bits in 0..1024 {
        let spans: Vec<u32> = (0..10).map(|i| if bits & (1<<i) == 0 {1} else {3}).collect();
        for columns in 3..=9 {
            let order = palette_order(&spans,columns).unwrap();
            let mut remaining: Vec<usize> = (0..spans.len()).collect();
            let mut space = columns;
            for &item in &order.order[..order.len as usize] {
                if spans[item as usize] > space {
                    assert!(remaining.iter().all(|&i| spans[i] > space));
                    space = columns;
                }
                let index = remaining.iter().position(|&i| i == item as usize).unwrap();
                remaining.remove(index);
                space -= spans[item as usize];
            }
            assert!(remaining.is_empty());
            for span in [1,3] {
                let same_kind: Vec<u32> = order.order[..order.len as usize].iter()
                    .copied().filter(|&i| spans[i as usize] == span).collect();
                assert!(same_kind.windows(2).all(|pair| pair[0] < pair[1]));
            }
        }
    }
}

#[cfg(test)]
#[test]
fn palette_bounds_and_empty_inventory() {
    assert_eq!(palette_order(&[],7).unwrap().len,0);
    assert_eq!(palette_order(&[1;59],7).unwrap().len,59);
    for (spans,columns) in [(&[1;60][..],7),(&[0][..],7),(&[2][..],7),(&[3][..],2),(&[1][..],0),(&[1][..],17)] {
        assert!(palette_order(spans,columns).is_none());
    }
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellToolbarSlot {
    /// 0 button, 1 address, 2 flexible space, 3 three-button stock group.
    pub kind: u32,
    /// 0 retained, 1 general overflow, 2 Firefox Extensions panel.
    pub overflow: u32,
}

pub fn plan(slots: &[ShellToolbarSlot], width: f64, required_menu: bool) -> Option<u64> {
    if slots.len() > 55 || !width.is_finite() || !(0.0..=100000.0).contains(&width)
        || slots.iter().any(|s| s.kind > 3 || s.overflow > 2 || ((s.kind == 1 || s.kind == 2) && s.overflow != 0)) {
        return None;
    }
    let used = |mask: u64| {
        let mut total = 16.0; // outer margins
        let mut previous_button = false;
        for (i, slot) in slots.iter().enumerate() {
            if mask & (1u64 << i) != 0 { continue; }
            let button = slot.kind == 0 || slot.kind == 3;
            if previous_button && button { total += 4.0; }
            total += match slot.kind { 0 => 32.0, 1 => 256.0, 3 => 104.0, _ => 1.0 };
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
    #[test] fn stock_groups_overflow_atomically_at_their_full_width() {
        let r=[ShellToolbarSlot{kind:1,overflow:0},ShellToolbarSlot{kind:3,overflow:1}];
        assert_eq!(plan(&r,412.0,false),Some(0));
        assert_eq!(plan(&r,411.0,false),Some(2));
        assert_eq!(plan(&r,0.0,false),Some(2));
        assert_eq!(plan(&[ShellToolbarSlot{kind:4,overflow:1}],500.0,false),None);
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

#[repr(C)]
pub struct ShellToolbarPreview { pub order: [u32;56], pub len: u32 }
/// Stable logical insertion: source == len denotes a new palette item.
pub fn preview(len:usize,source:usize,target:usize)->Option<ShellToolbarPreview> {
    if len>55 || source>len || target>len {return None;}
    let mut result=ShellToolbarPreview{order:[0;56],len:0};
    for i in 0..=len {
        if i==target {result.order[result.len as usize]=source as u32;result.len+=1;}
        if i<len && i!=source {result.order[result.len as usize]=i as u32;result.len+=1;}
    }
    Some(result)
}
#[cfg(test)]
#[test]
fn preview_orders_cover_palette_moves_and_noops() {
    let order=|n,s,t|{let p=preview(n,s,t).unwrap();p.order[..p.len as usize].to_vec()};
    assert_eq!(order(4,0,3),vec![1,2,0,3]);
    assert_eq!(order(4,3,0),vec![3,0,1,2]);
    assert_eq!(order(4,1,2),vec![0,1,2,3]);
    assert_eq!(order(4,1,1),vec![0,1,2,3]);
    assert_eq!(order(4,4,2),vec![0,1,4,2,3]);
    assert_eq!(order(0,0,0),vec![0]);
    assert_eq!(order(55,55,55).len(),56);
    assert!(preview(56,0,0).is_none());assert!(preview(4,5,0).is_none());assert!(preview(4,0,5).is_none());
    for n in 1..=55 {for s in 0..=n {for t in 0..=n {
        let mut actual=order(n,s,t);actual.sort_unstable();
        assert_eq!(actual,(0..(n+usize::from(s==n)) as u32).collect::<Vec<_>>());
    }}}
}
