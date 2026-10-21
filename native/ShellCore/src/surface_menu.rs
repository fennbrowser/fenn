//! Window-scoped menu openings, independent of tab and address-query membership.
use std::collections::BTreeSet;
use crate::publication::{Classification, ShellPublicationCounts, Stream, StreamKind};
pub const MAX_ACTIONS: usize = 256;
pub const MAX_DESCRIPTION: usize = 65536;
pub struct SurfaceMenu {
    max_description: usize,
    next: u32,
    session: u32,
    /// Descriptions are scoped to one opening and reset with it.
    stream: Stream,
    opening_requests: Stream,
    requested_session: u32,
    actions: BTreeSet<u32>,
    pending: BTreeSet<u32>,
}
impl Default for SurfaceMenu {
    fn default() -> Self {
        Self { max_description: MAX_DESCRIPTION, next: 0, session: 0, stream: Stream::new(StreamKind::Session), opening_requests: Stream::new(StreamKind::Snapshot), requested_session: 0, actions: BTreeSet::new(), pending: BTreeSet::new() }
    }
}
impl SurfaceMenu {
    /// A separately bounded large list uses the same session semantics.
    pub fn with_description_limit(limit: usize) -> Self {Self {max_description:limit,..Self::default()}}
    pub fn open(&mut self) -> u32 {
        self.dismiss();
        if let Some(next)=self.next.checked_add(1) {self.next=next;self.session=next;}
        self.session
    }
    /// Browser-initiated openings share the Snapshot classifier. Replays never
    /// reopen a dismissed editor or retarget an intervening native opening.
    pub fn request_open(&mut self, request:u32, fingerprint:&[u16])->Result<u32,()> {
        if request==0 || fingerprint.len()>self.max_description{return Err(());}
        let class=self.opening_requests.classify(0,request,fingerprint);
        self.opening_requests.record(class);
        match class {
            Classification::Accepted=>{let session=self.open();self.opening_requests.commit(request,fingerprint);self.requested_session=session;Ok(session)},
            Classification::Unchanged=>Ok(if self.session==self.requested_session{self.session}else{0}),
            _=>{class.outcome(self.opening_requests.kind())?;Ok(0)},
        }
    }
    pub fn counts(&self) -> ShellPublicationCounts {self.stream.counts()}
    pub fn dismiss(&mut self) {self.session=0;self.stream.reset();self.actions.clear();self.pending.clear();}
    fn classify(&self, session:u32, revision:u32, actions:&[u32], fingerprint:&[u16]) -> Classification {
        if session==0 || revision==0 || actions.len()>MAX_ACTIONS || fingerprint.len()>self.max_description {return Classification::Invalid;}
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
        self.stream.commit(revision,fingerprint);self.actions=actions.iter().copied().collect();self.pending.clear();Ok(true)
    }
    /// A live panel stays open. Reject repeated activation until a newer reply.
    pub fn activate_retained(&mut self,session:u32,revision:u32,action:u32)->bool {
        session!=0 && session==self.session && revision==self.stream.revision() &&
            self.actions.contains(&action) && self.pending.insert(action)
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

#[cfg(test)]
mod retained_tests {
 use super::*;
 #[test] fn a_large_list_has_an_explicit_separate_description_bound() {
  let mut large=SurfaceMenu::with_description_limit(1048576);let mut small=SurfaceMenu::default();
  let l=large.open();let s=small.open();let body=vec![7;MAX_DESCRIPTION+1];
  assert_eq!(small.accept(s,1,&[1],&body),Err(()));
  assert_eq!(large.accept(l,1,&[1],&body),Ok(true));
  assert_eq!(large.accept(l,1,&[1],&vec![8;MAX_DESCRIPTION+1]),Err(()));
  assert_eq!(large.accept(l,2,&[1],&vec![7;1048577]),Err(()));
  assert!(large.activate_retained(l,1,1));assert!(!large.activate_retained(l,1,1));
  large.dismiss();assert!(!large.activate_retained(l,1,1));
 }
 #[test] fn browser_opening_requests_classify_replay_conflict_stale_and_dismissal() {
  let mut m=SurfaceMenu::default();let first=m.request_open(1,&[7]).unwrap();assert!(first>0);
  assert_eq!(m.request_open(1,&[7]),Ok(first));assert_eq!(m.request_open(1,&[8]),Err(()));
  m.dismiss();assert_eq!(m.request_open(1,&[7]),Ok(0));
  let second=m.request_open(2,&[8]).unwrap();assert!(second>first);
  assert_eq!(m.request_open(1,&[7]),Ok(0));m.open();assert_eq!(m.request_open(2,&[8]),Ok(0));
  assert_eq!(m.request_open(0,&[7]),Err(()));
 }
 #[test] fn live_actions_wait_for_a_new_reply_and_never_survive_dismissal() {
  let mut m=SurfaceMenu::default();let s=m.open();m.accept(s,1,&[1,2],&[8]).unwrap();
  assert!(m.activate_retained(s,1,1));assert!(!m.activate_retained(s,1,1));
  assert!(m.activate_retained(s,1,2));assert!(!m.activate_retained(s,2,1));
  assert_eq!(m.accept(s,1,&[1,2],&[8]),Ok(false));assert!(!m.activate_retained(s,1,1));
  m.accept(s,2,&[2],&[9]).unwrap();assert!(!m.activate_retained(s,2,1));assert!(m.activate_retained(s,2,2));
  m.dismiss();assert!(!m.activate_retained(s,2,2));assert_eq!(m.accept(s,3,&[2],&[10]),Ok(false));
 }
}
