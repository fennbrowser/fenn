// Copyright Fenn contributors. Licensed under MPL-2.0.
//! Bounded local projection for a native TSF adapter. Gecko owns document edits.
use std::collections::VecDeque;
use crate::publication::{Stream, StreamKind};

pub const PAGE_INPUT_MAX_TEXT: usize = 65536;
pub const PAGE_INPUT_MAX_EVENTS: usize = 128;
/// The event kind that tells Gecko a publication was accepted (`revision`).
pub const ACCEPTED: u32 = 7;
const MAX_QUEUED_TEXT: usize = 262144;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ShellPageInputState {
    pub session: u64,
    pub revision: u64,
    pub edit: u64,
    pub start: u32,
    pub end: u32,
    pub length: u32,
}
#[derive(Clone, Debug)]
pub struct Event { pub clauses:Vec<ShellPageClause>, pub session:u64, pub edit:u64, pub revision:u64, pub request:u64, pub kind:u32, pub start:u32, pub end:u32, pub caret:u32, pub text:Vec<u16> }
pub struct PageInput {
    pub state:ShellPageInputState,
    pub text:Vec<u16>,
    stream:Stream<u64>,
    events:VecDeque<Event>,
    queued_text:usize,
    geometry:Geometry,
}
impl Default for PageInput {
    fn default()->Self { Self {state:Default::default(),text:vec![],stream:Stream::new(StreamKind::Session),events:VecDeque::new(),queued_text:0,geometry:Geometry::default()} }
}
impl PageInput {
    fn valid(text:&[u16],start:u32,end:u32)->bool {
        start<=end && end as usize<=text.len() && text.len()<=PAGE_INPUT_MAX_TEXT
    }
    pub fn publish(&mut self,session:u64,revision:u64,ack:u64,start:u32,end:u32,text:&[u16])->Result<bool,()> {
        if session==0 || revision==0 || session<self.state.session || !Self::valid(text,start,end) {return Err(());}
        // Reserve the acceptance event before anything changes: Gecko adopts a text base only when
        // it reads one, so an accepted publication it cannot observe would split the two offsets.
        if session==self.state.session && self.events.len()==PAGE_INPUT_MAX_EVENTS &&
            !self.events.back().is_some_and(|e|e.kind==ACCEPTED) {return Err(());}
        if session>self.state.session {
            if ack!=0 {return Err(());}
            self.state=ShellPageInputState{session,..Default::default()};
            self.events.clear();self.queued_text=0;self.stream.reset();
        }
        if ack>self.state.edit {return Err(());}
        // A drained queue is not an acknowledgment of browser processing.
        if ack<self.state.edit {self.accepted(revision);return Ok(false);}
        let mut fingerprint=text.to_vec();
        for value in [ack,start as u64,end as u64] {fingerprint.extend(value.to_le_bytes().chunks_exact(2).map(|b|u16::from_le_bytes([b[0],b[1]])));}
        if !self.stream.check(0,revision,&fingerprint)? {self.accepted(revision);return Ok(false);}
        self.text=text.to_vec();self.state.start=start;self.state.end=end;self.state.length=text.len() as u32;self.state.revision=revision;
        self.stream.commit(revision,&fingerprint);self.accepted(revision);Ok(true)
    }
    // Gecko's publications are posted, so it learns that one was taken from the event queue, in
    // order with the edits made against it. It is not an edit and takes no edit number.
    fn accepted(&mut self,revision:u64) {
        if self.events.back().is_some_and(|e|e.kind==ACCEPTED) {self.events.pop_back();}
        self.events.push_back(Event{clauses:vec![],session:self.state.session,edit:self.state.edit,revision,request:0,kind:ACCEPTED,start:0,end:0,caret:0,text:vec![]});
    }
    pub fn geometry_pending(&self)->bool {self.geometry.entries.iter().any(|e|!e.drained)}
    pub fn select(&mut self,start:u32,end:u32)->bool {
        if !Self::valid(&self.text,start,end) {return false;}
        if self.state.start!=start || self.state.end!=end {self.geometry.entries.clear();}
        self.state.start=start;self.state.end=end;true
    }
    pub fn replace(&mut self,start:u32,end:u32,text:&[u16])->bool {
        if !Self::valid(&self.text,start,end) || text.len()>PAGE_INPUT_MAX_TEXT || self.text.len()-(end-start) as usize+text.len()>PAGE_INPUT_MAX_TEXT {return false;}
        self.geometry.entries.clear();
        self.text.splice(start as usize..end as usize,text.iter().copied());
        self.state.start=start+text.len() as u32;self.state.end=self.state.start;self.state.length=self.text.len() as u32;true
    }
    pub fn enqueue(&mut self,kind:u32,start:u32,end:u32,caret:u32,text:&[u16])->bool {
        if self.state.session==0 || !(1..=5).contains(&kind) || start>end || end as usize>PAGE_INPUT_MAX_TEXT || text.len()>PAGE_INPUT_MAX_TEXT || (if kind==5 {caret>1 || !text.is_empty()} else {caret as usize>text.len()}) {return false;}
        let Some(edit)=self.state.edit.checked_add(1) else {return false;};
        // Coalesce only adjacent updates, never across a start/end boundary.
        let replace=kind==2 && self.events.back().is_some_and(|e|e.kind==2);
        let previous=if replace {self.events.back().unwrap().text.len()} else {0};
        if (!replace && self.events.len()==PAGE_INPUT_MAX_EVENTS) || self.queued_text-previous+text.len()>MAX_QUEUED_TEXT {return false;}
        if replace {self.events.pop_back();}
        self.queued_text=self.queued_text-previous+text.len();
        self.events.push_back(Event{clauses:vec![],session:self.state.session,edit,revision:0,request:0,kind,start,end,caret,text:text.to_vec()});self.state.edit=edit;true
    }
    // Reserve the semantic event before mutating the optimistic projection.
    // Queue pressure must never leave an edit that Gecko cannot observe.
    pub fn composition(&mut self,start:u32,end:u32,caret:u32,text:&[u16],clauses:&[ShellPageClause])->bool {
        if clauses.len()>64 {return false;}
        let mut position=0;
        for c in clauses {
            if c.start!=position || c.end<=c.start || c.end as usize>text.len() ||
                !(1..=4).contains(&c.kind) || c.flags>15 || c.line>4 || c.bold>1 {return false;}
            position=c.end;
        }
        if !clauses.is_empty() && position as usize!=text.len() {return false;}
        if !self.enqueue(2,start,end,caret,text) {return false;}
        self.events.back_mut().unwrap().clauses=clauses.to_vec();true
    }
    pub fn direct_replace(&mut self,start:u32,end:u32,text:&[u16])->bool {
        if !Self::valid(&self.text,start,end) || text.len()>PAGE_INPUT_MAX_TEXT || self.text.len()-(end-start) as usize+text.len()>PAGE_INPUT_MAX_TEXT {return false;}
        if !self.enqueue(4,start,end,0,text) {return false;}
        self.replace(start,end,text)
    }
    pub fn direct_select(&mut self,start:u32,end:u32,reversed:u32)->bool {
        if !Self::valid(&self.text,start,end) || reversed>1 {return false;}
        if !self.enqueue(5,start,end,reversed,&[]) {return false;}
        self.select(start,end)
    }
    pub fn geometry_context(&mut self,session:u64,revision:u64)->Result<bool,()> {
        if session!=self.state.session {return Err(());}
        self.geometry.context(session,revision,self.state.edit,self.state.length)
    }
    pub fn geometry_read(&mut self,start:u32,end:u32)->Result<Option<ShellPageGeometry>,()> {
        if start>end || end>self.state.length {return Err(());}
        self.geometry.sync_edit(self.state.edit);
        self.geometry.read(start,end)
    }
    pub fn geometry_reply(&mut self,reply:ShellPageGeometry)->bool {
        self.geometry.sync_edit(self.state.edit);
        self.geometry.reply(reply)
    }
    pub fn front(&self)->Option<&Event> {
        self.events.front().or_else(||self.geometry.entries.iter().find(|e|!e.drained).map(|e|&e.event))
    }
    pub fn pop(&mut self) {
        if let Some(event)=self.events.pop_front(){self.queued_text-=event.text.len();}
        else if let Some(entry)=self.geometry.entries.iter_mut().find(|e|!e.drained){entry.drained=true;}
    }
}
// Tests written before acceptance events existed check the edit queue alone.
#[cfg(test)] impl PageInput {
 fn quiet(&mut self,session:u64,revision:u64,ack:u64,start:u32,end:u32,text:&[u16])->Result<bool,()> {
  let result=self.publish(session,revision,ack,start,end,text);self.events.retain(|e|e.kind!=ACCEPTED);result
 }
}
#[cfg(test)] mod tests {
 use super::*;
 fn kinds(p:&PageInput)->Vec<u32> {p.events.iter().map(|e|e.kind).collect()}
 #[test] fn acceptance_is_ordered_with_edits_and_coalesced() {
  let mut p=PageInput::default();assert_eq!(p.publish(1,1,0,0,0,&[]),Ok(true));
  assert_eq!(kinds(&p),vec![ACCEPTED]);assert_eq!(p.front().unwrap().revision,1);
  assert_eq!(p.publish(1,2,0,0,0,&[65]),Ok(true));assert_eq!(kinds(&p),vec![ACCEPTED]);assert_eq!(p.front().unwrap().revision,2);
  assert!(p.enqueue(1,0,0,0,&[]));assert_eq!(p.state.edit,1);
  // Unacknowledged edits keep the old text but Gecko's revision still counts, as before.
  assert_eq!(p.publish(1,3,0,0,0,&[65]),Ok(false));assert_eq!(kinds(&p),vec![ACCEPTED,1,ACCEPTED]);
  assert_eq!(p.events.back().unwrap().edit,1);
  assert!(p.publish(1,4,2,0,0,&[]).is_err());assert_eq!(kinds(&p),vec![ACCEPTED,1,ACCEPTED]);
 }
 #[test] fn acceptance_is_reserved_before_the_text_changes() {
  let mut p=PageInput::default();p.publish(1,1,0,0,0,&[]).unwrap();p.pop();
  while p.events.len()<PAGE_INPUT_MAX_EVENTS {assert!(p.enqueue(1,0,0,0,&[]));}
  let edit=p.state.edit;
  assert!(p.publish(1,2,edit,0,1,&[66]).is_err());assert!(p.text.is_empty());
  while p.front().is_some() {p.pop();}
  assert_eq!(p.publish(1,3,edit,0,1,&[66]),Ok(true));assert_eq!(p.text,vec![66]);
  assert_eq!(p.publish(2,1,0,0,0,&[]),Ok(true));assert_eq!(kinds(&p),vec![ACCEPTED]);
 }
 #[test] fn draining_does_not_acknowledge() {
  let mut p=PageInput::default();assert_eq!(p.quiet(1,1,0,0,0,&[]),Ok(true));
  assert!(p.enqueue(1,0,0,0,&[]));p.pop();assert!(p.replace(0,0,&[0x65e5]));
  assert_eq!(p.quiet(1,2,0,0,0,&[]),Ok(false));assert_eq!(p.text,vec![0x65e5]);
  assert_eq!(p.quiet(1,3,1,1,1,&[0x65e5]),Ok(true));
 }
 #[test] fn sessions_revoke_events_and_old_snapshots() {
  let mut p=PageInput::default();p.quiet(3,1,0,0,0,&[]).unwrap();assert!(p.enqueue(1,0,0,0,&[]));
  p.quiet(4,1,0,0,0,&[]).unwrap();assert!(p.front().is_none());assert!(p.quiet(3,9,0,0,0,&[]).is_err());
 }
 #[test] fn edits_are_utf16_and_bounds_are_atomic() {
  let mut p=PageInput::default();p.quiet(1,1,0,0,0,&[0xd83d,0xde00]).unwrap();
  assert!(!p.replace(2,1,&[]));assert_eq!(p.text.len(),2);
  assert!(!p.replace(0,0,&vec![1;PAGE_INPUT_MAX_TEXT]));assert_eq!(p.text.len(),2);
  assert!(p.replace(0,2,&[0x65e5]));assert_eq!(p.state.start,1);
 }
 #[test] fn coalescing_preserves_composition_boundaries_and_limits() {
  let mut p=PageInput::default();p.quiet(1,1,0,0,0,&[]).unwrap();
  assert!(p.enqueue(1,0,0,0,&[]));for i in 0..1000 {assert!(p.enqueue(2,0,0,1,&[i]));}
  assert_eq!(p.events.len(),2);assert!(p.enqueue(3,0,0,1,&[9]));assert_eq!(p.events.len(),3);
  while p.events.len()<PAGE_INPUT_MAX_EVENTS {assert!(p.enqueue(1,0,0,0,&[]));}
  let before=p.state.edit;assert!(!p.enqueue(3,0,0,0,&[]));assert_eq!(p.state.edit,before);
 }
}

