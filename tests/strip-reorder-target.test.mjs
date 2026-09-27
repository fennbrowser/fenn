import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
function fixture() {
  const calls = [];
  const tabs = [1, 2, 3].map(id => ({id, visible:true}));
  const group = {tabs:tabs.slice(1)};
  tabs[1].group = tabs[2].group = group;
  const context = {
    window:{addEventListener() {}},
    ChromeUtils:{importESModule:() => ({WinUITabStripSession:class {
      constructor(options) { Object.assign(this, options); }
      update() { return null; }
    }})},
    gBrowser:{tabs, moveTabBefore:(...args) => calls.push(args),
      TabMetrics:{METRIC_SOURCE:{DRAG_AND_DROP:1}, userTriggeredContext:() => "drag"}},
  };
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.resolveStripTab = id => tabs.find(tab => tab.id === id);
  pane.canDragGroupTab = () => true;
  pane.publishStrip();
  return {tabs, group, calls, reorder:pane.stripSession.reorder};
}

test("reorder before another group's first visible member targets the group", () => {
  for (const fromGroup of [false,true]) {
    const f = fixture();
    if (fromGroup) f.tabs[0].group = {tabs:[f.tabs[0]]};
    f.reorder(1,2);
    assert.equal(f.calls[0][1], f.group);
    assert.equal(f.calls[0][2].metricsContext, "drag");
  }
});

test("collapsed hidden members do not turn the leading visible boundary into a join", () => {
  const f = fixture();
  f.tabs[1].visible = false;
  f.reorder(1,3);
  assert.equal(f.calls[0][1], f.group);
});

test("internal and interior reorders retain the member target", () => {
  const f = fixture();
  f.reorder(3,2);
  f.reorder(1,3);
  assert.equal(f.calls[0][1], f.tabs[1]);
  assert.equal(f.calls[1][1], f.tabs[2]);
});

test("explicit ungroup still targets the whole neighboring group", () => {
  const f = fixture();
  f.reorder(3,2,true);
  assert.equal(f.calls[0][1], f.group);
  assert.equal(f.reorder(1,999), false);
  assert.equal(f.calls.length, 1);
});
