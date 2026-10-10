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

test("indicator preference deltas preserve rows and carry only changed options", () => {
  const f = fixture();
  f.state.containerGlow = false;
  f.state.containerDot = true;
  const delta = f.session.update();
  assert.equal(delta.strip.containerGlow, false);
  assert.equal(delta.strip.containerDot, true);
  assert.deepEqual(delta.strip.rows, []);
});

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

test("a pinned row released over unpinned rows is unpinned even where its index is unchanged", () => {
  for (const beforeId of [22, 0]) {
    const f = fixture();
    f.state.rows[0].flags = 1;
    f.session.update();
    const calls = [];
    f.session.reorder = (...args) => calls.push(args);
    const action = {kind:"strip-reorder", revision:f.session.revision, id:11, beforeId, unpin:true};
    assert.equal(f.session.receive(action), true);
    assert.equal(f.session.receive(action), false);
    assert.deepEqual(calls, [[11, beforeId, false, true]]);
  }
});

test("unpin rejects unpinned sources, an all-pinned strip and malformed requests", () => {
  for (const change of [f => f.state.rows[0].flags = 0, f => f.state.rows[1].flags = 1,
    f => f.action.unpin = "true"]) {
    const f = fixture();
    f.state.rows[0].flags = 1;
    f.action = {kind:"strip-reorder", id:11, beforeId:22, unpin:true};
    change(f);
    f.session.update();
    f.action.revision = f.session.revision;
    f.session.reorder = () => assert.fail("must not mutate");
    assert.equal(f.session.receive(f.action), false);
  }
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

test("window privacy is published and omitted from deltas while unchanged", () => {
  const f = fixture();
  f.state.windowDragPrivate = true;
  const delta = f.session.update().strip;
  assert.equal(delta.windowDragPrivate, true);
  assert.equal("windowDragPrivate" in (f.session.update()?.strip ?? {}), false);
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

function groupingFixture() {
  const f = fixture();
  f.state.rows.forEach(r => Object.assign(r, {groupSource:true, groupTarget:true}));
  f.session.update();
  f.created = [];
  f.session.createGroup = (...args) => f.created.push(args);
  f.groupAction = {kind:"strip-create-group", session:f.session.windowDragSession,
    revision:f.session.revision, id:11, targetId:22, before:true};
  return f;
}

function joiningFixture() {
  const f = groupingFixture();
  f.state.layout = [{tab:11}, {target:f.target}, {target:f.target,tab:22}];
  f.session.update();
  f.joined = [];
  f.session.joinGroup = (...args) => f.joined.push(args);
  f.joinAction = {kind:"strip-join-group", session:f.session.windowDragSession,
    revision:f.session.revision, id:11, group:f.action.group, targetId:22, before:true};
  return f;
}

function previewFixture() {
  const f = joiningFixture();
  f.state.groups[0].collapsed = true;
  f.state.groups[0].tabs = [{id:33,title:"Hidden member",image:null}];
  f.selected = [];
  f.session.selectGroupTab = (...args) => f.selected.push(args);
  f.session.update();
  f.previewAction = {kind:"strip-group-select",session:f.session.windowDragSession,
    revision:f.session.revision,group:f.action.group,id:33};
  return f;
}

test("collapsed-group menu selects an invisible member once without admitting it as a strip row", () => {
  const f = previewFixture();
  assert.equal(f.session.receive({...f.previewAction,kind:"strip-reorder",beforeId:0}),false);
  assert.equal(f.session.receive(f.previewAction),true);
  assert.equal(f.session.receive(f.previewAction),false);
  assert.deepEqual(f.selected,[[f.target,33]]);
});

test("group previews reject removed, expanded, replaced and foreign members", () => {
  for (const change of [f => f.state.groups[0].tabs = [], f => f.state.groups[0].collapsed = false,
    f => f.state.groups[0].target = {}, f => f.previewAction.id = 11,
    f => f.previewAction.session++, f => f.previewAction.revision--,
    f => f.previewAction.group = 99, f => f.previewAction.id = 1.5]) {
    const f = previewFixture(); change(f);
    assert.equal(f.session.receive(f.previewAction),false);
    assert.deepEqual(f.selected,[]);
  }
});

test("preview title changes retain identity; browser refusal is consumed", () => {
  const f = previewFixture();
  f.state.groups[0].tabs[0].title = "Updated title";
  f.session.selectGroupTab = () => false;
  assert.equal(f.session.receive(f.previewAction),false);
  assert.equal(f.session.consumed,true);
});

test("ungroup drops may keep their tab index and are consumed once", () => {
  for (const beforeId of [0,11]) {
    const f = joiningFixture();
    const calls = [];
    f.session.reorder = (...args) => calls.push(args);
    const action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId, ungroup:true};
    assert.equal(f.session.receive(action),true);
    assert.equal(f.session.receive(action),false);
    assert.deepEqual(calls,[[22,beforeId,true,false]]);
  }
});

test("ungroup rejects stale membership, ineligible sources and malformed requests", () => {
  for (const change of [f => f.state.layout.pop(), f => f.state.rows[1].groupSource = false,
    f => f.state.groups[0].target = {}, f => f.action.ungroup = "true",
    f => f.action.id = 11, f => f.action.beforeId = 22, f => f.action.revision--]) {
    const f = joiningFixture();
    f.action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:0, ungroup:true};
    f.session.reorder = () => assert.fail("must not mutate");
    change(f);
    assert.equal(f.session.receive(f.action),false);
  }
});

test("existing-group drops join once at a member or a collapsed header", () => {
  for (const header of [false,true]) {
    const f = joiningFixture();
    if (header) {
      f.state.rows.pop();
      f.state.layout.pop();
      f.state.groups[0].collapsed = true;
      f.session.update();
      f.joinAction.revision = f.session.revision;
      f.joinAction.targetId = 0;
    }
    assert.equal(f.session.receive(f.joinAction),true);
    assert.equal(f.session.receive(f.joinAction),false);
    assert.deepEqual(f.joined,[[11,f.target,header ? 0 : 22,true]]);
  }
});

test("existing-group drops reject stale groups, membership, sessions and policy", () => {
  for (const change of [
    f => f.joinAction.session++, f => f.joinAction.revision--,
    f => f.joinAction.group = 999, f => f.joinAction.group = 1.5,
    f => f.joinAction.targetId = 11, f => f.joinAction.targetId = -1,
    f => f.joinAction.before = 1, f => f.state.rows[0].groupSource = false,
    f => f.state.rows[1].groupSource = false, f => f.state.groups.pop(),
    f => f.state.groups[0].target = {}, f => f.state.layout[2] = {tab:22},
    f => f.state.layout[0] = {tab:11,target:f.target},
  ]) {
    const f = joiningFixture(); change(f);
    assert.equal(f.session.receive(f.joinAction),false);
    assert.deepEqual(f.joined,[]);
  }
});

test("existing-group drops tolerate metadata changes but consume browser rejection", () => {
  const f = joiningFixture();
  f.state.rows[1].detail = "Updated title";
  f.joinAction.before = false;
  let calls = 0;
  f.session.joinGroup = (id, group, targetId, before) => {
    assert.deepEqual([id,group,targetId,before],[11,f.target,22,false]);
    calls++;
    return false;
  };
  assert.equal(f.session.receive(f.joinAction),false);
  assert.equal(f.session.receive(f.joinAction),false);
  assert.equal(calls,1);
});

test("group drop creates exactly once and preserves requested order", () => {
  const f = groupingFixture();
  assert.equal(f.session.receive(f.groupAction), true);
  assert.equal(f.session.receive(f.groupAction), false);
  assert.deepEqual(f.created, [[11,22,true]]);
});

test("group drop rejects stale identities, malformed actions and ineligible current tabs", () => {
  for (const change of [
    f => f.groupAction.session++, f => f.groupAction.revision--,
    f => f.groupAction.targetId = 99, f => f.groupAction.targetId = 11,
    f => f.groupAction.before = 1, f => f.groupAction.id = 1.5,
    f => f.state.rows.pop(), f => f.state.rows[0].groupSource = false,
    f => f.state.rows[1].groupTarget = false,
  ]) {
    const f = groupingFixture(); change(f);
    assert.equal(f.session.receive(f.groupAction), false);
    assert.deepEqual(f.created, []);
  }
});

test("group drop revalidates semantics without rejecting unrelated metadata", () => {
  const f = groupingFixture();
  f.state.rows[1].detail = "New title";
  assert.equal(f.session.receive(f.groupAction), true);
  assert.deepEqual(f.created, [[11,22,true]]);
});

test("group drop returns explicit browser rejection and cannot replay", () => {
  const f = groupingFixture();
  f.session.createGroup = () => false;
  assert.equal(f.session.receive(f.groupAction), false);
  assert.equal(f.session.receive(f.groupAction), false);
});


test("group context menu resolves live groups once, expanded or collapsed", () => {
  for (const collapsed of [false,true]) {
    const f = fixture(), calls = [];
    f.state.groups[0].collapsed = collapsed;
    f.session.update();
    f.session.openGroupMenu = (group, bounds) => calls.push({group,bounds});
    const event = {kind:"strip-group-context", revision:f.session.revision,
      session:f.session.windowDragSession, group:f.action.group, x:-120,y:32,width:80,height:32};
    assert.equal(f.session.receive(event), true);
    assert.deepEqual(calls, [{group:f.target,bounds:{x:-120,y:32,width:80,height:32}}]);
    assert.equal(f.session.receive(event), false);
    assert.equal(f.calls.length, 0);
  }
});

test("group context menu rejects stale identity and invalid screen bounds", () => {
  for (const override of [{session:0},{revision:0},{group:999},{group:1.5},
    {x:NaN},{y:Infinity},{width:0},{height:-1},{x:1000001}]) {
    const f = fixture();
    f.session.openGroupMenu = () => assert.fail("Invalid menu request");
    assert.equal(f.session.receive({kind:"strip-group-context", revision:f.session.revision,
      session:f.session.windowDragSession,group:f.action.group,x:1,y:1,width:32,height:32,...override}), false);
  }
  for (const replace of [false,true]) {
    const f = fixture();
    f.session.openGroupMenu = () => assert.fail("Removed group");
    if (replace) f.state.groups[0].target = {}; else f.state.groups = [];
    assert.equal(f.session.receive({kind:"strip-group-context", revision:f.session.revision,
      session:f.session.windowDragSession,group:f.action.group,x:1,y:1,width:32,height:32}),false);
  }
});


test("all-tabs button opens Firefox's panel at its bounds and rejects stale or invalid requests", () => {
  const f = fixture(), calls = [];
  f.session.openAllTabsMenu = bounds => calls.push(bounds);
  const event = {kind:"strip-all-tabs", revision:f.session.revision,
    session:f.session.windowDragSession, x:1200,y:0,width:46,height:32};
  assert.equal(f.session.receive(event), true);
  assert.equal(f.session.receive(event), true);
  assert.deepEqual(calls, Array(2).fill({x:1200,y:0,width:46,height:32}));
  for (const override of [{session:0},{revision:0},{x:NaN},{y:Infinity},{width:0},{height:-1},{x:1000001}])
    assert.equal(f.session.receive({...event, ...override}), false);
  assert.equal(calls.length, 2);
  f.session.openAllTabsMenu = null;
  assert.equal(f.session.receive(event), false);
});

test("hover-card requests ask Firefox for a preview without consuming the strip revision", () => {
  const f = fixture(), calls = [];
  f.session.preview = (id, request, scale) => calls.push([id, request, scale]);
  const event = {kind:"strip-preview", session:f.session.windowDragSession, id:11, request:3, scale:1.5};
  assert.equal(f.session.receive(event), true);
  // Hovering is not a gesture: it leaves the revision unconsumed and needs none.
  assert.ok(!f.session.consumed);
  assert.equal(f.session.receive({...event, request:4}), true);
  assert.equal(f.session.update(), null, "a preview changes nothing the strip publishes");
  assert.deepEqual(calls, [[11, 3, 1.5], [11, 4, 1.5]]);
  for (const override of [{session:0},{id:0},{id:1.5},{request:0},{request:"3"},{scale:NaN},{scale:0.1},{scale:9}])
    assert.equal(f.session.receive({...event, ...override}), false);
  f.session.preview = () => false;
  assert.equal(f.session.receive(event), false, "Firefox's refusal is reported");
  f.session.preview = null;
  assert.equal(f.session.receive(event), false);
  assert.equal(calls.length, 2);
});

test("localized labels publish once, then only when Firefox's strings change", () => {
  const f = fixture();
  f.state.labels = {tabs: "Tabs in this window", active: "Active tab", selected: "Selected tab",
    allTabs: {label: "List all tabs", tooltip: "List all tabs"}, newTab: {label: "New Tab", tooltip: "New Tab (Ctrl+T)"}};
  const full = f.session.update(true);
  assert.deepEqual(full.strip.labels, f.state.labels);
  assert.equal(f.session.update(), null);
  f.state.labels = {...f.state.labels, active: "Aktiver Tab", newTab: {label: "Neuer Tab", tooltip: "Neuer Tab (Strg+T)"}};
  const delta = f.session.update();
  assert.deepEqual(delta.strip.labels, f.state.labels);
  assert.deepEqual(delta.strip.rows, []);
  assert.equal(f.session.update(), null);
});

function groupDragFixture() {
  const f=fixture();
  f.state.groups[0].members=[11,33,44];
  f.state.groups[0].collapsed=true;
  f.state.layout=[{group:0,tab:22},{target:f.target,tab:0},{target:f.target,tab:11}];
  f.session.update();
  f.moves=[];
  f.session.moveGroup=(...args)=>f.moves.push(args);
  f.event={kind:"strip-group-reorder",session:f.session.windowDragSession,revision:f.session.revision,
    group:f.action.group,beforeGroup:0,beforeTab:22};
  return f;
}
test("whole group move preserves opaque group identity including hidden members",()=>{
  const f=groupDragFixture();
  assert.equal(f.session.receive(f.event),true);
  assert.deepEqual(f.moves,[[f.target,null,22]]);
  assert.equal(f.session.receive(f.event),false);
});
test("whole group drop rejects stale sessions, membership, targets, layout and no-ops",()=>{
  for (const change of [f=>f.event.session++,f=>f.event.revision--,f=>f.event.beforeTab=11,
    f=>f.event.beforeGroup=f.action.group,f=>f.event.beforeTab=0,f=>f.event.beforeTab=-1,
    f=>f.event.beforeTab=1.5,f=>f.state.groups[0].members=[11,33],
    f=>f.state.groups[0].target={},f=>f.state.rows[1].flags=1,
    f=>f.state.layout.reverse()]) {
    const f=groupDragFixture(); change(f);
    assert.equal(f.session.receive(f.event),false); assert.equal(f.moves.length,0);
  }
});
test("whole group destination names another group without merging",()=>{
  const f=groupDragFixture(), other={};
  f.state.groups.push({target:other,name:"Other",collapsed:true,members:[55]});
  f.state.layout.push({target:other,tab:0});
  f.session.update();
  const id=f.session.state.groups[1].id;
  // Adjacent before target is a no-op; move the final target group to the first.
  assert.equal(f.session.receive({...f.event,revision:f.session.revision,group:id,
    beforeGroup:f.action.group,beforeTab:0}),true);
  assert.deepEqual(f.moves,[[other,f.target,0]]);
});

function groupDetachFixture() {
  const f=groupDragFixture();
  f.detaches=[];
  f.session.detachGroup=(...args)=>f.detaches.push(args);
  f.detach={kind:"strip-group-detach",session:f.session.windowDragSession,revision:f.session.revision,
    group:f.action.group,screenX:900,screenY:-20};
  return f;
}
test("whole group detach hands Firefox the opaque group and physical release point once",()=>{
  const f=groupDetachFixture();
  assert.equal(f.session.receive(f.detach),true);
  assert.deepEqual(f.detaches,[[f.target,{screenX:900,screenY:-20}]]);
  assert.equal(f.session.receive(f.detach),false);
  assert.equal(f.moves.length,0);
});
test("whole group detach rejects stale sessions, membership, layout and malformed points",()=>{
  for (const change of [f=>f.detach.session++,f=>f.detach.revision--,f=>f.detach.group=0,
    f=>f.detach.group=1.5,f=>f.detach.group=99,f=>f.detach.screenX=NaN,f=>f.detach.screenY=Infinity,
    f=>f.detach.screenX=1000001,f=>f.state.groups[0].members=[11,33],
    f=>f.state.groups[0].target={},f=>f.state.layout.reverse(),f=>f.session.detachGroup=null]) {
    const f=groupDetachFixture(); change(f);
    assert.equal(f.session.receive(f.detach),false); assert.equal(f.detaches.length,0);
  }
});
test("whole group detach reports Firefox refusal",()=>{
  const f=groupDetachFixture();
  f.session.detachGroup=()=>false;
  assert.equal(f.session.receive(f.detach),false);
});

function groupAdoptFixture() {
  const f=groupDragFixture();
  f.adopts=[];
  f.session.adoptGroup=(...args)=>f.adopts.push(args);
  f.adopt={kind:"strip-group-adopt",session:f.session.windowDragSession,revision:f.session.revision,
    sourceHwnd:0x1234,sourceSession:77,group:5,beforeGroup:0,beforeTab:22};
  return f;
}
test("another window's group lands before a whole destination block once",()=>{
  const f=groupAdoptFixture();
  assert.equal(f.session.receive(f.adopt),true);
  assert.deepEqual(f.adopts,[[0x1234,77,5,null,22]]);
  assert.equal(f.session.receive(f.adopt),false);
  const g=groupAdoptFixture();
  assert.equal(g.session.receive({...g.adopt,beforeTab:0,beforeGroup:g.action.group}),true);
  assert.deepEqual(g.adopts,[[0x1234,77,5,g.target,0]]);
  const end=groupAdoptFixture();
  assert.equal(end.session.receive({...end.adopt,beforeTab:0}),true);
  assert.deepEqual(end.adopts,[[0x1234,77,5,null,0]]);
});
test("adopted group rejects stale destinations, members, malformed sources and refusal",()=>{
  for (const change of [f=>f.adopt.session++,f=>f.adopt.revision--,f=>f.adopt.sourceHwnd=0,
    f=>f.adopt.sourceHwnd=1.5,f=>f.adopt.sourceSession=0,f=>f.adopt.group=0,f=>f.adopt.beforeTab=-1,
    f=>f.adopt.beforeGroup=f.action.group,f=>f.adopt.beforeTab=11,f=>f.adopt.beforeTab=99,
    f=>f.state.rows[1].flags=1,f=>f.state.layout.reverse(),f=>f.session.adoptGroup=null]) {
    const f=groupAdoptFixture(); change(f);
    assert.equal(f.session.receive(f.adopt),false); assert.equal(f.adopts.length,0);
  }
  const f=groupAdoptFixture();
  f.session.adoptGroup=()=>false;
  assert.equal(f.session.receive(f.adopt),false);
});

// Presentation-only changes (a favicon, loading or audio status) between forming
// a gesture and Firefox draining it must not revoke the gesture; structural
// changes must. See the presentation protocol, sections 1 and 5.
const icon = {width:2, height:2, pixels:"0000ffff0000ffff0000ffff0000ffff"};
const loading = row => Object.assign(row, {flags:row.flags | 2, status:"Loading"});

test("a published favicon change does not revoke a queued reorder", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:11};
  f.state.rows[1].image = icon;
  assert.notEqual(f.session.update(), null);
  assert.equal(f.session.receive(action), true);
  assert.deepEqual(f.reorderCalls, [[22, 11]]);
});

