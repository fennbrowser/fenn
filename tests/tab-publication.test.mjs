import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
function fixture() {
  const tabs = [1, 2].map(id => ({label: `tab ${id}`, isConnected: true}));
  const snapshots = [], deltas = [];
  const context = {window: {addEventListener() {}, windowUtils: {
    syncWinUITabs(...args) { snapshots.push(args); return args[4]; },
    applyWinUITabDelta(json) { const delta = JSON.parse(json); deltas.push(delta); return delta.revision; },
  }}, gBrowser: {selectedTab: tabs[0]}};
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  Object.assign(pane, {element: {}, attached: true, visibleTabs: () => tabs,
    publishTabContext() {}, publishStrip() {}, tabCommands: () => 3});
  return {pane, tabs, snapshots, deltas, utils: context.window.windowUtils};
}

test("Firefox owns one stamp for a multi-row delta and no-op snapshot", () => {
  const f = fixture(); f.pane.syncTabs();
  f.tabs.forEach(tab => f.pane.dirtyTitles.add(tab)); f.pane.dirtySelection = true;
  f.pane.publishTabDelta();
  assert.equal(f.deltas[0].titles.length, 2);
  assert.equal(f.deltas[0].base, 1); assert.equal(f.deltas[0].revision, 2);
  assert.equal(f.pane.nativeRevision, 2);
  f.pane.syncTabs(); assert.equal(f.snapshots[1][4], 3);
  f.pane.publishTabDelta(); assert.equal(f.pane.tabPublicationRevision, 3);
  f.pane.attached = false; f.pane.syncTabs(); assert.equal(f.pane.tabPublicationRevision, 3);
});

for (const acknowledgement of [0, 9000, "throws"]) {
  test(`refused delta (${acknowledgement}) gets one independently stamped snapshot`, () => {
    const f = fixture(); f.pane.syncTabs();
    f.utils.applyWinUITabDelta = () => {
      if (acknowledgement === "throws") throw new Error("refused");
      return acknowledgement;
    };
    f.pane.nativeRevision += 1000;
    f.pane.dirtyTitles.add(f.tabs[0]); f.pane.publishTabDelta();
    assert.equal(f.snapshots.length, 2); assert.equal(f.snapshots[1][4], 3);
    assert.equal(f.pane.nativeRevision, 3); assert.equal(f.pane.publicationRecoveries, 1);
  });
}

test("refused complete publication propagates without retry or accepting a foreign stamp", () => {
  const f = fixture(); f.pane.syncTabs();
  let calls = 0; f.utils.applyWinUITabDelta = () => 0;
  f.utils.syncWinUITabs = () => { calls++; return 1000; };
  f.pane.dirtyTitles.add(f.tabs[0]);
  assert.throws(() => f.pane.publishTabDelta(), /publication failed/);
  assert.equal(calls, 1); assert.equal(f.pane.nativeRevision, 1);
  assert.equal(f.pane.tabPublicationRevision, 3);
});

test("revision exhaustion never wraps or sends another publication", () => {
  const f = fixture(); f.pane.tabPublicationRevision = 0xfffffffe;
  f.pane.syncTabs(); assert.equal(f.pane.nativeRevision, 0xffffffff);
  assert.throws(() => f.pane.syncTabs(), /revision exhausted/);
  assert.equal(f.snapshots.length, 1);
});
