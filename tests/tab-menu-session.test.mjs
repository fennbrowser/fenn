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

test("publication carries identities only, and an opening is answered with one description", () => {
  const f = fixture();
  const initial = f.publish();
  assert.equal(initial.version, 2);
  assert.equal(initial.publishOnly, true);
  assert.deepEqual(initial.state.tabs, [{ tab: 72 }]);
  assert.equal("models" in initial.state, false, "publication never describes commands");
  assert.equal(f.session.menuPublication(), null, "nothing is described until a menu opens");
  assert(f.open());
  const answer = f.session.menuPublication();
  assert.equal(answer.menu.tab, 72);
  assert.equal(answer.menu.session, 1);
  assert.equal(answer.menu.revision, f.session.revision);
  assert.equal(answer.menu.items[0].id, "opaque");
  assert.deepEqual(f.session.snapshot(), { version: 2 });
  assert(f.activate());
  assert.deepEqual(f.calls, [[f.target, "opaque", "tab"]]);
  assert.equal(f.activate(), false);
  assert(f.publish()); // Commands revoke input queued during their execution.
  assert.equal(f.publish(), null);
});

test("new-tab menu binds its surface at opening and revalidates it before execution", () => {
  const target = {}, calls = [];
  let name = "Work";
  const session = new WinUITabMenuSession({
    resolveTab: id => id === 1 ? target : null,
    describe: (_tab, surface) => ({items: [{kind: "command", enabled: true,
      id: surface === "new-tab" ? "container:2" : "close", label: name}]}),
    execute: (...args) => calls.push(args),
  });
  session.update(new Map([[1, target]]));
  const event = {tab: 1, revision: session.revision, session: 1, surface: "new-tab"};
  assert(session.receive({...event, kind: "open"}));
  assert.equal(session.menuPublication().menu.items[0].id, "container:2");
  assert(!session.receive({...event, kind: "activate", id: "close", surface: "tab"}));
  name = "Renamed";
  assert(!session.receive({...event, kind: "activate", id: "container:2"}));
  assert.equal(calls.length, 0);
  event.session++;
  assert(session.receive({...event, kind: "open"}));
  assert(session.receive({...event, kind: "activate", id: "container:2", surface: "tab"}));
  assert.deepEqual(calls, [[target, "container:2", "new-tab"]]);
  assert(!session.receive({...event, kind: "activate", id: "container:2"}));
  assert(!session.receive({...event, session: 3, kind: "open", surface: "unknown"}));
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

test("unpublished and unknown targets are revoked instead of being described", () => {
  const f = fixture();
  let describes = 0;
  f.session.describe = () => { describes++; return { items: [] }; };
  assert.equal(f.open(), false);
  assert.equal(describes, 0, "an unpublished target costs no description");
  assert.deepEqual(f.session.snapshot(), { version: 2, revoke: 1 });
  assert.equal(f.activate(), false);
});

// Structural prerequisites (the target and the published context) are checked
// when the menu opens, so an opening against stale structure is revoked before
// anything is described. A change to what the commands *say* is captured at
// opening and re-checked at activation instead, because that is the state the
// user is actually looking at.
test("structural changes revoke an opening; description changes fail activation", () => {
  for (const change of [f => f.state.selected = 2, f => f.state.target = null,
    f => f.state.target = {}]) {
    const f = fixture(); f.publish();
    change(f);
    assert.equal(f.open(), false, "structure moved: nothing is described");
    assert.equal(f.session.snapshot().revoke, 1);
    assert.equal(f.activate(), false);
    assert.equal(f.calls.length, 0);
  }
  for (const change of [f => f.state.enabled = false,
    f => f.session.describe = () => ({ items: [{ kind: "command", id: "other", enabled: true }] })]) {
    for (const afterOpen of [false, true]) {
      const f = fixture(); f.publish();
      if (!afterOpen) change(f);
      f.open();
      if (afterOpen) change(f);
      assert.equal(f.activate(), false);
      assert.equal(f.calls.length, 0);
      // A command whose eligibility changed after the menu opened revokes the
      // session; one that was already ineligible when it opened simply is not
      // in the description, so there is nothing to revoke. Either way the
      // command did not run, and publication state was not consumed.
      assert.equal(f.session.snapshot().revoke, afterOpen ? 1 : undefined);
      assert.equal(f.session.revision, 1);
    }
  }
});

test("an open menu is revalidated against live state and revoked when it no longer matches", () => {
  const f = fixture(); f.publish();
  assert(f.open());
  assert.equal(f.session.revalidateActive(), false, "unchanged: no revocation");
  assert.equal(f.session.snapshot().revoke, undefined);
  f.state.enabled = false;
  assert(f.session.revalidateActive(), "the displayed commands are no longer current");
  assert.equal(f.session.snapshot().revoke, 1);
  assert.equal(f.session.menuPublication(), null);
  assert.equal(f.activate(), false);
  // With nothing open, revalidation describes nothing at all.
  let describes = 0;
  f.session.describe = () => { describes++; return { items: [] }; };
  assert.equal(f.session.revalidateActive(), false);
  assert.equal(describes, 0);
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

// The measured regression this milestone removes: publication used to describe
// every tab's commands, every pass. It must now describe none, and an opening
// must cost exactly one description regardless of how many tabs exist.
test("publishing 1,000 tabs describes none of them; opening one describes one", () => {
  const targets = new Map(Array.from({ length: 1000 }, (_, i) => [i + 1, {}]));
  let describes = 0;
  const session = new WinUITabMenuSession({
    resolveTab: id => targets.get(id),
    describe: () => { describes++; return { items: [{ kind: "command", id: "r", enabled: true }] }; },
    execute: () => {},
  });
  const initial = session.update(targets);
  assert.equal(initial.state.tabs.length, 1000);
  assert.equal("models" in initial.state, false);
  assert.equal(describes, 0, "publication describes no tab");
  assert.equal(session.update(targets), null, "an unchanged window publishes nothing");
  assert.equal(describes, 0);

  assert(session.receive({ kind: "open", session: 1, tab: 1000, revision: 1 }));
  assert.equal(describes, 1, "one opening, one description");
  assert.equal(session.menuPublication().menu.tab, 1000);
  assert(session.receive({ kind: "activate", session: 1, tab: 1000, revision: 1, id: "r" }));
  assert.equal(describes, 2, "activation revalidates that one tab, and no other");

  // Reopening after the forced republication costs one more description, and
  // still none for the other 999 tabs.
  describes = 0;
  const republished = session.update(targets);
  assert.equal(describes, 0);
  assert(session.receive({ kind: "open", session: 2, tab: 500, revision: republished.state.revision }));
  assert.equal(describes, 1);

  targets.delete(1000);
  describes = 0;
  assert.equal(session.update(targets).state.tabs.length, 999);
  assert.equal(describes, 0, "a structural change still describes nothing");
  assert.equal(session.tabs.has(1000), false);
  assert.equal(session.receive({ kind: "open", session: 3, tab: 1000, revision: session.revision }), false);
  assert.equal(describes, 0, "a revoked opening describes nothing");
});

test("a title-only change publishes nothing, because titles are not published here", () => {
  const targets = new Map([[1, { label: "a" }], [2, { label: "b" }]]);
  const session = new WinUITabMenuSession({
    resolveTab: id => targets.get(id),
    describe: () => ({ items: [{ kind: "command", id: "r", enabled: true }] }),
    execute: () => {},
  });
  assert(session.update(targets));
  targets.get(1).label = "changed";
  assert.equal(session.update(targets), null,
    "a renamed tab does not invalidate identities, order or selection");
});

test("publications are window-local and invalid updates preserve the current state", () => {
  const f = fixture(); f.publish();
  const other = fixture();
  assert.equal(other.open(1, f.session.revision), false);
  assert.throws(() => f.session.update(new Map([[0, {}]])));
  assert.equal(f.session.revision, 1);
  assert(f.open()); assert(f.activate());
});
