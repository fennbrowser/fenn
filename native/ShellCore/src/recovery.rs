//! Survivor-owned restart budget. Time is monotonic milliseconds supplied by the host.
#[derive(Default)]
pub struct ShellRecovery {
    attempts: std::collections::VecDeque<u64>,
    last_time: u64,
    active: bool,
    exit_deadline: Option<u64>,
    stopped: bool,
    terminal: bool,
    manual: bool,
    episode: u64,
}

impl ShellRecovery {
    pub fn begin(&mut self, now: u64) -> bool {
        if self.stopped || self.terminal || self.active || now < self.last_time { return false; }
        self.last_time = now;
        while self.attempts.front().is_some_and(|t| now - t >= 60_000) {
            self.attempts.pop_front();
        }
        if self.attempts.len() >= 2 { return false; }
        self.exit_deadline = None;
        self.attempts.push_back(now);
        self.active = true;
        true
    }
    pub fn wait_for_exit(&mut self, now: u64) -> bool {
        if self.stopped || now < self.last_time { return false; }
        self.last_time = now;
        let deadline = *self.exit_deadline.get_or_insert(now.saturating_add(2000));
        now < deadline
    }
    pub fn peer_exited(&mut self, now: u64) -> bool {
        if self.stopped || now < self.last_time { return false; }
        self.last_time = now;
        self.active = false;
        self.exit_deadline = None;
        if self.manual { self.manual=false; self.enter_terminal(now); }
        true
    }
    pub fn exhausted(&self, now: u64) -> bool {
        !self.stopped && !self.active && now >= self.last_time &&
            self.attempts.iter().filter(|t| now - **t < 60_000).count() >= 2
    }
    pub fn finish(&mut self) { self.active = false; self.manual=false; self.exit_deadline = None; }
    pub fn stop(&mut self) { self.active = false; self.stopped = true; }
    pub fn enter_terminal(&mut self, now:u64)->bool {
        if self.stopped || now<self.last_time {return false;}
        self.last_time=now;self.active=false;self.manual=false;self.exit_deadline=None;
        if !self.terminal {
            let Some(next)=self.episode.checked_add(1) else {self.stop();return false;};
            self.episode=next;self.terminal=true;
        }
        true
    }
    pub fn retry_status(&self,now:u64)->ShellRecoveryRetry {
        if self.stopped || !self.terminal || self.active || now<self.last_time {return ShellRecoveryRetry::default();}
        let recent:Vec<_>=self.attempts.iter().filter(|t|now-**t<60_000).collect();
        let available_at=if recent.len()>=2 {recent[0].saturating_add(60_000)} else {now};
        ShellRecoveryRetry{episode:self.episode,available_at,eligible:u32::from(available_at<=now)}
    }
    pub fn retry(&mut self,episode:u64,now:u64)->bool {
        let status=self.retry_status(now);
        if episode==0 || status.episode!=episode || status.eligible==0 {return false;}
        self.terminal=false;
        if !self.begin(now){self.terminal=true;return false;}
        self.manual=true;true
    }
}

#[repr(C)]
#[derive(Default,Clone,Copy)]
pub struct ShellRecoveryRetry {pub episode:u64,pub available_at:u64,pub eligible:u32}

// Transportable budget charges only. Never transfer active attempts, terminal
// state, manual intent or readiness. All peers share the host monotonic clock.
#[repr(C)]
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShellRecoveryLedger { pub times: [u64; 2], pub count: u32, pub reserved: u32 }
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct ShellRecoveryHistory { pub shell: ShellRecoveryLedger, pub engine: ShellRecoveryLedger }

