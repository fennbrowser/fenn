import assert from "node:assert/strict";
import { test } from "node:test";
import { WinUITabStripSession } from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

function fixture() {
  const target = {};
  const state = { rtl:false, groupsLabel:"Tab groups",
    rows:[{id:11, detail:"Work", flags:0, image:null}, {id:22, detail:"", flags:0, image:null}],
    groups:[{target, name:"Work", label:"Work â€” Expanded", collapsed:false}] };
  const calls = [], reorderCalls = [], detachCalls = [], adoptCalls = [];
  const session = new WinUITabStripSession({read: () => structured(state),
    collapse: (group, value) => calls.push([group, value]),
    reorder: (id, beforeId) => reorderCalls.push([id, beforeId]),
    detach: (id, point) => detachCalls.push([id, point]),
    adopt: (sourceHwnd, id, index) => adoptCalls.push([sourceHwnd, id, index])});
  function structured(value) {
    return {...value, rows:value.rows.map(r => ({...r})), groups:value.groups.map(g => ({...g}))};
  }
  const snapshot = session.update();
  const action = {kind:"strip-group", revision:snapshot.strip.revision,
    group:snapshot.strip.groups[0].id, collapsed:true};
  return {target, state, calls, reorderCalls, detachCalls, adoptCalls, session, snapshot, action};
}

test("group objects stay in Firefox; stable opaque IDs survive label/state changes", () => {
  const f = fixture();
  assert.equal("target" in f.snapshot.strip.groups[0], false);
  assert.equal(f.session.update(), null);
  f.state.groups[0].name = "Renamed";
  const next = f.session.update();
  assert.equal(next.strip.groups[0].id, f.action.group);
  assert.equal(next.strip.revision, 2);
});

test("current group activation is consumed once before executing Firefox", () => {
  const f = fixture();
  assert.equal(f.session.receive(f.action), true);
  assert.deepEqual(f.calls, [[f.target, true]]);
  assert.equal(f.session.receive(f.action), false);
  assert.equal(f.session.update().strip.revision, 2);
});

test("detached/replaced groups, changed state and hidden tab revisions revoke actions", () => {
  for (const change of [f => f.state.groups.pop(), f => f.state.groups[0].target = {},
    f => f.state.groups[0].collapsed = true, f => f.state.rows.pop(),
    f => f.state.rows[0].detail = "Different split"] ) {
    const f = fixture(); change(f);
    assert.equal(f.session.receive(f.action), false);
    assert.equal(f.calls.length, 0);
  }
});

test("unknown IDs, stale revisions, no-op and malformed actions are rejected", () => {
  for (const override of [{group:999}, {group:1.5}, {revision:0}, {collapsed:"true"},
    {collapsed:false}, {kind:"activate"}]) {
    const f = fixture();
    assert.equal(f.session.receive({...f.action, ...override}), false);
    assert.equal(f.calls.length, 0);
  }
});

test("oversized snapshots fail closed", () => {
  const f = fixture();
  f.state.rows = Array.from({length:10001}, (_, id) => ({id}));
  assert.throws(() => f.session.update(), /limit/);
});

test("a favicon image field round-trips and changing it alone bumps the revision", () => {
  const f = fixture();
  const image = {width:2, height:2, pixels:"0000ffff0000ffff0000ffff0000ffff"};
  f.state.rows[0].image = image;
  const next = f.session.update();
  assert.deepEqual(next.strip.rows[0].image, image);
  assert.equal(f.session.update(), null);
  f.state.rows[0].image = {...image, pixels:"ff000000".repeat(4)};
  assert.equal(f.session.update().strip.revision, 3);
});

test("a large favicon payload alone can trip the snapshot size cap", () => {
  const f = fixture();
  // A single row's hex-pixel string alone can blow past the session's
  // 900000-character budget, independent of row/group count -- this is the
  // failure mode gWinUIPane.describeStrip() must keep snapshots under by
  // dropping images rather than reaching this session-level throw.
  f.state.rows[0].image = {width:1, height:300000, pixels:"0000ffff".repeat(300000)};
  assert.throws(() => f.session.update(), /too large/);
});

test("a completed drag is consumed once and calls reorder with the real row ids", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.snapshot.strip.revision, id:22, beforeId:11};
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(f.reorderCalls, [[22, 11]]);
  assert.equal(f.session.receive(action), false);
  assert.equal(f.session.update().strip.revision, 2);
});

test("moving a row to the end is expressed as beforeId 0", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.snapshot.strip.revision, id:11, beforeId:0};
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(f.reorderCalls, [[11, 0]]);
});

test("a reorder that is already the current order is rejected as a no-op", () => {
  const f = fixture();
  // Row 11 is already immediately before row 22; row 22 is already last.
  for (const action of [{kind:"strip-reorder", revision:f.snapshot.strip.revision, id:11, beforeId:22},
    {kind:"strip-reorder", revision:f.snapshot.strip.revision, id:22, beforeId:0}]) {
    assert.equal(f.session.receive(action), false);
  }
  assert.equal(f.reorderCalls.length, 0);
});

