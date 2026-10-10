//! Window-scoped menu openings, independent of tab and address-query membership.
use std::collections::BTreeSet;
use crate::publication::{Classification, ShellPublicationCounts, Stream, StreamKind};
pub const MAX_ACTIONS: usize = 256;
pub const MAX_DESCRIPTION: usize = 65536;
pub struct SurfaceMenu {
    next: u32,
    session: u32,
    /// Descriptions are scoped to one opening and reset with it.
    stream: Stream,
    actions: BTreeSet<u32>,
}
impl Default for SurfaceMenu {
    fn default() -> Self {
        Self { next: 0, session: 0, stream: Stream::new(StreamKind::Session), actions: BTreeSet::new() }
    }
}
impl SurfaceMenu {
    pub fn open(&mut self) -> u32 {
        self.dismiss();
        if let Some(next)=self.next.checked_add(1) {self.next=next;self.session=next;}
        self.session
    }
    pub fn counts(&self) -> ShellPublicationCounts {self.stream.counts()}
    pub fn dismiss(&mut self) {self.session=0;self.stream.reset();self.actions.clear();}
    fn classify(&self, session:u32, revision:u32, actions:&[u32], fingerprint:&[u16]) -> Classification {
        if session==0 || revision==0 || actions.len()>MAX_ACTIONS || fingerprint.len()>MAX_DESCRIPTION {return Classification::Invalid;}
        let ids:BTreeSet<_>=actions.iter().copied().collect();
        if ids.contains(&0) || ids.len()!=actions.len(){return Classification::Invalid;}
        // An answer to a superseded opening is late, not wrong.
        if session!=self.session {return Classification::Stale;}
        // A replay must repeat the action set as well as the description.
        match self.stream.classify(0,revision,fingerprint) {
            Classification::Unchanged if ids!=self.actions => Classification::Conflict,
            class => class,
        }
    }
    /// Resolve before the host mutates any section of a containing publication.
    /// Accepted work is counted by accept; ignored/refused work ends here.
    pub fn resolve(&mut self, session:u32, revision:u32, actions:&[u32], fingerprint:&[u16]) -> Result<bool,()> {
        let class=self.classify(session,revision,actions,fingerprint);
        if class!=Classification::Accepted {self.stream.record(class);}
        class.outcome(self.stream.kind())
    }
    pub fn accept(&mut self, session:u32, revision:u32, actions:&[u32], fingerprint:&[u16]) -> Result<bool,()> {
        let class=self.classify(session,revision,actions,fingerprint);
        self.stream.record(class);
        if !class.outcome(self.stream.kind())? {return Ok(false);}
        self.stream.commit(revision,fingerprint);self.actions=actions.iter().copied().collect();Ok(true)
    }
    pub fn activate(&mut self,session:u32,revision:u32,action:u32)->bool {
        if session==0 || session!=self.session || revision!=self.stream.revision() || !self.actions.contains(&action){return false;}
        self.dismiss();true
    }
}
#[cfg(test)]
mod tests {
 use super::*;
 #[test] fn resolution_does_not_publish_or_replace_live_actions() {
  let mut m=SurfaceMenu::default();let s=m.open();
  assert_eq!(m.resolve(s,1,&[7],&[9]),Ok(true));assert!(!m.activate(s,1,7));
  assert_eq!(m.counts().accepted,0);
  assert_eq!(m.accept(s,1,&[7],&[9]),Ok(true));
  assert_eq!(m.resolve(s,1,&[7],&[9]),Ok(false));
  assert_eq!(m.resolve(s,1,&[8],&[9]),Err(()));
  assert_eq!(m.resolve(s,2,&[8,8],&[10]),Err(()));
  assert_eq!(m.resolve(s+1,2,&[8],&[10]),Ok(false));
  assert_eq!(m.resolve(s,2,&[8],&[10]),Ok(true));
  assert_eq!(m.counts().accepted,1);assert!(m.activate(s,1,7));
  assert_eq!(m.accept(s,2,&[8],&[10]),Ok(false));
 }
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