#[cfg(test)] mod direct_edit_tests {
 use super::*;
 #[test] fn replacement_and_backward_selection_are_ordered_and_acknowledged() {
  let mut p=PageInput::default();p.quiet(1,1,0,1,2,&[65,66,67]).unwrap();
  assert!(p.direct_replace(1,2,&[0x65e5,0x672c]));
  assert!(p.direct_select(1,3,1));
  let a=p.front().unwrap();assert_eq!((a.kind,a.start,a.end,a.text.clone()),(4,1,2,vec![0x65e5,0x672c]));p.pop();
  let b=p.front().unwrap();assert_eq!((b.kind,b.start,b.end,b.caret),(5,1,3,1));p.pop();
  assert_eq!(p.quiet(1,2,0,1,2,&[65,66,67]),Ok(false));
  assert_eq!(p.quiet(1,3,2,1,3,&[65,0x65e5,0x672c,67]),Ok(true));
 }
 #[test] fn rejected_commands_leave_projection_and_sequence_unchanged() {
  let mut p=PageInput::default();p.quiet(1,1,0,0,0,&[65]).unwrap();
  assert!(!p.direct_select(0,1,2));assert!(!p.direct_replace(0,2,&[]));
  for _ in 0..PAGE_INPUT_MAX_EVENTS {assert!(p.direct_select(0,0,0));}
  let state=p.state;assert!(!p.direct_replace(0,1,&[66]));assert!(!p.direct_select(0,1,1));
  assert_eq!(p.text,vec![65]);assert_eq!(p.state.edit,state.edit);assert_eq!(p.state.end,state.end);
 }
}

