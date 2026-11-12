import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {compileDomActions, FennAdblockDomRunner} from "../upstream/firefox/browser/components/fennadblock/FennAdblockDom.sys.mjs";
const policy = JSON.parse(readFileSync(new URL("../configs/adblock/capabilities.json", import.meta.url)));
const action = (type, arg, selector=".ad") => JSON.stringify({selector:[{type:"css-selector",arg:selector}], action:{type,arg}});
const css = {supports:s => !s.includes("has-text") && !s.includes("{")};
test("literal DOM actions are admitted; styles are delegated",()=>{
  const c=compileDomActions([action("remove"),action("remove-attr","title"),action("remove-class","ad"),action("style","color:red")],policy,{CSS:css});
  assert.equal(c.rules.length,3);assert.equal(c.counts.unsupported,0);
});
test("invalid selectors, argument forms and oversized input fail closed",()=>{
  for(const encoded of [action("remove-attr","x:y"),action("remove-attr","/regex/"),action("remove-class","a b"),action("remove-class",""),action("remove",null,".ad:has-text(ad)"),action("remove",null,"a".repeat(1025)),"{",null]){
    assert.equal(compileDomActions([encoded],policy,{CSS:css}).counts.rejected,1);
  }
  assert.equal(compileDomActions(Array(130).fill(action("remove")),policy,{CSS:css}).counts.truncated,2);
  assert.equal(compileDomActions([action("remove")],{dom_version:0},{CSS:css}).rules.length,0);
});
function fixture(count=4, overrides={}) {
  const timers=new Map();let serial=0, authorized=true, clock=0;const delays=new Map();
  const doc={documentElement:{},head:{},body:{}};
  const nodes=Array.from({length:count},()=>({isConnected:true,ownerDocument:doc,matches:()=>true,remove(){this.isConnected=false;}}));
  doc.createTreeWalker=()=>{
    let i=-1;
    const next=()=>{do{i++;}while(i<nodes.length&&!nodes[i].isConnected);return nodes[i]||null;};
    return {nextNode:next,nextSibling:next,parentNode:()=>null};
  };
  let disconnected=false;
  const win={document:doc,MutationObserver:class {observe(){} disconnect(){disconnected=true;}}};
  const runner=new FennAdblockDomRunner(win,{rules:[{type:"remove",selector:".ad"}],counts:{accepted:1,rejected:0,truncated:0}}, {...policy,...overrides},
    {setTimer:(f,delay)=>{timers.set(++serial,f);delays.set(serial,delay);return serial;},clearTimer:id=>timers.delete(id),current:()=>authorized,now:()=>clock});
  const tick=()=>{const [id,f]=timers.entries().next().value;timers.delete(id);clock+=delays.get(id);f();};
  return {runner,nodes,timers,tick,revoke:()=>{authorized=false;},disconnected:()=>disconnected,advance:ms=>{clock+=ms;},delay:()=>delays.get(timers.keys().next().value)};
}
test("work is deferred and sliced",()=>{
  const f=fixture(4,{dom_slice_checks:2});f.runner.start();assert.equal(f.runner.counts.mutations,0);
  f.tick();assert.equal(f.runner.counts.mutations,2);assert.equal(f.timers.size,1);
  f.tick();assert.equal(f.runner.counts.mutations,4);f.runner.stop();
});
test("mutation cap stops and releases all DOM references",()=>{
  const f=fixture(4,{dom_max_mutations:2});f.runner.start();f.tick();
  assert.equal(f.runner.counts.mutations,2);assert.equal(f.runner.counts.exhausted,true);
  assert.equal(f.timers.size,0);assert.ok(f.disconnected());
  for(const key of ["win","walker","node","current","now"])assert.equal(f.runner[key],null);
  assert.deepEqual(f.runner.rules,[]);
});
test("revocation before drain performs no action",()=>{
  const f=fixture();f.runner.start();f.revoke();f.tick();assert.equal(f.runner.counts.mutations,0);assert.ok(f.disconnected());
});
test("stop cancels queued work without restoring removed nodes",()=>{
  const f=fixture(4,{dom_slice_checks:1});f.runner.start();f.tick();f.runner.stop();
  assert.equal(f.timers.size,0);assert.equal(f.nodes[0].isConnected,false);assert.equal(f.nodes[1].isConnected,true);
});