test("an unpublished favicon change does not revoke a queued detach", () => {
  const f = fixture();
  const action = {kind:"strip-detach", revision:f.session.revision, id:22, screenX:100, screenY:-40};
  f.state.rows[0].image = icon;
  assert.equal(f.session.receive(action), true);
  assert.equal(f.detachCalls.length, 1);
});

test("a published loading change does not revoke a queued group creation", () => {
  const f = groupingFixture();
  loading(f.state.rows[1]);
  assert.notEqual(f.session.update(), null);
  assert.equal(f.session.receive(f.groupAction), true);
  assert.deepEqual(f.created, [[11,22,true]]);
});

test("a published favicon change does not revoke a queued group join", () => {
  const f = joiningFixture();
  f.state.rows[1].image = icon;
  assert.notEqual(f.session.update(), null);
  assert.equal(f.session.receive(f.joinAction), true);
  assert.equal(f.joined.length, 1);
});

test("published and unpublished loading changes do not revoke a group toggle", () => {
  for (const publish of [true, false]) {
    const f = fixture();
    loading(f.state.rows[0]);
    if (publish) assert.notEqual(f.session.update(), null);
    assert.equal(f.session.receive(f.action), true);
    assert.deepEqual(f.calls, [[f.target, true]]);
  }
});

