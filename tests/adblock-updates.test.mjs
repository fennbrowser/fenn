import test from "node:test";
import assert from "node:assert/strict";
import {FennAdblockUpdateSchedule} from "../upstream/firefox/browser/components/fennadblock/FennAdblockUpdates.sys.mjs";
const MINUTE=60000, DAY=24*60*MINUTE;
function fixture(options={}) {
  const f={now:100*DAY, timers:new Map(), writes:[], checks:0, aborts:0, serial:0};
  f.schedule=new FennAdblockUpdateSchedule({now:()=>f.now,random:()=>.5,
    setTimer:(fn,delay)=>{const id=++f.serial;f.timers.set(id,{fn,delay});return id;},clearTimer:id=>f.timers.delete(id),
    read:()=>null,write:value=>f.writes.push(structuredClone(value)),eligible:()=>true,
    check:()=>{f.checks++;assert.equal(f.writes.at(-1).outcome,"checking");return {accepted:true};},
    abort:()=>f.aborts++,...options});
  f.delay=()=>[...f.timers.values()].at(-1)?.delay;
  return f;
}
test("first check waits two minutes and successful check persists daily cadence",async()=>{
  const f=fixture();await f.schedule.start();assert.equal(f.delay(),2*MINUTE);assert.equal(f.checks,0);
  await f.schedule.tick();assert.equal(f.checks,1);assert.equal(f.delay(),DAY);assert.equal(f.schedule.state.lastSuccess,f.now);
});
test("enabling in a running browser checks a due update after ten seconds",async()=>{
  for(const state of [null,{schema:1,next:1,failures:0,lastSuccess:1}]){
    const f=fixture({read:()=>state});await f.schedule.start({prompt:true});assert.equal(f.delay(),10000);assert.equal(f.checks,0);
  }
});
test("a prompt start keeps the daily cadence and failure backoff",async()=>{
  const recent=fixture({read:()=>({schema:1,next:100*DAY+20*60*MINUTE,failures:0,lastSuccess:100*DAY-4*60*MINUTE})});
  await recent.schedule.start({prompt:true});assert.equal(recent.delay(),20*60*MINUTE);
  const failing=fixture({read:()=>({schema:1,next:100*DAY+2*60*MINUTE,failures:2})});
  await failing.schedule.start({prompt:true});assert.equal(failing.delay(),2*60*MINUTE);
});
test("restart preserves next due time and does not duplicate timers",async()=>{
  const f=fixture({read:()=>({schema:1,next:100*DAY+8*60*MINUTE,failures:0})});await f.schedule.start();await f.schedule.start();
  assert.equal(f.delay(),8*60*MINUTE);assert.equal(f.timers.size,1);
});
test("overdue and invalid state still defer startup",async()=>{
  for(const state of [{schema:1,next:1,failures:0},{schema:1,next:999*DAY,failures:0},{schema:1,next:0,failures:-1},{schema:7,next:0,failures:0}]){
    const f=fixture({read:()=>state});await f.schedule.start();assert.equal(f.delay(),2*MINUTE);
  }
});
test("offline or metered eligibility performs no check or last-success write",async()=>{
  const f=fixture({eligible:()=>false});await f.schedule.start();await f.schedule.tick();assert.equal(f.checks,0);assert.equal(f.writes.length,0);assert.equal(f.delay(),60*MINUTE);
});
test("failures back off and success resets them",async()=>{
  let accepted=false;const f=fixture({random:()=>0,check:()=>({accepted})});await f.schedule.start();
  for(const delay of [30*MINUTE,2*60*MINUTE,8*60*MINUTE,DAY,DAY]){await f.schedule.tick();assert.equal(f.delay(),delay);}
  accepted=true;await f.schedule.tick();assert.equal(f.schedule.state.failures,0);assert.equal(f.delay(),22*60*MINUTE);
});
test("Retry-After respects a bounded server deferral",async()=>{
  const f=fixture({check:()=>{throw Object.assign(new Error(),{retryAfterMs:10*DAY});}});await f.schedule.start();await f.schedule.tick();assert.equal(f.delay(),DAY);
});
test("an overlapping check cannot start another transport",async()=>{
  let resolve;const pending=new Promise(r=>resolve=r);let count=0;
  const f=fixture({check:()=>{count++;return pending;}});await f.schedule.start();const first=f.schedule.tick();
  await new Promise(r=>setImmediate(r));assert.equal(await f.schedule.tick(),false);assert.equal(count,1);resolve({accepted:true});await first;
});
test("disable aborts pending work and cannot schedule or persist success afterward",async()=>{
  let resolve;const pending=new Promise(r=>resolve=r);const f=fixture({check:()=>pending});await f.schedule.start();const job=f.schedule.tick();
  await new Promise(r=>setImmediate(r));f.schedule.stop();resolve({accepted:true});await job;
  assert.equal(f.aborts,1);assert.equal(f.timers.size,0);assert.equal(f.writes.at(-1).outcome,"checking");
});
test("disable during state read never arms a timer",async()=>{
  let resolve;const pending=new Promise(r=>resolve=r);const f=fixture({read:()=>pending});const job=f.schedule.start();f.schedule.stop();resolve(null);await job;assert.equal(f.timers.size,0);
});
test("failure to persist attempt prevents network traffic",async()=>{
  const f=fixture({write:()=>{throw new Error("disk");}});await f.schedule.start();await assert.rejects(f.schedule.tick());assert.equal(f.checks,0);
});

test("manual checks preserve cooldown, failure backoff and restart deferral",async()=>{
 const f=fixture();await f.schedule.start();assert.equal(await f.schedule.checkNow(),true);
 assert.equal(await f.schedule.checkNow(),false);f.now+=MINUTE;
 assert.equal(await f.schedule.checkNow(),true);
 const g=fixture({check:()=>{throw Object.assign(new Error(),{retryAfterMs:DAY});}});
 await g.schedule.start();await g.schedule.checkNow();g.now+=MINUTE;
 assert.equal(await g.schedule.checkNow(),false);g.now+=DAY;
 assert.equal(g.schedule.canCheckNow(),true);
 const h=fixture({read:()=>({...g.schedule.state,outcome:"checking",next:100*DAY+30*MINUTE})});
 await h.schedule.start();assert.equal(h.schedule.canCheckNow(),false);
});
test("manual network deferral publishes availability without recording success",async()=>{
 let changes=0;const f=fixture({eligible:()=>false,changed:()=>changes++});await f.schedule.start();
 await f.schedule.checkNow();assert.equal(f.schedule.unavailable,true);assert.equal(f.schedule.running,false);
 assert.equal(f.schedule.state.lastSuccess,0);assert.equal(changes,3);
});
test("stored display dates are bounded and malformed outcomes discarded",async()=>{
 const f=fixture({read:()=>({schema:1,next:0,failures:0,lastCheck:-1,lastSuccess:101*DAY,outcome:"arbitrary"})});
 await f.schedule.start();assert.equal(f.schedule.state.lastCheck,0);assert.equal(f.schedule.state.lastSuccess,0);
 assert.equal(f.schedule.state.outcome,"never");
});