test("accumulated scans survive lifetime totals while each scan stays bounded",()=>{
 const f=fixture(1,{dom_max_checks:5,dom_max_passes:3,dom_rate_window_ms:100});
 f.nodes[0].matches=()=>false;f.runner.start();
 for(let i=0;i<20;i++){f.tick();f.advance(100);f.runner.dirty=true;f.runner.schedule();}
 assert.ok(f.runner.counts.checks>5);assert.ok(f.runner.counts.passes>3);assert.equal(f.runner.counts.exhausted,false);f.runner.stop();
});
test("rate exhaustion coalesces mutations, resumes and preserves partial scans",()=>{
 const f=fixture(2,{dom_max_checks:3,dom_rate_window_ms:1000});f.nodes.forEach(n=>n.matches=()=>false);
 f.runner.start();f.tick();f.runner.dirty=true;f.runner.schedule();f.tick();
 assert.equal(f.runner.counts.throttles,1);assert.ok(f.runner.walker);assert.equal(f.runner.counts.checks,3);
 const delay=f.delay();f.runner.schedule();f.runner.dirty=true;assert.equal(f.timers.size,1);assert.equal(f.delay(),delay);
 f.tick();assert.ok(f.runner.counts.checks>3);assert.equal(f.runner.counts.exhausted,false);f.runner.stop();
});
test("revocation during cooldown cancels work and clears retained state",()=>{
 const f=fixture(1,{dom_max_passes:1,dom_rate_window_ms:1000});f.nodes[0].matches=()=>false;
 f.runner.start();f.tick();f.runner.dirty=true;f.runner.schedule();f.tick();assert.equal(f.runner.counts.throttles,1);
 f.revoke();f.tick();assert.equal(f.runner.counts.stopped,true);assert.equal(f.runner.targetWrites,null);assert.equal(f.runner.rate,null);assert.equal(f.timers.size,0);
});
test("same-target writeback loop stops before rate-window renewal",()=>{
 const f=fixture(1,{dom_max_target_writes:3});f.runner.start();
 for(let i=0;i<4;i++){f.tick();if(!f.runner.counts.stopped){f.nodes[0].isConnected=true;f.runner.dirty=true;f.runner.schedule();}}
 assert.equal(f.runner.counts.exhaustionReason,"target-write-loop");assert.equal(f.runner.counts.mutations,3);
});

test("an oversized single scan still stops at its hard check cap",()=>{
 const f=fixture(10,{dom_max_checks:3});f.nodes.forEach(n=>n.matches=()=>false);f.runner.start();f.tick();
 assert.equal(f.runner.counts.exhaustionReason,"scan-checks");assert.equal(f.runner.counts.lastScan.checks,3);assert.equal(f.runner.counts.throttles,0);
});
test("text-work cooldown retries the same rule without skipping its target",()=>{
 const f=fixture(2,{procedural_max_work:10,dom_rate_window_ms:1000});
 const original=f.nodes[0].ownerDocument.createTreeWalker;
 f.nodes[0].ownerDocument.createTreeWalker=(root,kind)=>{
  if(kind!==4)return original();let read=false;return {nextNode:()=>read?null:(read=true,{length:2,substringData:()=>"Ad"})};
 };
 f.nodes.forEach(n=>n.hasAttribute=()=>false);
 f.runner.rules=[{type:"remove-attr",arg:"none",selector:".ad",operators:[{type:"has-text",text:"Ad"}]}];
 f.runner.start();f.tick();assert.equal(f.runner.counts.work,6);
 f.runner.dirty=true;f.runner.schedule();f.tick();assert.equal(f.runner.counts.throttles,1);assert.equal(f.runner.ruleIndex,0);assert.equal(f.runner.node,f.nodes[1]);
 f.tick();assert.equal(f.runner.counts.work,13);assert.equal(f.runner.counts.exhausted,false);assert.equal(f.runner.walker,null);f.runner.stop();
});

test("recent target writes survive the rate-window boundary",()=>{
 const f=fixture(1,{dom_max_target_writes:3});f.nodes[0].matches=()=>false;f.runner.start();f.tick();
 f.advance(9400);f.nodes[0].matches=()=>true;
 for(let i=0;i<3;i++){f.nodes[0].isConnected=true;f.runner.dirty=true;f.runner.schedule();f.tick();}
 f.advance(600);f.nodes[0].isConnected=true;f.runner.dirty=true;f.runner.schedule();f.tick();
 assert.equal(f.runner.counts.exhaustionReason,"target-write-loop");assert.equal(f.runner.previousWrites,null);
});
test("write bookkeeping drops old generations after two windows",()=>{
 const f=fixture(1,{dom_rate_window_ms:1000});f.runner.start();f.tick();const old=f.runner.targetWrites;
 f.nodes[0].isConnected=true;f.nodes[0].matches=()=>false;
 for(let i=0;i<2;i++){f.advance(1000);f.runner.dirty=true;f.runner.schedule();f.tick();}
 assert.notEqual(f.runner.targetWrites,old);assert.notEqual(f.runner.previousWrites,old);assert.equal(f.runner.targetWrites.get(f.nodes[0]),undefined);assert.equal(f.runner.previousWrites.get(f.nodes[0]),undefined);f.runner.stop();
});

test("ordinary writes across windows are not a lifetime mutation fuse",()=>{
 const f=fixture(1,{dom_max_mutations:3,dom_rate_window_ms:1000});f.runner.start();
 for(let i=0;i<6;i++){f.tick();f.advance(1000);f.nodes[0].isConnected=true;f.runner.dirty=true;f.runner.schedule();}
 assert.equal(f.runner.counts.mutations,6);assert.equal(f.runner.counts.exhausted,false);f.runner.stop();
});
