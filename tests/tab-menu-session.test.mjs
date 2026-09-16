import assert from "node:assert/strict";
import { test } from "node:test";
import { WinUITabMenuSession } from "../upstream/firefox/browser/base/content/WinUITabMenuSession.sys.mjs";

test("selection publications and requests retain active identity and reject stale sets", () => {
  const targets = new Map([[1, {}], [2, {}], [3, {}]]);
  const calls = [];
  let selected = [1];
  const session = new WinUITabMenuSession({
    resolveTab: id => targets.get(id),
    describe: () => ({ items: [{ kind: "command", id: "r", enabled: true }] }),
    context: () => ({ selected: 1, selection: selected }),
    execute() {},
    select: (active, tabs) => calls.push([active, tabs]),
  });
  const snapshot = session.update(targets);
  assert.deepEqual(snapshot.state.tabs.map(t => t.selected), [true, false, false]);
  const event = { kind: "selection", revision: session.revision, tab: 1, tabs: [1, 2] };
  assert(session.receive(event));
  assert.deepEqual(calls, [[targets.get(1), [targets.get(1), targets.get(2)]]]);
  for (const invalid of [{ tabs: [] }, { tabs: [1, 1] }, { tabs: [1, 99] },
    { tab: 3 }, { revision: 0 }, { tabs: [1, "2"] }]) {
    assert.equal(session.receive({ ...event, ...invalid }), false);
  }
  selected = [1, 3];
  assert.equal(session.receive(event), false);
  session.update(targets);
  assert.equal(session.receive(event), false);
  session.receive({ kind: "open", session: 1, tab: 1, revision: session.revision });
  assert.equal(session.receive({ ...event, revision: session.revision }), false);
  assert.equal(calls.length, 1);
});

function fixture() {
  const target = {};
  const calls = [];
  const state = { target, enabled: true, selected: 1 };
  const session = new WinUITabMenuSession({
    resolveTab: id => id === 72 ? state.target : null,
    describe: () => ({ items: [
      { kind: "command", id: "opaque", label: "Reload Tab", enabled: state.enabled },
    ] }),
    context: () => state.selected,
    execute: (...args) => calls.push(args),
  });
  const publish = () => session.update(new Map(state.target ? [[72, state.target]] : []));
  const open = (id = 1, revision = session.revision) => session.receive({ kind: "open", tab: 72, session: id, revision });
  const activate = (overrides = {}) => session.receive({ kind: "activate", tab: 72,
    session: 1, revision: session.revision, id: "opaque", ...overrides });
  return { session, state, calls, target, publish, open, activate };
}

test("first opening and activation need no model response", () => {
  const f = fixture();
  const initial = f.publish();
  assert.equal(initial.version, 2);
  assert.equal(initial.publishOnly, true);
  assert.deepEqual(initial.state.tabs, [{ tab: 72, model: 1 }]);
  assert(f.open());
  assert.deepEqual(f.session.snapshot(), { version: 2 });
  assert(f.activate());
  assert.deepEqual(f.calls, [[f.target, "opaque"]]);
  assert.equal(f.activate(), false);
  assert(f.publish()); // Commands revoke input queued during their execution.
  assert.equal(f.publish(), null);
});

test("a cancelled or throwing command revokes openings queued in its modal loop", () => {
  for (const throws of [false, true]) {
    const f = fixture();
    f.publish(); f.open();
    const revision = f.session.revision;
    f.session.execute = () => { if (throws) throw new Error("cancelled"); };
    if (throws) assert.throws(() => f.activate(), /cancelled/);
    else assert(f.activate());
    assert.equal(f.publish().state.revision, revision + 1);
    assert.equal(f.open(2, revision), false);
    assert.equal(f.activate({ session: 2, revision }), false);
  }
});

test("unpublished and unknown targets never request lazy menu construction", () => {
  const f = fixture();
  assert.equal(f.open(), false);
  assert.deepEqual(f.session.snapshot(), { version: 2, revoke: 1 });
  assert.equal(f.activate(), false);
});

