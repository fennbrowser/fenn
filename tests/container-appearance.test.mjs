import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";
import {runInNewContext} from "node:vm";
import {WinUITabStripSession} from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

test("glows are opt-in and both appearance preferences publish live deltas", () => {
  let dots, glows;
  const key = "widget.windows.winui-prototype.tabstrip.containerDot";
  const glowKey = "widget.windows.winui-prototype.tabstrip.glows";
  const context = {
    window:{addEventListener() {}, docShell:{usePrivateBrowsing:false}},
    document:{documentElement:{matches:() => false}},
    gBrowser:{tabs:[], selectedTabs:[], tabGroups:[]},
    Services:{prefs:{getBoolPref:(name, fallback) => name === key ? dots ?? fallback : name === glowKey ? glows ?? fallback : fallback,
      getIntPref:(_name, fallback) => fallback, getStringPref:(_name, fallback) => fallback}},
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
  assert.equal(session.state.containerGlow, false);
  assert.equal(session.state.groupGlow, false);
  for (const [enabled, value, changed] of [[true, false, glowKey], [true, true, key], [false, true, glowKey], [false, false, key], [true, false, glowKey]]) {
    dots = value;
    glows = enabled;
    pane.stripDirty = false;
    pane.observe(null, "nsPref:changed", changed);
    assert.equal(pane.stripDirty, true);
    const delta = session.update().strip;
    assert.equal(session.state.containerDot, value);
    assert.equal(session.state.containerGlow, enabled && !value);
    assert.equal(session.state.groupGlow, enabled);
    assert.equal(delta[changed === glowKey ? "groupGlow" : "containerDot"], changed === glowKey ? enabled : value);
    assert.equal(delta.rows.length, 0);
  }
  assert.equal(scheduled, 5);
});

test("strip backdrop preference is validated and publishes a live delta", () => {
  let backdrop;
  const key = "widget.windows.winui-prototype.tabstrip.backdrop";
  const context = {
    window:{addEventListener() {}, docShell:{usePrivateBrowsing:false}},
    document:{documentElement:{matches:() => false}},
    gBrowser:{tabs:[], selectedTabs:[], tabGroups:[]},
    Services:{prefs:{getBoolPref:(_name, fallback) => fallback, getIntPref:(_name, fallback) => fallback,
      getStringPref:(name, fallback) => name === key ? backdrop ?? fallback : fallback}},
  };
  const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.localization = () => ({formatValueSync:id => id});
  pane.visibleTabs = () => [];
  pane.schedule = () => {};
  const session = new WinUITabStripSession({read:() => pane.describeStrip()});
  assert.equal(session.update().strip.backdrop, "mica-alt");
  for (const [value, expected] of [["mica", "mica"], ["acrylic", "acrylic"], ["Mica", "mica-alt"], ["mica", "mica"], ["mica-alt", "mica-alt"]]) {
    const previous = session.state.backdrop;
    backdrop = value;
    pane.stripDirty = false;
    pane.observe(null, "nsPref:changed", key);
    assert.equal(pane.stripDirty, true);
    const delta = session.update();
    assert.equal(session.state.backdrop, expected);
    if (expected === previous) assert.equal(delta, null);
    else {
      assert.equal(delta.strip.backdrop, expected);
      assert.equal(delta.strip.rows.length, 0);
    }
  }
});