// Bounded on-demand geometry is independent of semantic edit acknowledgment.
// A layout publication uses the shared session-stream revision semantics.
const MAX_GEOMETRY:usize=16;
#[repr(C)]
#[derive(Clone,Copy,Default,Debug,PartialEq,Eq)]
pub struct ShellPageGeometry {
    pub session:u64, pub revision:u64, pub edit:u64, pub request:u64,
    pub start:u32, pub end:u32, pub valid:u32, pub clipped:u32,
    pub left:i32, pub top:i32, pub right:i32, pub bottom:i32,
}
struct GeometryEntry {event:Event,drained:bool,reply:Option<ShellPageGeometry>}
struct Geometry {
    session:u64, revision:u64, edit:u64, next:u64,
    stream:Stream<u64>, entries:VecDeque<GeometryEntry>,
}
impl Default for Geometry {
    fn default()->Self {Self{session:0,revision:0,edit:0,next:0,stream:Stream::new(StreamKind::Session),entries:VecDeque::new()}}
}
impl Geometry {
    fn context(&mut self,session:u64,revision:u64,edit:u64,length:u32)->Result<bool,()> {
        if session==0 || session<self.session || revision==0 {return Err(());}
        if session!=self.session {self.stream.reset();self.entries.clear();self.session=session;}
        if !self.stream.check(0,revision,&[])? {return Ok(false);}
        let ranges:Vec<_>=if self.edit==edit {self.entries.iter().filter(|e|e.event.end<=length).map(|e|(e.event.start,e.event.end)).collect()} else {vec![]};
        self.stream.commit(revision,&[]);self.revision=revision;self.edit=edit;self.entries.clear();
        // A layout revision invalidates rectangles, not the TIP's interest in
        // those ranges. Refresh them before issuing another layout notification.
        for (start,end) in ranges {self.read(start,end)?;}
        Ok(true)
    }
    fn sync_edit(&mut self,edit:u64) {if self.edit!=edit {self.edit=edit;self.entries.clear();}}
    fn read(&mut self,start:u32,end:u32)->Result<Option<ShellPageGeometry>,()> {
        if self.session==0 || self.revision==0 {return Err(());}
        if let Some(entry)=self.entries.iter().find(|e|e.event.start==start && e.event.end==end) {
            return Ok(entry.reply.filter(|r|r.valid==1));
        }
        if self.entries.len()==MAX_GEOMETRY {
            if let Some(i)=self.entries.iter().position(|e|e.reply.is_some()){self.entries.remove(i);}
            else {return Ok(None);}
        }
        self.next=self.next.checked_add(1).ok_or(())?;
        self.entries.push_back(GeometryEntry{event:Event{clauses:vec![],session:self.session,edit:self.edit,revision:self.revision,
            request:self.next,kind:6,start,end,caret:0,text:vec![]},drained:false,reply:None});
        Ok(None)
    }
    fn reply(&mut self,reply:ShellPageGeometry)->bool {
        if reply.session!=self.session || reply.revision!=self.revision || reply.edit!=self.edit || reply.valid>1 || reply.clipped>1 ||
            (reply.valid==1 && (reply.right<reply.left || reply.bottom<reply.top)) {return false;}
        let Some(entry)=self.entries.iter_mut().find(|e|e.event.request==reply.request && e.event.start==reply.start && e.event.end==reply.end) else {return false;};
        if !entry.drained {return false;}
        if let Some(old)=entry.reply {return old==reply;}
        entry.reply=Some(reply);true
    }
}
#[cfg(test)] mod geometry_tests {
 use super::*;
 fn ready()->PageInput {let mut p=PageInput::default();p.quiet(1,1,0,0,0,&[65;40]).unwrap();p.geometry_context(1,1).unwrap();p}
 fn answer(p:&PageInput)->ShellPageGeometry {let e=p.front().unwrap();ShellPageGeometry{session:e.session,revision:e.revision,edit:e.edit,request:e.request,start:e.start,end:e.end,valid:1,left:10,top:20,right:60,bottom:40,..Default::default()}}
 #[test] fn layout_refresh_retains_interest_but_never_old_rectangles() {
  let mut p=ready();assert_eq!(p.geometry_read(2,8),Ok(None));let old=answer(&p);p.pop();assert!(p.geometry_reply(old));
  assert_eq!(p.geometry_context(1,2),Ok(true));
  let next=p.front().unwrap();assert_eq!((next.start,next.end,next.revision),(2,8,2));assert_ne!(next.request,old.request);
  assert_eq!(p.geometry_read(2,8),Ok(None));assert!(!p.geometry_reply(old));
  let current=answer(&p);p.pop();assert!(p.geometry_reply(current));assert_eq!(p.geometry_read(2,8),Ok(Some(current)));
 }
 #[test] fn arbitrary_ranges_are_cached_without_edit_acknowledgment() {
  let mut p=ready();assert_eq!(p.geometry_read(2,8),Ok(None));let reply=answer(&p);p.pop();assert!(p.geometry_reply(reply));
  assert_eq!(p.geometry_read(2,8),Ok(Some(reply)));assert_eq!(p.state.edit,0);
  assert_eq!(p.geometry_read(9,10),Ok(None));assert_ne!(p.front().unwrap().request,reply.request);
 }
 #[test] fn stale_layout_edit_and_session_replies_are_rejected() {
  let mut p=ready();p.geometry_read(2,8).unwrap();let old=answer(&p);p.pop();p.geometry_context(1,2).unwrap();assert!(!p.geometry_reply(old));
  p.geometry_read(2,8).unwrap();let old=answer(&p);p.pop();p.direct_select(1,2,0);assert!(!p.geometry_reply(old));
  p.quiet(2,1,0,0,0,&[65;40]).unwrap();p.geometry_context(2,1).unwrap();assert!(!p.geometry_reply(old));
  assert_eq!(p.geometry_context(2,0),Err(()));
 }
 #[test] fn optimistic_mutations_revoke_geometry_before_edit_is_emitted() {
  let mut p=ready();p.geometry_read(2,8).unwrap();let old=answer(&p);p.pop();
  assert!(p.replace(0,1,&[66]));assert!(!p.geometry_reply(old));
  p.geometry_read(2,8).unwrap();let old=answer(&p);p.pop();
  assert!(p.select(1,2));assert!(!p.geometry_reply(old));
 }
 #[test] fn duplicate_requests_and_failed_layout_are_bounded() {
  let mut p=ready();for _ in 0..100 {assert_eq!(p.geometry_read(0,1),Ok(None));}assert_eq!(p.geometry.entries.len(),1);
  let mut reply=answer(&p);p.pop();reply.valid=0;assert!(p.geometry_reply(reply));
  assert_eq!(p.geometry_read(0,1),Ok(None));assert!(p.front().is_none());
  for i in 1..40 {p.geometry_read(i,i).unwrap();}assert_eq!(p.geometry.entries.len(),MAX_GEOMETRY);
  assert!(p.geometry_read(1,41).is_err());
 }
}

