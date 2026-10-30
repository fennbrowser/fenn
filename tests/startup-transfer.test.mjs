import assert from "node:assert/strict";
import { test } from "node:test";
import { WinUIStartupTransfer, WinUIStartupConnection, WinUIStartupRowsPublisher, connectFirefoxStartup } from "../upstream/firefox/browser/base/content/WinUIStartupTransfer.sys.mjs";

const identity = { context: "42", session: "9007199254740993", generation: "7" };
test("row publisher revises changed snapshots and reprojects unchanged identities after acknowledgment",()=>{
  const calls=[];let snapshot={ids:["90"],selected:"90"};
  const publisher=new WinUIStartupRowsPublisher(identity,(operation,fields)=>{
    calls.push({operation,...fields});return {...identity,rows:[[1,"1","0",true],[2,"0","90",false]]};
  },()=>snapshot);
  publisher.publish();publisher.publish();publisher.publish(true);
  snapshot={ids:["91","90"],selected:"91"};publisher.publish();
  assert.deepEqual(calls.map(c=>[c.operation,c.revision]),[[11,"1"],[11,"1"],[11,"2"]]);
  assert.deepEqual(publisher.rows[0],[1,"1","0",true]);
});

test("row publisher refuses invalid snapshots and wrong-endpoint projections without committing",()=>{
  let snapshot={ids:["90","90"],selected:"90"};let reply={...identity,session:"8",rows:[]};
  const publisher=new WinUIStartupRowsPublisher(identity,()=>reply,()=>snapshot);
  assert.throws(()=>publisher.publish());assert.equal(publisher.revision,0n);
  snapshot={ids:["90"],selected:"90"};assert.throws(()=>publisher.publish());
  reply={...identity,rows:[[1,"1","0",true],[1,"0","90",false]]};
  assert.throws(()=>publisher.publish());assert.equal(publisher.revision,0n);
  reply={...identity,rows:[[1,"1","0",true]]};publisher.publish();
  snapshot=null;assert.equal(publisher.publish().length,1);
});
const packet = (request, kind = 1, extra = {}) => ({ ...identity, request: String(request),
  draft: "1", kind, ...extra });

test("mixed editor selection preserves mappings and validates the destination", async()=>{
  const calls=[];let selected="90";
  const f=fixture({select:browser=>{calls.push(browser);selected=browser;return true;},selected:browser=>selected===browser});
  await f.consumer.execute(packet(1));
  assert.equal((await f.consumer.execute(packet(2,4,{browser:"90"}))).accepted,false);
  assert.equal((await f.consumer.execute(packet(3,4,{browser:"91"}))).accepted,true);
  assert.equal((await f.consumer.execute(packet(4,5,{browser:"90"}))).accepted,true);
  assert.equal(f.consumer.mappings.get("1"),"90");assert.deepEqual(calls,["91","90"]);
  f.backend.selected=()=>false;
  assert.equal((await f.consumer.execute(packet(5,4,{browser:"91"}))).uncertain,true);
});

test("a safely refused editor switch is acknowledged without losing the connection", async()=>{
  const f=fixture({select:()=>false,selected:()=>false});await f.consumer.execute(packet(1));
  let done=false;const calls=[];
  const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
    calls.push(message.operation);
    if(message.operation===8)return done?{...identity,kind:0}:packet(2,4,{browser:"91"});
    if(message.operation===6){assert.equal(message.accepted,false);done=true;}
  });
  await connection.wake();assert.equal(connection.stopped,false);
  await connection.wake();assert.deepEqual(calls,[8,6,8]);
});

test("connection completes one lease at a time despite repeated wakeups", async () => {
  const f=fixture(); const ack=[];
  const commands=[packet(1),packet(2,2,{browser:"90",text:"queued",modifiers:0})];
  const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
    if(message.operation===8)return commands[ack.length] || {...identity,kind:0};
    if(message.operation===6)ack.push(message.request);
  });
  const running=connection.wake();
  assert.equal(connection.wake(),running);
  await running;
  assert.deepEqual(ack,["1","2"]);
  assert.deepEqual(f.calls,["create",["submit","90","queued",0]]);
  assert.equal(connection.stopped,false);
});

test("uncertain execution and failed acknowledgment never cause automatic replay", async () => {
  for(const failAck of [false,true]) {
    const f=fixture(failAck?{}:{create:()=>{throw Error('after effect');}});
    const calls=[];
    const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
      calls.push(message.operation);
      if(message.operation===8)return packet(1);
      if(message.operation===6)throw Error('lost acknowledgment');
    });
    await connection.wake();await connection.wake();
    assert.equal(connection.stopped,true);
    assert.deepEqual(calls,failAck?[8,6,7]:[8,7]);
  }
});