test("reorder requests naming unknown rows, stale revisions or malformed ids are rejected", () => {
  const base = f => ({kind:"strip-reorder", revision:f.snapshot.strip.revision, id:11, beforeId:22});
  for (const override of [{id:999}, {beforeId:999}, {id:1.5}, {beforeId:-1},
    {revision:0}, {id:"11"}]) {
    const f = fixture();
    assert.equal(f.session.receive({...base(f), ...override}), false);
    assert.equal(f.reorderCalls.length, 0);
  }
});

test("a row set that changed since publication revokes a queued reorder", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.snapshot.strip.revision, id:22, beforeId:11};
  f.state.rows[0].detail = "Different split";
  assert.equal(f.session.receive(action), false);
  assert.equal(f.reorderCalls.length, 0);
});

test("a completed detach is consumed once and calls detach with the screen point", () => {
  const f = fixture();
  const action = {kind:"strip-detach", revision:f.snapshot.strip.revision, id:22, screenX:100, screenY:-40};
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(f.detachCalls, [[22, {screenX:100, screenY:-40}]]);
  assert.equal(f.session.receive(action), false);
  assert.equal(f.session.update().strip.revision, 2);
});

test("detach requests naming unknown rows, stale revisions or malformed fields are rejected", () => {
  const base = f => ({kind:"strip-detach", revision:f.snapshot.strip.revision, id:11, screenX:100, screenY:200});
  for (const override of [{id:999}, {id:1.5}, {id:"11"}, {revision:0},
    {screenX:"100"}, {screenY:NaN}, {screenX:undefined}]) {
    const f = fixture();
    assert.equal(f.session.receive({...base(f), ...override}), false);
    assert.equal(f.detachCalls.length, 0);
  }
});

test("a row set that changed since publication revokes a queued detach", () => {
  const f = fixture();
  const action = {kind:"strip-detach", revision:f.snapshot.strip.revision, id:22, screenX:100, screenY:200};
  f.state.rows[0].detail = "Different split";
  assert.equal(f.session.receive(action), false);
  assert.equal(f.detachCalls.length, 0);
});

test("a completed cross-window adopt is consumed once and names the source window and index", () => {
  const f = fixture();
  // id/index here name a row in the SOURCE window's own session, not this
  // one's -- unlike every other kind, there is no local row to look up.
  const action = {kind:"strip-adopt", revision:f.snapshot.strip.revision, sourceHwnd:0x1a2b, id:77, index:1};
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(f.adoptCalls, [[0x1a2b, 77, 1]]);
  // Nothing in this fixture's own state actually changed (adopt affects the
  // SOURCE window, not this session), so update() has nothing new to
  // report -- the revision bump itself, not a fresh snapshot, is what
  // rejects a repeat delivery of the same action below.
  assert.equal(f.session.revision, 2);
  assert.equal(f.session.receive(action), false);
});

test("adopt requests with stale revisions or malformed fields are rejected", () => {
  const base = f => ({kind:"strip-adopt", revision:f.snapshot.strip.revision, sourceHwnd:0x1a2b, id:77, index:0});
  for (const override of [{sourceHwnd:1.5}, {sourceHwnd:"0x1a2b"}, {id:1.5}, {index:-1},
    {index:1.5}, {revision:0}]) {
    const f = fixture();
    assert.equal(f.session.receive({...base(f), ...override}), false);
    assert.equal(f.adoptCalls.length, 0);
  }
});

test("window adoption revalidates the destination before forwarding the source revision", () => {
  const f = fixture();
  const calls = [];
  f.session.adopt = (...args) => { calls.push(args); return false; };
  const action = {kind:"strip-adopt", revision:f.snapshot.strip.revision,
    sourceHwnd:0x1a2b, id:77, index:1, sourceRevision:4, sourceSession:99};
  assert.equal(f.session.receive(action), false); // Firefox rejection is authoritative.
  assert.deepEqual(calls, [[0x1a2b, 77, 1, 4, 99, undefined]]);
  assert.equal(f.session.receive(action), false);
  assert.equal(calls.length, 1);
  for (const mutate of [x => x.state.rows.reverse(), x => x.state.rows.pop(),
    x => { x.state.windowDragPrivate = true; }, x => { x.session.consumed = true; }]) {
    const next = fixture();
    mutate(next);
    assert.equal(next.session.receive(action), false);
    assert.equal(next.adoptCalls.length, 0);
  }
  for (const override of [{sourceRevision:0}, {sourceRevision:1.5}, {sourceRevision:"4"}, {sourceSession:0}, {sourceSession:undefined}, {index:3}, {continueDrag:1}, {continueDrag:true, sourceRevision:undefined}]) {
    const next = fixture();
    assert.equal(next.session.receive({...action, ...override}), false);
    assert.equal(next.adoptCalls.length, 0);
  }
});

test("hover adoption forwards an explicit continuation only once", () => {
  const f = fixture();
  const calls = [];
  f.session.adopt = (...args) => { calls.push(args); return true; };
  const action = {kind:"strip-adopt", revision:f.snapshot.strip.revision,
    sourceHwnd:0x1a2b, id:77, index:1, sourceRevision:4, sourceSession:99, continueDrag:true};
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(calls, [[0x1a2b, 77, 1, 4, 99, true]]);
  assert.equal(f.session.receive(action), false);
  assert.equal(calls.length, 1);
});