fn merge_ledger(a: ShellRecoveryLedger, b: ShellRecoveryLedger, now: u64) -> Option<ShellRecoveryLedger> {
    fn entries(v: ShellRecoveryLedger, now: u64) -> Option<Vec<u64>> {
        let n=v.count as usize;
        if n>2 || v.reserved!=0 || v.times[n..].iter().any(|t|*t!=0) {return None;}
        let times=&v.times[..n];
        if times.iter().any(|t|*t>now) || times.windows(2).any(|w|w[0]>w[1]) {return None;}
        Some(times.iter().copied().filter(|t|now-*t<60_000).collect())
    }
    let a=entries(a,now)?;let b=entries(b,now)?;
    let mut merged=a.clone();
    for (i,t) in b.iter().enumerate() {
        if b[..=i].iter().filter(|v|*v==t).count()>a.iter().filter(|v|*v==t).count(){merged.push(*t);}
    }
    if merged.len()>2{return None;}
    merged.sort_unstable();let mut result=ShellRecoveryLedger::default();
    result.count=merged.len() as u32;result.times[..merged.len()].copy_from_slice(&merged);Some(result)
}
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_history_merge(a: ShellRecoveryHistory,b: ShellRecoveryHistory,now:u64,out:*mut ShellRecoveryHistory)->bool {
    let Some(out)=out.as_mut() else{return false;};
    let Some(shell)=merge_ledger(a.shell,b.shell,now) else{return false;};
    let Some(engine)=merge_ledger(a.engine,b.engine,now) else{return false;};
    *out=ShellRecoveryHistory{shell,engine};true
}
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_export(value:*const ShellRecovery,now:u64,out:*mut ShellRecoveryLedger)->bool {
    let (Some(r),Some(out))=(value.as_ref(),out.as_mut()) else{return false;};
    if now<r.last_time{return false;}
    let mut ledger=ShellRecoveryLedger::default();
    for t in r.attempts.iter().filter(|t|now-**t<60_000) {ledger.times[ledger.count as usize]=*t;ledger.count+=1;}
    *out=ledger;true
}
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_import(value:*mut ShellRecovery,ledger:ShellRecoveryLedger,now:u64)->bool {
    let Some(r)=value.as_mut() else{return false;};
    let mut current=ShellRecoveryLedger::default();
    if !shell_recovery_export(r,now,&mut current){return false;}
    let Some(merged)=merge_ledger(current,ledger,now) else{return false;};
    r.attempts=merged.times[..merged.count as usize].iter().copied().collect();r.last_time=now;true
}

/// A read-only monotonic-time status query. Zero episode means no manual retry surface.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_retry_status(value:*const ShellRecovery,now:u64)->ShellRecoveryRetry {
    value.as_ref().map_or_else(ShellRecoveryRetry::default,|r|r.retry_status(now))
}
/// Atomically reserve one explicit attempt without resetting the rolling ledger.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_retry(value:*mut ShellRecovery,episode:u64,now:u64)->bool {
    value.as_mut().is_some_and(|r|r.retry(episode,now))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ledger_survives_alternating_supervisor_replacement() {unsafe {
        let mut shell=ShellRecovery::default();let mut engine=ShellRecovery::default();
        let mut history=ShellRecoveryHistory::default();
        for now in [100,200] {
            assert!(shell.begin(now));shell.finish();
            assert!(shell_recovery_export(&shell,now,&mut history.shell));
            let mut new_shell_supervisor=ShellRecovery::default();
            assert!(shell_recovery_import(&mut new_shell_supervisor,history.engine,now));engine=new_shell_supervisor;
            assert!(engine.begin(now+1));engine.finish();
            assert!(shell_recovery_export(&engine,now+1,&mut history.engine));
            let mut new_engine_supervisor=ShellRecovery::default();
            assert!(shell_recovery_import(&mut new_engine_supervisor,history.shell,now+1));shell=new_engine_supervisor;
        }
        assert!(!shell.begin(300));assert!(!engine.begin(300));
        assert!(shell.begin(60_100));assert!(!engine.begin(60_100));assert!(engine.begin(60_101));
    }}
    #[test]
    fn ledger_merge_is_bounded_idempotent_and_preserves_policy() {unsafe {
        let ledger=ShellRecoveryLedger{times:[100,100],count:2,reserved:0};
        assert_eq!(merge_ledger(ledger,ledger,100),Some(ledger));
        assert!(merge_ledger(ledger,ShellRecoveryLedger{times:[101,0],count:1,reserved:0},101).is_none());
        for bad in [ShellRecoveryLedger{count:3,..Default::default()},ShellRecoveryLedger{times:[101,0],count:1,reserved:0},ShellRecoveryLedger{times:[1,2],count:1,reserved:0},ShellRecoveryLedger{reserved:1,..Default::default()},ShellRecoveryLedger{times:[100,99],count:2,reserved:0}] {
            assert!(merge_ledger(bad,Default::default(),100).is_none());
        }
        let mut r=ShellRecovery::default();assert!(r.begin(100));assert!(r.enter_terminal(100));
        let episode=r.retry_status(100).episode;
        assert!(shell_recovery_import(&mut r,ledger,100));assert!(!r.retry(episode,100));
        assert!(r.retry(episode,60_100));
        assert!(shell_recovery_import(&mut r,Default::default(),60_100));assert!(!r.begin(60_100));
        r.stop();assert!(shell_recovery_import(&mut r,Default::default(),60_101));assert!(!r.begin(90_000));
        assert!(!shell_recovery_import(&mut r,Default::default(),60_100));
    }}
    #[test]
    fn manual_retry_preserves_budget_and_requires_current_episode() {
        let mut r=ShellRecovery::default();
        assert!(r.begin(100));r.finish();assert!(r.begin(200));
        assert!(r.enter_terminal(300));let episode=r.retry_status(300).episode;
        assert_eq!(r.retry_status(300).available_at,60_100);
        assert!(!r.retry(episode,60_099));assert!(!r.begin(60_100));
        assert!(!r.retry(episode+1,60_100));assert!(r.retry(episode,60_100));
        assert!(!r.retry(episode,60_100));
        assert!(r.peer_exited(60_101));assert!(!r.begin(60_102));
        let next=r.retry_status(60_102);assert_ne!(next.episode,episode);
        assert_eq!(next.available_at,60_200);assert!(!r.retry(episode,60_200));
        assert!(r.retry(next.episode,60_200));r.finish();
        assert_eq!(r.retry_status(60_201).episode,0);
    }
    #[test]
    fn terminal_clock_and_last_close_cannot_reopen_supervision() {
        let mut r=ShellRecovery::default();assert!(r.enter_terminal(100));
        let episode=r.retry_status(100).episode;
        assert_eq!(r.retry_status(99).episode,0);assert!(!r.retry(episode,99));
        assert!(r.enter_terminal(101));assert_eq!(r.retry_status(101).episode,episode);
        r.stop();assert!(!r.enter_terminal(102));assert!(!r.retry(episode,90_000));
        assert_eq!(r.retry_status(90_000).episode,0);
    }
    #[test]
    fn successful_bind_does_not_reset_rolling_budget() {
        let mut r = ShellRecovery::default();
        assert!(r.begin(100));
        assert!(!r.begin(200));
        r.finish();
        assert!(r.begin(300));
        r.finish();
        assert!(r.exhausted(60_099));
        assert!(!r.exhausted(60_100));
        assert!(!r.exhausted(299));
        assert!(!r.begin(60_099));
        assert!(r.begin(60_100));
        r.finish();
        assert!(!r.begin(60_100));
        assert!(r.begin(60_300));
    }
    #[test]
    fn confirmed_peer_exit_releases_active_attempt_without_refunding_budget() {
        let mut r = ShellRecovery::default();
        assert!(r.begin(100));
        assert!(!r.begin(150), "live in-flight attempts cannot overlap");
        assert!(!unsafe { shell_recovery_event(&mut r, 5, 99) });
        assert!(unsafe { shell_recovery_event(&mut r, 5, 200) });
        assert!(r.begin(201), "a dead unfinished replacement releases the attempt");
        assert!(unsafe { shell_recovery_event(&mut r, 5, 300) });
        assert!(!r.begin(301), "neither unfinished attempt is refunded");
        assert!(r.exhausted(301));
        r.stop();
        assert!(!unsafe { shell_recovery_event(&mut r, 5, 90_000) });
        assert!(!r.begin(90_001));
    }
    #[test]
    fn pipe_loss_wait_is_bounded_without_spending_a_restart() {
        let mut r = ShellRecovery::default();
        assert!(r.wait_for_exit(100));
        assert!(r.wait_for_exit(2099));
        assert!(!r.wait_for_exit(2100));
        assert!(r.begin(2100)); r.finish();
        assert!(r.begin(2200)); r.finish();
        assert!(!r.begin(2300));
    }
    #[test]
    fn stop_and_clock_regression_do_not_admit_attempts() {
        let mut r = ShellRecovery::default();
        assert!(r.begin(1000)); r.finish();
        assert!(!r.begin(999));
        assert!(r.begin(1001)); r.stop();
        assert!(!r.begin(90_000));
    }
}