test("safe rejection is acknowledged once then pauses automatic draining", async () => {
  const f=fixture({create:()=>null});const calls=[];
  const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
    calls.push(message);
    if(message.operation===8)return packet(1);
  });
  await connection.wake();await connection.wake();
  assert.deepEqual(calls.map(c=>c.operation),[8,6,7]);
  assert.equal(calls[1].accepted,false);
  assert.equal(connection.lastOutcome.terminal,true);
});

test("disconnect while a controller is pending never acknowledges completion", async () => {
  let finish;const f=fixture({create:()=>new Promise(resolve=>{finish=resolve;})});const calls=[];
  const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
    calls.push(message.operation);if(message.operation===8)return packet(1);
  });
  const running=connection.wake();await Promise.resolve();await Promise.resolve();
  connection.disconnect();finish("90");await running;
  assert.deepEqual(calls,[8,7]);
});
function fixture(overrides = {}) {
  const calls = [];
  const backend = { current: () => true,
    create: () => { calls.push("create"); return "90"; },
    submit: (...args) => { calls.push(["submit", ...args.slice(0, 3)]); return true; },
    close: () => { calls.push("close"); return true; }, ...overrides };
  return { calls, backend, consumer: new WinUIStartupTransfer(identity, backend) };
}
test("duplicate pending/completed creation executes once and conflicting replay fails", async () => {
  let finish;
  const f = fixture({ create: () => { f.calls.push("create"); return new Promise(resolve => { finish = resolve; }); } });
  const first = f.consumer.execute(packet(1));
  assert.equal(first, f.consumer.execute(packet(1)));
  assert.equal((await f.consumer.execute(packet(1, 1, { draft: "2" }))).reason, "conflicting-replay");
  assert.equal((await f.consumer.execute(packet(2))).reason, "busy");
  await Promise.resolve(); finish("90");
  assert.equal((await first).accepted, true);
  assert.deepEqual(await f.consumer.execute(packet(1)), await first);
  assert.deepEqual(f.calls, ["create"]);
});
test("raw submission is copied and kept behind the matching browser identity", async () => {
  const f = fixture(); await f.consumer.execute(packet(1));
  const input = packet(2, 2, { browser: "90", text: "first search", modifiers: 3 });
  const done = f.consumer.execute(input); input.text = "later edit";
  assert.equal((await done).accepted, true);
  assert.deepEqual(f.calls[1], ["submit", "90", "first search", 3]);
  assert.equal((await f.consumer.execute(packet(3, 2, { browser: "91", text: "wrong", modifiers: 0 }))).accepted, false);
  assert.equal(f.calls.length, 2);
  assert.equal((await f.consumer.execute(packet(1))).reason, "stale-request");
});
test("invalid identities and modifiers have no side effects", async () => {
  const f = fixture();
  for (const change of [{ session: Number(identity.session) }, { generation: "8" },
    { request: "18446744073709551616" }, { request: "01" }, { draft: "0" },
    { kind: 2, browser: "90", text: "x", modifiers: 8 }]) {
    assert.equal((await f.consumer.execute({ ...packet(1), ...change })).reason, "invalid-envelope");
  }
  assert.equal(f.calls.length, 0);
});
test("denied close retains mapping and successful retry releases it", async () => {
  const f = fixture({ close: () => false }); await f.consumer.execute(packet(1));
  assert.equal((await f.consumer.execute(packet(2, 3, { browser: "90" }))).accepted, false);
  assert.equal(f.consumer.mappings.get("1"), "90");
  f.backend.close = () => true;
  assert.equal((await f.consumer.execute(packet(3, 3, { browser: "90" }))).accepted, true);
  assert.equal(f.consumer.mappings.size, 0);
});
test("uncertain controller failure freezes the consumer rather than retrying", async () => {
  const f = fixture({ create: () => { f.calls.push("changed"); throw new Error("after side effect"); } });
  const result = await f.consumer.execute(packet(1));
  assert.equal(result.uncertain, true); assert.equal(f.consumer.closed, true);
  assert.equal((await f.consumer.execute(packet(1))).reason, "disconnected");
  assert.deepEqual(f.calls, ["changed"]);
});
test("disconnect during execution cannot report successful completion", async () => {
  let finish;
  const f = fixture({ create: () => new Promise(resolve => { finish = resolve; }) });
  const done = f.consumer.execute(packet(1)); await Promise.resolve();
  f.consumer.disconnect(); finish("90");
  assert.equal((await done).uncertain, true); assert.equal((await done).accepted, false);
});
test("mapping bound does not evict live browser tabs", async () => {
  let next = 90;
  const f = fixture({ create: () => String(next++) });
  for (let i = 1; i <= 32; i++) assert.equal((await f.consumer.execute(packet(i, 1, { draft: String(i) }))).accepted, true);
  assert.equal((await f.consumer.execute(packet(33, 1, { draft: "33" }))).accepted, false);
  assert.equal(next, 122); assert.equal(f.consumer.mappings.size, 32);
});
test("context teardown fails closed and protocol rejection is not completion", async () => {
  const f = fixture({ current: () => { throw new Error("window destroyed"); } });
  const result = await f.consumer.execute(packet(1));
  assert.equal(result.terminal, false); assert.equal(result.reason, "disconnected");
  assert.equal(f.consumer.closed, true); assert.equal(f.calls.length, 0);
});


