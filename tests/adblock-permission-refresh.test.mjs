import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

globalThis.JSWindowActorChild = class {};
const source = (await readFile(new URL("../upstream/firefox/browser/components/fennadblock/FennAdblockChild.sys.mjs", import.meta.url), "utf8")).replace(/^import\b[^;]*;$/gm, "");
const {FennAdblockChild} = await import("data:text/javascript;base64," + Buffer.from(source).toString("base64"));
function setup() {
  const sharedData = new Map([["FennAdblock:ResourceToken", "7:1"]]);
  globalThis.Services = {cpmm:{sharedData},prefs:{getBoolPref:()=>true}};
  const actor = new FennAdblockChild();
  actor.document = {}; actor.stopped = false;
  const pending = [], applied = [];
  actor.sendQuery = name => { assert.equal(name,"Refresh"); return new Promise(resolve=>pending.push(resolve)); };
  actor.configureCosmetics = value => applied.push(value);
  const set = token => sharedData.set("FennAdblock:ResourceToken", token);
  return {actor,pending,applied,set};
}
test("permission refresh applies only the current authorization",async()=>{
  const {actor,pending,applied}=setup(); const work=actor.refreshCosmetics("7");
  pending.shift()({token:"7:1",resources:{generation:7}}); await work;
  assert.deepEqual(applied,[{generation:7}]);
});
test("revocation during refresh discards old resources and retries once",async()=>{
  const {actor,pending,applied,set}=setup(); const work=actor.refreshCosmetics("7");
  set("7:2"); pending.shift()({token:"7:1",resources:{generation:7}});
  await Promise.resolve(); assert.deepEqual(applied,[]); assert.equal(pending.length,1);
  pending.shift()({token:"7:2",resources:null}); await work; assert.deepEqual(applied,[null]);
});
test("concurrent notifications coalesce into one authorization request",async()=>{
  const {actor,pending}=setup(); const work=actor.refreshCosmetics("7");
  await actor.refreshCosmetics("7"); assert.equal(pending.length,1);
  pending.shift()({token:"7:1",resources:null}); await work;
});
test("new engine generation prevents permission-only restart",async()=>{
  const {actor,pending,applied,set}=setup(); const work=actor.refreshCosmetics("7");
  set("8:2"); pending.shift()({token:"7:1",resources:{generation:7}}); await work;
  assert.deepEqual(applied,[]); assert.equal(pending.length,0);
});
test("pagehide and document replacement reject pending completion",async()=>{
  for (const invalidate of [a=>{a.lifecycle=1;},a=>{a.document={};},a=>{a.stopped=true;}]) {
    const {actor,pending,applied}=setup(); const work=actor.refreshCosmetics("7");
    invalidate(actor);pending.shift()({token:"7:1",resources:{generation:7}});await work;
    assert.deepEqual(applied,[]);
  }
});
test("mismatched response token cannot authorize a runner",async()=>{
  const {actor,pending,applied}=setup(); const work=actor.refreshCosmetics("7");
  pending.shift()({token:"7:0",resources:{generation:7}});await work;assert.deepEqual(applied,[]);
});