test("several presentation publications keep an older action fresh", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:11};
  f.state.rows[0].image = icon; f.session.update();
  loading(f.state.rows[1]); f.session.update();
  f.state.rows[1].flags |= 8; f.session.update();
  assert.equal(f.session.revision, action.revision + 3);
  assert.equal(f.session.receive(action), true);
});

test("a published split change still revokes a queued reorder", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:11};
  f.state.rows[1].detail = "Split 1";
  assert.notEqual(f.session.update(), null);
  assert.equal(f.session.receive(action), false);
  assert.equal(f.reorderCalls.length, 0);
});

test("a pin change still revokes a queued reorder", () => {
  const f = fixture();
  const action = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:11};
  f.state.rows[0].flags |= 1;
  assert.equal(f.session.receive(action), false);
  f.session.update();
  assert.equal(f.session.receive(action), false);
});

test("a complete publication and a consumed action are structural boundaries", () => {
  const f = fixture();
  const before = f.session.revision;
  f.session.update(true);
  assert.equal(f.session.receive({...f.action, revision:before}), false);
  assert.equal(f.session.receive({...f.action, revision:f.session.revision}), true);
  // Firefox refuses or no-ops the toggle: the next publication may carry the
  // same structure, but anything formed before the consumption stays revoked.
  f.state.groups[0].collapsed = false;
  const consumedAt = f.session.revision;
  f.state.rows[0].image = icon;
  f.session.update();
  assert.equal(f.session.receive({...f.action, revision:consumedAt}), false);
  assert.equal(f.session.receive({...f.action, revision:f.session.revision}), true);
});

test("revisions beyond the latest publication are refused", () => {
  const f = fixture();
  assert.equal(f.session.receive({...f.action, revision:f.session.revision + 1}), false);
  assert.equal(f.calls.length, 0);
});

// Controls: the same sequence with a structural change must still refuse.
test("a published structural change still revokes queued gestures", () => {
  const f = groupingFixture();
  const reorder = {kind:"strip-reorder", revision:f.session.revision, id:22, beforeId:11};
  f.state.rows.push({id:33, detail:"", flags:0, image:null, groupSource:true, groupTarget:true});
  assert.notEqual(f.session.update(), null);
  assert.equal(f.session.receive(f.groupAction), false);
  assert.equal(f.session.receive(reorder), false);
  assert.deepEqual(f.created, []);
  assert.deepEqual(f.reorderCalls, []);
});

test("without an intervening change the same gestures are accepted", () => {
  const f = groupingFixture();
  assert.equal(f.session.receive(f.groupAction), true);
  const g = fixture();
  assert.equal(g.session.receive({kind:"strip-reorder", revision:g.session.revision, id:22, beforeId:11}), true);
});
