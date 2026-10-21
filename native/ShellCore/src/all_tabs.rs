//! Vertical list projection; WinUI supplies only realized screen rectangles.
use std::collections::BTreeSet;

pub const MAX_ROWS: usize = 2048;
#[repr(C)]
#[derive(Clone,Copy,Debug)]
pub struct ShellAllTabsRow {pub id:u32,pub top:f64,pub height:f64}

pub fn project(rows:&[ShellAllTabsRow],y:f64,top:f64,bottom:f64)->Result<Option<(u32,bool)>,()> {
    let finite=|v:f64|v.is_finite() && v.abs()<=1_000_000.0;
    if rows.len()>MAX_ROWS || !finite(y) || !finite(top) || !finite(bottom) || bottom<=top{return Err(());}
    let mut ids=BTreeSet::new();
    for row in rows {if row.id==0 || !ids.insert(row.id) || !finite(row.top) || !finite(row.height) || row.height<=0.0 || !finite(row.top+row.height){return Err(());}}
    if y<top || y>=bottom{return Ok(None);}
    Ok(rows.iter().filter(|r|r.top+r.height>top && r.top<bottom)
        .min_by(|a,b|(y-a.top-a.height/2.0).abs().total_cmp(&(y-b.top-b.height/2.0).abs()).then(a.id.cmp(&b.id)))
        .map(|r|(r.id,y<r.top+r.height/2.0)))
}

#[cfg(test)]
mod tests {
 use super::*;
 #[test] fn projection_follows_screen_order_not_recycled_container_or_id_order(){
  let rows=[ShellAllTabsRow{id:9,top:100.0,height:40.0},ShellAllTabsRow{id:2,top:140.0,height:40.0}];
  assert_eq!(project(&rows,102.0,100.0,180.0),Ok(Some((9,true))));
  assert_eq!(project(&rows,138.0,100.0,180.0),Ok(Some((9,false))));
  assert_eq!(project(&rows,150.0,100.0,180.0),Ok(Some((2,true))));
  assert_eq!(project(&rows,179.0,100.0,180.0),Ok(Some((2,false))));
  assert_eq!(project(&rows,99.0,100.0,180.0),Ok(None));
  assert_eq!(project(&rows,180.0,100.0,180.0),Ok(None));
 }
 #[test] fn clipping_and_malformed_geometry_do_not_invent_targets(){
  let row=ShellAllTabsRow{id:7,top:0.0,height:40.0};
  assert_eq!(project(&[row],120.0,100.0,180.0),Ok(None));
  assert_eq!(project(&[],120.0,100.0,180.0),Ok(None));
  for bad in [ShellAllTabsRow{id:0,..row},ShellAllTabsRow{height:0.0,..row},ShellAllTabsRow{top:f64::NAN,..row}]{assert_eq!(project(&[bad],1.0,0.0,40.0),Err(()));}
  assert_eq!(project(&[row,row],1.0,0.0,40.0),Err(()));
  assert_eq!(project(&[row],f64::INFINITY,0.0,40.0),Err(()));
  assert_eq!(project(&vec![row;MAX_ROWS+1],1.0,0.0,40.0),Err(()));
 }
}
