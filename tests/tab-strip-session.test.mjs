import assert from "node:assert/strict";
import { test } from "node:test";
import { WinUITabStripSession } from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

function fixture() {
  const target = {};
  const state = { rtl:false, groupsLabel:"Tab groups", rows:[{id:11, detail:"Work", flags:0}],
    groups:[{target, name:"Work", label:"Work — Expanded", collapsed:false}] };
  const calls = [];
  const session = new WinUITabStripSession({read: () => structured(state),
    collapse: (group, value) => calls.push([group, value])});
  function structured(value) {
    return {...value, rows:value.rows.map(r => ({...r})), groups:value.groups.map(g => ({...g}))};
  }
  const snapshot = session.update();
  const action = {kind:"strip-group", revision:snapshot.strip.revision,
    group:snapshot.strip.groups[0].id, collapsed:true};
  return {target, state, calls, session, snapshot, action};
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