#[no_mangle]
pub extern "C" fn shell_recovery_create() -> *mut ShellRecovery {
    Box::into_raw(Box::new(ShellRecovery::default()))
}
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_destroy(value: *mut ShellRecovery) {
    if !value.is_null() { drop(Box::from_raw(value)); }
}
/// Operations: 0 begin, 1 finish, 2 permanent stop, 3 await peer exit (at most 2s), 4 query exhausted idle budget, 5 confirmed peer exit, 6 terminal awaiting user retry.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_event(value: *mut ShellRecovery, operation: u32, now: u64) -> bool {
    let Some(r) = value.as_mut() else { return false; };
    match operation {
        0 => r.begin(now),
        1 => { r.finish(); true },
        2 => { r.stop(); true },
        3 => r.wait_for_exit(now),
        4 => r.exhausted(now),
        5 => r.peer_exited(now),
        6 => r.enter_terminal(now),
        _ => false,
    }
}

/// Per-window readiness barrier. Evidence is supplied only by each resource owner.
#[repr(C)]
#[derive(Default)]
pub struct ShellRecoveryReadiness {
    pub generation: u32,
    pub window: u32,
    pub required: u32,
    pub observed: u32,
    pub deadline: u64,
    pub last_time: u64,
    pub remaining: u64,
    pub paused: u32,
    /// 0 pending, 1 ready, 2 expired/invalid.
    pub phase: u32,
}