test("presenter adoption accepts an untouched placeholder and keeps a deferred one connected", () => {
  const saved = { Services: globalThis.Services, ChromeUtils: globalThis.ChromeUtils, Ci: globalThis.Ci };
  try {
    globalThis.Services = { dirsvc: { get: () => ({ path: "fixture-profile" }) }, tm: { dispatchToMainThread() {} } };
    globalThis.ChromeUtils = { importESModule: () => ({ PrivateBrowsingUtils: { isWindowPrivate: () => false } }) };
    globalThis.Ci = { nsIFile: {} };
    for (const eligible of [true, false]) {
      const tab = {}; let adopted = false, synced = 0;
      const calls = [];
      const pane = { tabIds: new Map([[tab, 90]]), visibleTabs: () => [tab], syncTabs() {}, syncAddress() { ++synced; } };
      const window = { gWinUIPane: pane, gBrowser: { selectedTab: tab }, addEventListener() {}, windowUtils: {
        transferWinUIStartup(input) {
          const request = JSON.parse(input); calls.push(request);
          if (request.operation === 14) { assert.equal(request.browser, "90"); adopted = eligible; }
          return JSON.stringify({ ...identity, kind: 0, mappings: 0, lost: false, editorBrowser: adopted ? "90" : "0",
            rows: adopted ? [[2, "0", "90", true]] : [[1, "1", "0", true], [2, "0", "90", false]] });
        }
      } };
      const connection = connectFirefoxStartup(window, identity.context, identity.generation);
      connection.attachPresenter();
      assert.equal(connection.stopped, false);
      assert.equal(connection.editorReady, eligible);
      assert.equal(synced, eligible ? 1 : 0);
      assert.equal(calls.filter(call => call.operation === 14).length, 1);
      assert.equal(connection.rows.some(row => row[1] === "1"), !eligible);
      connection.disconnect();
    }
  } finally {
    for (const [key, value] of Object.entries(saved)) {
      if (value === undefined) delete globalThis[key]; else globalThis[key] = value;
    }
  }
});


test("draft import validates range, preserves empty text and never closes the mapped tab", async()=>{
  const calls=[];
  const f=fixture({adopt:(...args)=>{calls.push(args.slice(0,5));return true;}});
  await f.consumer.execute(packet(1));
  const command=packet(2,6,{browser:"90",text:"editing",selectionStart:2,selectionLength:3,modifiers:1});
  assert.equal((await f.consumer.execute({...command,selectionLength:99})).terminal,false);
  const result=await f.consumer.execute(command);
  assert.equal(result.accepted,true);
  assert.equal(await f.consumer.execute(command),result);
  assert.deepEqual(calls,[["90","editing",2,3,true]]);
  assert.equal(f.consumer.mappings.get("1"),"90");
  assert.equal((await f.consumer.execute(packet(3,6,{browser:"90",text:"",selectionStart:0,selectionLength:0,modifiers:0}))).accepted,true);
  assert.deepEqual(calls[1],["90","",0,0,false]);
  assert.deepEqual(f.calls,["create"]);
});


test("only acknowledged draft retirement removes the consumer mapping",async()=>{
  for(const retired of [false,true]) {
    const f=fixture({adopt:()=>true});await f.consumer.execute(packet(1));
    let done=false;
    const connection=new WinUIStartupConnection(identity,f.consumer,message=>{
      if(message.operation===8)return done?{...identity,kind:0}:packet(2,6,{browser:"90",text:"saved",selectionStart:1,selectionLength:2,modifiers:0,mappings:1});
      if(message.operation===6){done=true;return {mappings:retired?0:1};}
    });
    await connection.wake();
    assert.equal(f.consumer.mappings.has("1"),!retired);
    assert.equal(connection.stopped,false);
  }
});