/// Platform-neutral composition presentation, with Win32 COLORREF values.
/// kind: raw, selected raw, converted, selected converted (1..4).
/// flags: text/background/underline color and underline style (1/2/4/8).
#[repr(C)]
#[derive(Clone,Copy,Debug,Default,PartialEq,Eq)]
pub struct ShellPageClause {
 pub start:u32,pub end:u32,pub kind:u32,pub flags:u32,
 pub line:u32,pub bold:u32,pub foreground:u32,pub background:u32,pub underline:u32,
}

#[cfg(test)] mod clause_tests {
 use super::*;
 #[test] fn clauses_coalesce_atomically_and_keep_styles() {
  let mut p=PageInput::default();p.quiet(1,1,0,0,0,&[]).unwrap();
  let clauses=[ShellPageClause{start:0,end:1,kind:3,..Default::default()},ShellPageClause{start:1,end:3,kind:4,flags:15,line:4,bold:1,foreground:0x123456,..Default::default()}];
  assert!(p.composition(0,0,3,&[65,66,67],&clauses));
  let before=p.state.edit;
  let mut invalid=clauses;invalid[1].start=0;
  assert!(!p.composition(0,0,3,&[65,66,67],&invalid));assert_eq!(p.state.edit,before);
  assert_eq!(p.front().unwrap().clauses,clauses);
  invalid=clauses;invalid[1].line=5;assert!(!p.composition(0,0,3,&[65,66,67],&invalid));
  assert!(!p.composition(0,0,3,&[65,66,67],&[clauses[0];65]));
  assert!(p.composition(0,0,1,&[65],&[clauses[0]]));assert_eq!(p.events.len(),1);
  assert_eq!(p.front().unwrap().clauses,vec![clauses[0]]);
  assert!(p.enqueue(3,0,0,1,&[65]));assert_eq!(p.events.len(),2);
  p.pop();assert!(p.front().unwrap().clauses.is_empty());
 }
}
