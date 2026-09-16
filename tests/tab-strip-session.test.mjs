import assert from "node:assert/strict";
import { test } from "node:test";
import { WinUITabStripSession } from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

function fixture() {
  const target = {};
  const state = { rtl:false, groupsLabel:"Tab groups",
    rows:[{id:11, detail:"Work", flags:0, image:null}, {id:22, detail:"", flags:0, image:null}],
    groups:[{target, name:"Work", label:"Work — Expanded", collapsed:false}] };
  const calls = [], reorderCalls = [];
  const session = new WinUITabStripSession({read: () => structured(state),
    collapse: (group, value) => calls.push([group, value]),
    reorder: (id, beforeId) => reorderCalls.push([id, beforeId])});
  function structured(value) {
    return {...value, rows:value.rows.map(r => ({...r})), groups:value.groups.map(g => ({...g}))};
  }
  const snapshot = session.update();
  const action = {kind:"strip-group", revision:snapshot.strip.revision,
    group:snapshot.strip.groups[0].id, collapsed:true};
  return {target, state, calls, reorderCalls, session, snapshot, action};
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
