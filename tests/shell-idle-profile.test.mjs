import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../scripts/Profile-ShellIdle.js", import.meta.url), "utf8");
function fixture(diagnostic) {
  const calls = [];
  const inherited = { syncAddress(value) { calls.push([this, value]); return value; } };
  const pane = Object.assign(Object.create(inherited), {
    addressDirty: false,
    schedulePublication(value) { calls.push([this, value]); return value; },
    publishState() { throw new Error("publication failure"); },
  });
  class Session { snapshot() { return "snapshot"; } }
  const originalSchedule = pane.schedulePublication;
  const originalSnapshot = Session.prototype.snapshot;
  const context = { window: {}, gWinUIPane: pane, performance: { now: () => performance.now() },
    ChromeUtils: { importESModule: () => ({ WinUIAddressSession: Session }) } };
  runInNewContext(`(function() { ${source}\n}).call(null, ${diagnostic})`, context);
  return { pane, calls, probe: context.window.__idleProbe, inherited, Session, originalSchedule, originalSnapshot };
}

test("idle counters preserve values, receivers and exceptions and restore inherited methods", () => {
  const f = fixture(false);
  const value = {};
  assert.equal(f.pane.syncAddress(value), value);
  assert.deepEqual(f.calls, [[f.pane, value]]);
  assert.throws(() => f.pane.publishState(), /publication failure/);
  assert.equal(f.probe.counts["pane.publishState"].calls, 1);
  f.probe.restore();
  assert.equal(Object.hasOwn(f.pane, "syncAddress"), false);
  assert.equal(f.pane.syncAddress, f.inherited.syncAddress);
  assert.equal(f.pane.schedulePublication, f.originalSchedule);
  assert.equal(f.Session.prototype.snapshot, f.originalSnapshot);
});

test("the diagnostic suppresses only clean-address tasks and restores the original scheduler", () => {
  const f = fixture(true);
  assert.equal(f.pane.schedulePublication("background"), undefined);
  assert.equal(f.calls.length, 0);
  f.pane.addressDirty = true;
  assert.equal(f.pane.schedulePublication("selected"), "selected");
  assert.deepEqual(f.calls, [[f.pane, "selected"]]);
  f.probe.restore();
  f.pane.addressDirty = false;
  assert.equal(f.pane.schedulePublication("restored"), "restored");
  assert.equal(f.pane.schedulePublication, f.originalSchedule);
});