test("live eligibility, context and target identity are checked before open and execution", () => {
  for (const change of [f => f.state.enabled = false, f => f.state.selected = 2,
    f => f.state.target = null, f => f.state.target = {},
    f => f.session.describe = () => ({ items: [{ kind: "command", id: "other", enabled: true }] })]) {
    for (const afterOpen of [false, true]) {
      const f = fixture(); f.publish();
      if (!afterOpen) change(f);
      f.open();
      if (afterOpen) change(f);
      assert.equal(f.activate(), false);
      assert.equal(f.calls.length, 0);
      assert.equal(f.session.snapshot().revoke, 1);
      assert(f.publish()); // Validation must not consume the pending state update.
    }
  }
});

test("changed state replaces the snapshot and rejects old revisions", () => {
  const f = fixture(); const first = f.publish(); f.open();
  f.state.selected = 2;
  const second = f.publish();
  assert(second.state.revision > first.state.revision);
  assert.equal(f.activate({ revision: first.state.revision }), false);
  assert.equal(f.open(2, first.state.revision), false);
  assert(f.open(3));
  assert(f.activate({ session: 3 }));
});

test("wrong IDs, sessions, revisions and replayed input are rejected", () => {
  for (const override of [{ id: "unknown" }, { tab: 1 }, { session: 2 }, { revision: 0 }]) {
    const f = fixture(); f.publish(); f.open();
    assert.equal(f.activate(override), false);
  }
  const f = fixture(); f.publish(); f.open();
  assert(f.session.receive({ kind: "dismiss", session: 1, tab: 72 }));
  assert.equal(f.activate(), false);
  assert.equal(f.open(), false);
  assert(f.open(2));
  assert.equal(f.session.receive({ kind: "dismiss", session: 1, tab: 72 }), false);
  assert(f.activate({ session: 2 }));
});

test("nested eligibility and consumption before execution reentry", () => {
  const f = fixture();
  f.session.describe = () => ({ items: [{ kind: "submenu", enabled: false,
    items: [{ kind: "command", id: "opaque", enabled: true }] }] });
  f.publish(); f.open(); assert.equal(f.activate(), false);
  f.session.describe = () => ({ items: [{ kind: "submenu", enabled: true,
    items: [{ kind: "command", id: "opaque", enabled: true }] }] });
  f.publish(); f.open(2);
  f.session.execute = () => assert.equal(f.activate({ session: 2 }), false);
  assert(f.activate({ session: 2 }));
});

test("all live tabs share descriptions without a 32-tab opening cache", () => {
  const targets = new Map(Array.from({ length: 1000 }, (_, i) => [i + 1, {}]));
  const session = new WinUITabMenuSession({
    resolveTab: id => targets.get(id),
    describe: () => ({ items: [{ kind: "command", id: "r", enabled: true }] }),
    execute: () => {},
  });
  const initial = session.update(targets);
  assert.equal(initial.state.models.length, 1);
  assert.equal(initial.state.tabs.length, 1000);
  assert.equal(session.update(targets), null);
  assert(session.receive({ kind: "open", session: 1, tab: 1000, revision: 1 }));
  assert(session.receive({ kind: "activate", session: 1, tab: 1000, revision: 1, id: "r" }));
  targets.delete(1000);
  assert.equal(session.update(targets).state.tabs.length, 999);
  assert.equal(session.tabs.has(1000), false);
  assert.equal(session.receive({ kind: "open", session: 2, tab: 1000, revision: 2 }), false);
});

test("publications are window-local and invalid updates preserve the current state", () => {
  const f = fixture(); f.publish();
  const other = fixture();
  assert.equal(other.open(1, f.session.revision), false);
  assert.throws(() => f.session.update(new Map([[0, {}]])));
  assert.equal(f.session.revision, 1);
  assert(f.open()); assert(f.activate());
});