test("only-tab eligibility is explicitly published and revoked by deltas", () => {
  const f = fixture();
  f.state.windowDragTabId = 11;
  f.state.windowDragPrivate = false;
  let delta = f.session.update().strip;
  assert.equal(delta.windowDragTabId, 11);
  assert.equal(delta.windowDragPrivate, false);
  f.state.windowDragTabId = 0;
  delta = f.session.update().strip;
  assert.equal(delta.windowDragTabId, 0);
  assert.equal("windowDragPrivate" in delta, false);
});

test("metadata deltas retain unrelated rows, images and groups", () => {
  const f = fixture();
  f.state.rows[0].image = {width:2, height:2, pixels:"0000ffff".repeat(4)};
  f.session.update();
  f.state.rows[0].flags = 2;
  const delta = f.session.update().strip;
  assert.equal(delta.version, 2);
  assert.equal(delta.base, 2);
  assert.deepEqual(delta.rows, [{id:11, flags:2}]);
  assert.deepEqual(delta.removed, []);
  assert.equal("groups" in delta, false);
  assert.equal("groupsLabel" in delta, false);
  assert.equal("rtl" in delta, false);
  f.state.rows[0].image = null;
  assert.deepEqual(f.session.update().strip.rows, [{id:11, image:null}]);
});

test("structural deltas remove and add identities; recovery is independently complete", () => {
  const f = fixture();
  f.state.rows.splice(0, 1, {id:33, detail:"New", flags:0, image:null});
  const delta = f.session.update().strip;
  assert.deepEqual(delta.removed, [11]);
  assert.deepEqual(delta.rows, [f.state.rows[0]]);
  const recovery = f.session.update(true).strip;
  assert.equal(recovery.base, 0);
  assert.equal(recovery.revision, 3);
  assert.deepEqual(recovery.rows, f.state.rows);
  assert.equal(recovery.groups.length, 1);
  assert.equal(f.session.update(), null);
});

test("consumed actions do not advance the acknowledged delta base", () => {
  const f = fixture();
  assert.equal(f.session.receive({kind:"strip-adopt", revision:1, sourceHwnd:42, id:77, index:0}), true);
  const delta = f.session.update().strip;
  assert.equal(delta.base, 1);
  assert.equal(delta.revision, 3);
  assert.deepEqual(delta.rows, []);
});

test("1,000-tab metadata update contains just one changed field and no pixel payload", () => {
  const image = {width:1, height:1, pixels:"0000ffff"};
  const state = {rtl:false, groupsLabel:"Groups", groups:[],
    rows:Array.from({length:1000}, (_, i) => ({id:i+1, detail:"", flags:0, image}))};
  const session = new WinUITabStripSession({read: () => ({...state, rows:state.rows.map(r => ({...r}))})});
  session.update();
  state.rows[500].flags = 4;
  const delta = session.update();
  assert.deepEqual(delta.strip.rows, [{id:501, flags:4}]);
  assert.ok(JSON.stringify(delta).length < 200);
  assert.equal(JSON.stringify(delta).includes("pixels"), false);
});


test("ordered group anchors retain opaque identity through active-member collapse", () => {
  const f = fixture();
  f.state.layout = [{target:f.target}, {target:f.target, tab:11}, {tab:22}];
  let update = f.session.update();
  const id = f.action.group;
  assert.deepEqual(update.strip.layout, [{group:id,tab:0},{group:id,tab:11},{group:0,tab:22}]);
  assert.equal(JSON.stringify(update).includes("target"), false);
  f.state.groups[0].collapsed = true;
  update = f.session.update();
  assert.equal(update.strip.layout, undefined, "retained active member does not rebuild order");
  f.state.rows.shift();
  f.state.layout.splice(1,1);
  update = f.session.update();
  assert.deepEqual(update.strip.layout, [{group:id,tab:0},{group:0,tab:22}]);
  assert.deepEqual(update.strip.removed, [11]);
  f.state.groups[0].name = "Renamed";
  assert.equal(f.session.update().strip.layout, undefined, "metadata preserves layout");
});

test("layout cannot serialize an unregistered group object", () => {
  const f = fixture();
  f.state.layout = [{target:{}}];
  assert.throws(() => f.session.update(), /Invalid native strip layout/);
});


test("group color changes publish metadata without replacing ordered membership", () => {
  const f = fixture();
  f.state.layout = [{target:f.target}, {target:f.target, tab:11}, {tab:22}];
  f.state.groups[0].color = 0xff336699;
  f.session.update();
  f.state.groups[0].color = 0xffdd3322;
  const update = f.session.update();
  assert.equal(update.strip.groups[0].color, 0xffdd3322);
  assert.equal(update.strip.groups[0].id, f.action.group);
  assert.equal(update.strip.layout, undefined);
  assert.deepEqual(update.strip.rows, []);
});
