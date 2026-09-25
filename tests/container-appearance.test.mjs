import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";
import {runInNewContext} from "node:vm";
import {WinUITabStripSession} from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

test("one container preference selects mutually exclusive visuals and publishes live deltas", () => {
  let dots;
  const key = "widget.windows.winui-prototype.tabstrip.containerDot";
  const context = {
    window:{addEventListener() {}, docShell:{usePrivateBrowsing:false}},
    document:{documentElement:{matches:() => false}},
    gBrowser:{tabs:[], selectedTabs:[], tabGroups:[]},
    Services:{prefs:{getBoolPref:(name, fallback) => name === key ? dots ?? fallback : fallback,
      getIntPref:(_name, fallback) => fallback}},
  };
  const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.localization = () => ({formatValueSync:id => id});
  pane.visibleTabs = () => [];
  let scheduled = 0;
  pane.schedule = () => scheduled++;
  const session = new WinUITabStripSession({read:() => pane.describeStrip()});
  session.update();
  assert.equal(session.state.containerDot, false);
  assert.equal(session.state.containerGlow, true);
  for (const value of [true, false]) {
    dots = value;
    pane.stripDirty = false;
    pane.observe(null, "nsPref:changed", key);
    assert.equal(pane.stripDirty, true);
    const delta = session.update().strip;
    assert.equal(delta.containerDot, value);
    assert.equal(delta.containerGlow, !value);
    assert.equal(delta.rows.length, 0);
  }
  assert.equal(scheduled, 2);
});