#[no_mangle]
pub extern "C" fn shell_recovery_readiness_begin(generation: u32, window: u32, required: u32, now: u64) -> ShellRecoveryReadiness {
    ShellRecoveryReadiness { generation, window, required, observed: 0,
        deadline: now.saturating_add(30_000), last_time: now, remaining: 30_000, paused: 0,
        phase: if generation == 0 || window == 0 || required == 0 { 2 } else { 0 } }
}

/// Returns phase, or 3 for a stale identity, clock regression or unknown evidence bit.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_readiness_observe(state: *mut ShellRecoveryReadiness,
    generation: u32, window: u32, evidence: u32, now: u64) -> u32 {
    let Some(s) = state.as_mut() else { return 3; };
    if s.generation != generation || s.window != window || now < s.last_time || evidence & !s.required != 0 { return 3; }
    if s.phase == 2 { return 2; }
    s.last_time = now;
    if s.paused == 0 && now >= s.deadline { s.phase = 2; return 2; }
    if s.phase == 1 { return 1; }
    s.observed |= evidence;
    if s.observed == s.required { s.phase = 1; }
    s.phase
}

#[cfg(test)]
mod readiness_tests {
    use super::*;
    #[test]
    fn all_owner_evidence_is_required_for_current_window() { unsafe {
        let mut s = shell_recovery_readiness_begin(4, 7, 15, 100);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 3, 7, 15, 101), 3);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 4, 8, 15, 101), 3);
        assert_eq!(s.observed, 0);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 4, 7, 3, 101), 0);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 4, 7, 4, 102), 0);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 4, 7, 8, 103), 1);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 4, 7, 8, 104), 1);
    } }
    #[test]
    fn a_ready_barrier_cannot_be_committed_after_its_deadline() { unsafe {
        let mut s = shell_recovery_readiness_begin(1, 2, 31, 100);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 31, 101), 1);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 0, 30_100), 2);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 0, 30_101), 2);
    } }
    #[test]
    fn timeout_never_becomes_ready_and_invalid_evidence_cannot_extend_it() { unsafe {
        let mut s = shell_recovery_readiness_begin(1, 1, 3, 100);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 1, 1, 200), 0);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 1, 2, 199), 3);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 1, 4, 201), 3);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 1, 2, 30_100), 2);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 1, 3, 30_101), 2);
    } }
}

/// Pause only after the supplied resource prerequisites; resuming preserves remaining active time.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_readiness_pause(state: *mut ShellRecoveryReadiness,
    generation: u32, window: u32, prerequisites: u32, now: u64) -> u32 {
    let result = shell_recovery_readiness_observe(state, generation, window, 0, now);
    if result != 0 { return result; }
    let s = &mut *state;
    if prerequisites & !s.required != 0 || s.observed & prerequisites != prerequisites { return 3; }
    if s.paused == 0 { s.remaining = s.deadline - now; s.paused = 1; }
    0
}

#[no_mangle]
pub unsafe extern "C" fn shell_recovery_readiness_resume(state: *mut ShellRecoveryReadiness,
    generation: u32, window: u32, now: u64) -> u32 {
    let result = shell_recovery_readiness_observe(state, generation, window, 0, now);
    if result != 0 { return result; }
    let s = &mut *state;
    if s.paused != 0 { s.deadline = now.saturating_add(s.remaining); s.paused = 0; }
    0
}

#[cfg(test)]
mod deferred_tests {
    use super::*;
    #[test]
    fn background_wait_preserves_active_time_and_identity() { unsafe {
        let mut s = shell_recovery_readiness_begin(1, 2, 31, 100);
        assert_eq!(shell_recovery_readiness_pause(&mut s, 1, 2, 7, 101), 3);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 7, 200), 0);
        assert_eq!(shell_recovery_readiness_pause(&mut s, 1, 2, 7, 1100), 0);
        assert_eq!(s.remaining, 29_000);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 0, 900_000), 0);
        assert_eq!(shell_recovery_readiness_resume(&mut s, 2, 2, 900_001), 3);
        assert_eq!(shell_recovery_readiness_resume(&mut s, 1, 2, 900_100), 0);
        assert_eq!(s.deadline, 929_100);
        assert_eq!(shell_recovery_readiness_resume(&mut s, 1, 2, 900_200), 0);
        assert_eq!(s.deadline, 929_100, "duplicate resume cannot extend the deadline");
        assert_eq!(shell_recovery_readiness_pause(&mut s, 1, 2, 7, 901_100), 0);
        assert_eq!(s.remaining, 28_000);
        assert_eq!(shell_recovery_readiness_resume(&mut s, 1, 2, 1_000_000), 0);
        assert_eq!(shell_recovery_readiness_observe(&mut s, 1, 2, 0, 1_028_000), 2);
        assert_eq!(shell_recovery_readiness_pause(&mut s, 1, 2, 7, 1_028_001), 2);
    } }
}
