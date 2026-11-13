import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { test } from "node:test";

// Counts Glean metric calls by their dotted path, standing in for the
// class's private attempt/outcome counters.
function gleanRecorder(calls) {
  const metric = path =>
    new Proxy(function () {}, {
      get: (_target, key) => metric(`${path}.${String(key)}`),
      apply: () => {
        calls.set(path, (calls.get(path) ?? 0) + 1);
      },
    });
  return metric("Glean");
}

function sessionFile(fail) {
  const blockers = new Set();
  const calls = new Map();
  class SessionWriteError extends Error {}
  const logger = new Proxy({}, { get: () => () => {} });
  const lazy = {
    RunState: { isClosed: false, isClosing: false },
    SessionStore: { willAutoRestore: false },
    SessionWriteError,
    sessionStoreLogger: logger,
  };
  const source =
    readFileSync(
      new URL(
        "../upstream/firefox/browser/components/sessionstore/SessionFile.sys.mjs",
        import.meta.url
      ),
      "utf8"
    )
      .replace(/^import\b[^;]*;$/m, "")
      .replace("export class SessionFile", "class SessionFile")
      .replaceAll("SessionFile.#getWriter()", "__testWriter()") +
    "\n;this.SessionFile = SessionFile;";
  const scope = {
    XPCOMUtils: { declareLazy: () => lazy },
    PathUtils: { profileDir: "profile", join: (...parts) => parts.join("/") },
    IOUtils: {
      profileBeforeChange: {
        addBlocker: (_name, promise) => blockers.add(promise),
        removeBlocker: promise => blockers.delete(promise),
      },
    },
    Services: {
      prefs: { getCharPref: () => "", getIntPref: () => 0, setCharPref() {} },
      obs: { notifyObservers() {} },
      appinfo: { platformBuildID: "build" },
    },
    Glean: gleanRecorder(calls),
    __testWriter: async () => ({
      write: async () => {
        if (fail) throw new Error("Disk full");
        return { telemetry: {}, result: {} };
      },
    }),
  };
  vm.runInNewContext(source, scope);
  return { file: scope.SessionFile, blockers, calls };
}

const ATTEMPTS = "Glean.sessionRestore.writeAttempts.add";
const SUCCESS = "Glean.sessionRestore.writeOutcome.success.add";
const failures = calls =>
  [...calls].filter(([k]) => k.includes("writeOutcome") && k != SUCCESS)
    .reduce((n, [, v]) => n + v, 0);

test("strict session writes propagate disk failure after removing the shutdown blocker", async () => {
  const f = sessionFile(true);
  await assert.rejects(f.file.write({}, { rejectOnFailure: true }), /Disk full/);
  assert.equal(f.calls.get(ATTEMPTS), 1);
  assert.equal(f.calls.get(SUCCESS), undefined);
  assert.equal(failures(f.calls), 1);
  assert.equal(f.blockers.size, 0);
});

test("ordinary session writes retain their existing infallible shutdown behavior", async () => {
  const f = sessionFile(true);
  await f.file.write({});
  assert.equal(failures(f.calls), 1);
  assert.equal(f.blockers.size, 0);
});

test("strict successful session writes settle after persistence and blocker cleanup", async () => {
  const f = sessionFile(false);
  await f.file.write({}, { rejectOnFailure: true });
  assert.equal(f.calls.get(SUCCESS), 1);
  assert.equal(failures(f.calls), 0);
  assert.equal(f.blockers.size, 0);
});
