//! Window-scoped menu openings, independent of tab and address-query membership.
use std::collections::BTreeSet;
pub const MAX_ACTIONS: usize = 256;
pub const MAX_DESCRIPTION: usize = 65536;
#[derive(Default)]
pub struct SurfaceMenu {
    next: u32,
    session: u32,
    revision: u32,
    fingerprint: Vec<u16>,
    actions: BTreeSet<u32>,
}
impl SurfaceMenu {
    pub fn open(&mut self) -> u32 {
        self.dismiss();
        if let Some(next)=self.next.checked_add(1) {self.next=next;self.session=next;}
        self.session
    }
    pub fn dismiss(&mut self) {self.session=0;self.revision=0;self.actions.clear();self.fingerprint.clear();}
    pub fn accept(&mut self, session:u32, revision:u32, actions:&[u32], fingerprint:&[u16]) -> Result<bool,()> {
        if session==0 || revision==0 || actions.len()>MAX_ACTIONS || fingerprint.len()>MAX_DESCRIPTION {return Err(());}
        let ids:BTreeSet<_>=actions.iter().copied().collect();
        if ids.contains(&0) || ids.len()!=actions.len(){return Err(());}
        if session!=self.session || revision<self.revision {return Ok(false);}
        if revision==self.revision {return if fingerprint==self.fingerprint && ids==self.actions {Ok(false)}else{Err(())};}
        self.revision=revision;self.actions=ids;self.fingerprint=fingerprint.to_vec();Ok(true)
    }
    pub fn activate(&mut self,session:u32,revision:u32,action:u32)->bool {
        if session==0 || session!=self.session || revision!=self.revision || !self.actions.contains(&action){return false;}
        self.dismiss();true
    }
}
#[cfg(test)]
mod tests {
 use super::*;
 #[test] fn stale_and_duplicate_activation() {
  let mut m=SurfaceMenu::default();let a=m.open();assert_eq!(m.accept(a,1,&[1,2],&[7]),Ok(true));
  assert!(!m.activate(a,2,1));assert!(!m.activate(a,1,3));assert!(m.activate(a,1,2));assert!(!m.activate(a,1,2));
  assert_eq!(m.accept(a,2,&[1],&[8]),Ok(false));let b=m.open();assert!(b>a);assert_eq!(m.accept(a,3,&[1],&[]),Ok(false));
 }
 #[test] fn revision_conflicts_and_validation_are_atomic() {
  let mut m=SurfaceMenu::default();let s=m.open();assert_eq!(m.accept(s,1,&[1],&[2]),Ok(true));
  assert_eq!(m.accept(s,1,&[1],&[2]),Ok(false));assert_eq!(m.accept(s,1,&[2],&[2]),Err(()));
  assert_eq!(m.accept(s,2,&[1,1],&[]),Err(()));assert_eq!(m.accept(s,2,&[0],&[]),Err(()));
  assert_eq!(m.accept(s,2,&[1],&vec![0;MAX_DESCRIPTION+1]),Err(()));
  assert_eq!(m.accept(s,2,&(1..=257).collect::<Vec<_>>(),&[]),Err(()));assert!(m.activate(s,1,1));
 }
 #[test] fn close_and_exhaustion_do_not_reuse_ids() {
  let mut m=SurfaceMenu::default();let s=m.open();m.dismiss();assert_eq!(m.accept(s,1,&[1],&[]),Ok(false));
  assert!(m.open()>s);m.next=u32::MAX;assert_eq!(m.open(),0);
 }
}
