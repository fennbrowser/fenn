import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

// Exercise the shipped browser adapter, not a copy of its drain algorithm.
const source = readFileSync(new URL(
  "../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");

function fixture(requests) {
  const completions = [], errors = [], posted = [];
  const dialog = { isOpen: false };
  const context = {
    window: { addEventListener() {}, windowUtils: {
      drainWinUIRequests: () => JSON.stringify({version: 1, generation: 7, requests}),
      completeWinUIRequests: text => completions.push(JSON.parse(text)),
    } },
    gDialogBox: dialog,
    gBrowser: {removeTab() { throw new Error("controlled Firefox command failure"); }},
    Services: { tm: { dispatchToMainThread: callback => posted.push(callback) } },
    console: { error: (...args) => errors.push(args) },
  };
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.element = {};
  pane.attached = true;
  pane.structuralDirty = false;
  let publications = 0;
  pane.publish = () => { publications++; };
  return {pane, completions, errors, posted, dialog, publications: () => publications};
}

for (const failedId of [1, 2, 3]) {
  test(`a failure at request ${failedId} answers every request without replay`, () => {
    const requests = [1, 2, 3].map(id => ({id, kind: 4, action: {kind: "new-tab"}}));
    const f = fixture(requests);
    const attempted = [];
    f.pane.executeRequest = request => {
      attempted.push(request.id);
      if (request.id === failedId) throw new Error("controlled command failure");
      return 1;
    };
    f.pane.drain();
    assert.deepEqual(attempted, [1, 2, 3]);
    assert.deepEqual(f.completions, [{version: 1, generation: 7, blocked: false,
      outcomes: requests.map(({id}) => ({id, outcome: id === failedId ? 2 : 1}))}]);
    assert.equal(f.pane.draining, false);
    assert.equal(f.publications(), 1);
    assert.equal(f.errors.length, 1);
    assert.equal(f.pane.structuralDirty, true);
  });
}

test("a throwing real close handler releases its guard and answers the next request", () => {
  const f = fixture([{id: 1, kind: 3, action: {kind: "close", tab: 9}},
                     {id: 2, kind: 4, action: {kind: "new-tab"}}]);
  f.pane.resolveStripTab = () => ({_pendingPermitUnload: false});
  let newTabs = 0;
  f.pane.executeNewTabRequest = () => { newTabs++; return 1; };
  f.pane.drain();
  assert.equal(f.pane.processingAction, false);
  assert.equal(newTabs, 1);
  assert.deepEqual(f.completions[0].outcomes, [{id: 1, outcome: 2}, {id: 2, outcome: 1}]);
});

test("a failed command that opens a dialog cannot authorize later commands", () => {
  const f = fixture([{id: 1, kind: 4, action: {}}, {id: 2, kind: 4, action: {}},
                     {id: 3, kind: 2, action: {kind: "open"}}]);
  const attempted = [];
  f.pane.executeRequest = request => {
    attempted.push(request.id);
    if (request.id === 1) {
      f.dialog.isOpen = true;
      throw new Error("failure after opening dialog");
    }
    return 1;
  };
  f.pane.drain();
  assert.deepEqual(attempted, [1, 3]);
  assert.deepEqual(f.completions[0].outcomes,
    [{id: 1, outcome: 2}, {id: 2, outcome: 2}, {id: 3, outcome: 1}]);
});

test("a wakeup during a throwing command is deferred until after completion", () => {
  const f = fixture([{id: 1, kind: 4, action: {}}]);
  f.pane.executeRequest = () => {
    f.pane.drain();
    throw new Error("failure after nested wakeup");
  };
  f.pane.drain();
  assert.equal(f.completions.length, 1);
  assert.deepEqual(f.completions[0].outcomes, [{id: 1, outcome: 2}]);
  assert.equal(f.posted.length, 1);
});

test("ordinary completed and rejected commands keep their outcomes without forcing a snapshot", () => {
  const f = fixture([{id: 1, kind: 4, action: {}}, {id: 2, kind: 4, action: {}}]);
  f.pane.executeRequest = request => request.id === 1 ? 1 : 2;
  f.pane.drain();
  assert.deepEqual(f.completions[0].outcomes, [{id: 1, outcome: 1}, {id: 2, outcome: 2}]);
  assert.equal(f.errors.length, 0);
  assert.equal(f.pane.structuralDirty, false);
});
