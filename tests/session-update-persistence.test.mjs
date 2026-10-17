import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { test } from "node:test";

function sessionFile(fail) {
  const blockers = new Set();
  const RunState = { isClosed: false, isClosing: false };
  const lazy = { RunState, sessionStoreLogger: { error() {} } };
  const source = readFileSync(new URL("../upstream/firefox/browser/components/sessionstore/SessionFile.sys.mjs", import.meta.url), "utf8")
    .replace(/^import .*;$/m, "").replace("export var SessionFile", "var SessionFile");
  const scope = {
    XPCOMUtils: { declareLazy: () => lazy },
    PathUtils: { profileDir: "profile", join: (...parts) => parts.join("/") },
    IOUtils: { profileBeforeChange: {
      addBlocker: (_name, promise) => blockers.add(promise),
      removeBlocker: promise => blockers.delete(promise),
    } },
    Services: { prefs: { getCharPref: () => "", getIntPref: () => 0 } },
    Glean: {},
  };
  vm.runInNewContext(source, scope);
  scope.SessionFileInternal.getWriter = async () => ({ write: async () => {
    if (fail) throw new Error("Disk full");
    return { telemetry: {}, result: {} };
  } });
  return { file: scope.SessionFile, blockers, internal: scope.SessionFileInternal };
}

test("strict session writes propagate disk failure after removing the shutdown blocker", async () => {
  const f = sessionFile(true);
  await assert.rejects(f.file.write({}, { rejectOnFailure: true }), /Disk full/);
  assert.equal(f.internal._failures, 1);
  assert.equal(f.blockers.size, 0);
});

test("ordinary session writes retain their existing infallible shutdown behavior", async () => {
  const f = sessionFile(true);
  await f.file.write({});
  assert.equal(f.internal._failures, 1);
  assert.equal(f.blockers.size, 0);
});

test("strict successful session writes settle after persistence and blocker cleanup", async () => {
  const f = sessionFile(false);
  await f.file.write({}, { rejectOnFailure: true });
  assert.equal(f.internal._successes, 1);
  assert.equal(f.blockers.size, 0);
});
