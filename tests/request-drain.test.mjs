import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";
import { WinUITabStripSession } from "../upstream/firefox/browser/base/content/WinUITabStripSession.sys.mjs";

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
  return {context, pane, completions, errors, posted, dialog, publications: () => publications};
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

test("title and active-tab selection events do not rebuild strip metadata", () => {
  for (const [type, changed] of [["TabSelect"], ["TabAttrModified", ["label"]]]) {
    const f = fixture([]);
    f.pane.stripDirty = false;
    f.pane.schedule = () => {};
    f.pane.handleEvent({type, target:{}, detail:{changed}});
    assert.equal(f.pane.stripDirty, false, type);
    assert.ok(f.pane.dirtySelection || f.pane.contextDirty || f.pane.dirtyTitles.size);
  }
  for (const attribute of ["image", "busy", "muted", "soundplaying", "pinned"]) {
    const f = fixture([]);
    f.pane.stripDirty = false;
    f.pane.schedule = () => {};
    f.pane.handleEvent({type:"TabAttrModified", target:{}, detail:{changed:[attribute]}});
    assert.equal(f.pane.stripDirty, true, attribute);
  }
});

test("collapsed-member title changes refresh group previews without changing strip membership", () => {
  const f = fixture([]);
  f.pane.stripDirty = false;
  f.pane.schedule = () => {};
  const tab = {group:{collapsed:true}};
  f.pane.handleEvent({type:"TabAttrModified",target:tab,detail:{changed:["label"]}});
  assert.equal(f.pane.stripDirty,true);
  assert.equal(f.pane.structuralDirty,false);
  assert.equal(f.pane.dirtyTitles.has(tab),true);
});

test("multiselection republishes grouping eligibility as row deltas without rebuilding layout", () => {
  const tabs = [1, 2].map(() => ({isConnected:true, visible:true, hasAttribute:() => false}));
  const browser = {tabs, selectedTabs:[tabs[0]], tabGroups:[], getIcon:() => ""};
  const context = {
    window:{addEventListener() {}},
    PrivateBrowsingUtils:{isWindowPrivate:() => false},
    document:{documentElement:{matches:() => false}},
    gBrowser:browser,
    Services:{prefs:{getBoolPref:() => true, getIntPref:() => 240, getStringPref:(_name, fallback) => fallback}},
    ContextualIdentityService:{getPublicIdentityFromId:() => null},
  };
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.localization = () => ({formatValueSync:id => id,
    formatMessagesSync: ([{id}]) => [{attributes: [{name: "label", value: id}, {name: "tooltiptext", value: id}]}]});
  pane.visibleTabs = () => tabs;
  tabs.forEach((tab, index) => pane.tabIds.set(tab, index + 1));
  pane.schedule = () => {};
  const session = new WinUITabStripSession({read:() => pane.describeStrip()});
  let previous = session.update().strip.revision;
  assert.ok(session.state.rows.every(row => row.groupSource && row.groupTarget));

  for (const selectedTabs of [tabs, [tabs[0]]]) {
    browser.selectedTabs = selectedTabs;
    pane.stripDirty = pane.structuralDirty = pane.contextDirty = false;
    pane.handleEvent({type:"TabMultiSelect", target:tabs[0]});
    assert.equal(pane.stripDirty, true, "eligibility changes must reach the native strip");
    assert.equal(pane.contextDirty, true, "menu multiselection state also changes");
    assert.equal(pane.structuralDirty, false, "membership and order remain intact");
    const delta = session.update().strip;
    const eligible = selectedTabs.length === 1;
    assert.equal(delta.base, previous);
    assert.deepEqual(delta.rows, [1, 2].map(id => ({id, groupSource:eligible, groupTarget:eligible})));
    assert.deepEqual(delta.removed, []);
    for (const key of ["layout", "groups", "groupingDelay"]) assert.equal(key in delta, false, key);
    previous = delta.revision;
    assert.equal(session.update(), null, "unchanged eligibility produces no publication");
  }
});

test("a rejected strip delta recovers with one complete publication", () => {
  const context = {window:{addEventListener() {}, windowUtils:{}}};
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  const sent = [], updates = [];
  pane.stripSession = {update(full = false) {
    updates.push(full);
    return {version:2, publishOnly:true, strip:{version:2, base:full ? 0 : 4, revision:full ? 6 : 5}};
  }};
  context.window.windowUtils.syncWinUITabMenu = text => {
    const message = JSON.parse(text);
    sent.push(message);
    if (message.strip.base) throw new Error("missing base");
  };
  pane.publishStrip();
  assert.deepEqual(updates, [false, true]);
  assert.equal(sent.length, 2);
  assert.equal(sent[1].strip.base, 0);
});

test("window adoption validates the source tab and privacy", () => {
  function adoptionFixture() {
    let callbacks;
    const tab = {closing:false, _pendingPermitUnload:false};
    const sourceWindow = {private:false, docShell:{
      treeOwner:{QueryInterface:() => ({nativeHandle:"0x1234"})}},
      gBrowser:{tabs:[tab]},
      gWinUIPane:{resolveStripTab: id => id === 7 ? tab : null,
        stripSession:{revision:4, windowDragSession:12}}};
    const adopted = [];
    const context = {window:{addEventListener() {}},
      PrivateBrowsingUtils:{isWindowPrivate:win => win === sourceWindow && sourceWindow.private},
      Ci:{nsIBaseWindow:{}}, Services:{wm:{getEnumerator:() => [sourceWindow]}},
      gBrowser:{adoptTab:(...args) => { adopted.push(args); return {}; }},
      ChromeUtils:{importESModule:() => ({WinUITabStripSession:class {
        constructor(value) { callbacks = value; }
        update() { return null; }
      }})}};
    runInNewContext(source, context);
    context.gWinUIPane.publishStrip();
    return {sourceWindow, tab, adopted, context, adopt:() => callbacks.adopt(0x1234, 7, 0)};
  }
  const accepted = adoptionFixture();
  assert.equal(accepted.adopt(), true);
  assert.equal(accepted.adopted[0][0], accepted.tab);
  for (const change of [
    f => { f.sourceWindow.private = true; },
    f => { f.sourceWindow.gWinUIPane.resolveStripTab = () => null; },
  ]) {
    const f = adoptionFixture();
    change(f);
    assert.equal(f.adopt(), false);
    assert.equal(f.adopted.length, 0);
  }
  const rejected = adoptionFixture();
  rejected.context.gBrowser.adoptTab = () => null;
  assert.equal(rejected.adopt(), false, "adoptTab rejection reaches the explicit outcome");
});


test("whole-group adapter delegates the group object and rejects split-internal/pinned/empty targets",()=>{
  let commands;
  const moves=[], group={isConnected:true,tabs:[{}]}, first={isConnected:true}, second={isConnected:true};
  first.splitview=second.splitview={tabs:[first,second]};
  const context={window:{addEventListener(){}},gDialogBox:{isOpen:false},
    ChromeUtils:{importESModule:()=>({WinUITabStripSession:class {
      constructor(options){commands=options;} update(){return null;}
    }})},
    gBrowser:{tabGroups:[group],TabMetrics:{METRIC_SOURCE:{DRAG_AND_DROP:1},userTriggeredContext:()=>({})},
      moveTabBefore:(...args)=>moves.push(args),moveTabAfter:(...args)=>moves.push(args)}};
  runInNewContext(source,context);
  context.gWinUIPane.resolveStripTab=id=>id===1?first:second;
  context.gWinUIPane.publishStrip();
  assert.equal(commands.moveGroup(group,null,2),false);
  assert.equal(commands.moveGroup(group,null,1),true); assert.equal(moves[0][0],group); assert.equal(moves[0][1],first);
  first.pinned=true; assert.equal(commands.moveGroup(group,null,1),false);
  group.tabs=[]; assert.equal(commands.moveGroup(group,null,0),false);
  assert.equal(moves.length,1);
});

test("a hover-card request is answered with the registrable domain, then one bounded thumbnail", async () => {
  const published = [], captures = [];
  const prefs = { "browser.tabs.hoverPreview.showThumbnails": true };
  const context = {
    window: { addEventListener() {}, windowUtils: {
      syncWinUITabMenu: text => published.push(JSON.parse(text)),
    } },
    URL,
    document: { createElementNS: () => {
      const canvas = { getContext: () => ({ getImageData: (x, y, width, height) => ({ width, height,
        data: new Uint8ClampedArray(width * height * 4).fill(255).map((v, i) => i % 4 === 3 ? 128 : 200) }) }) };
      return canvas;
    } },
    PageThumbs: { captureTabPreviewThumbnail: (browser, canvas) =>
      new Promise(resolve => captures.push({ browser, canvas, resolve })) },
    Services: {
      prefs: { getBoolPref: (name, fallback) => prefs[name] ?? fallback },
      eTLD: { getBaseDomainFromHost: host => {
        if (/^[\d.]+$/.test(host)) throw new Error("NS_ERROR_HOST_IS_IP_ADDRESS");
        return { "news.bbc.co.uk": "bbc.co.uk", "www.example.org": "example.org" }[host] ?? host.split(".").slice(-2).join(".");
      } },
    },
    console: { error() {} },
  };
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  const tab = (spec, extra = {}) => ({ linkedBrowser: { currentURI: { spec } }, selected: false,
    hasAttribute: name => !!extra[name], ...extra });
  const tabs = new Map([[11, tab("https://news.bbc.co.uk/sport")], [22, tab("http://192.168.1.1:8080/", { selected: true })],
    [33, tab("about:reader?url=https%3A%2F%2Fwww.example.org%2Fa", { pending: true })], [44, tab("https://www.mozilla.org/", { pending: true })]]);
  Object.assign(pane, { element: {}, stripSession: {}, resolveStripTab: id => tabs.get(id) });
  const answers = () => published.map(p => p.tabPreview);

  // The selected tab, like Firefox's own preview, gets no thumbnail; an IP
  // address has no registrable domain and is shown as it is.
  assert.equal(pane.previewTab(22, 1, 1), true);
  assert.deepEqual(answers(), [{ tab: 22, request: 1, domain: "192.168.1.1", thumbnail: false }]);
  assert.equal(captures.length, 0);
  assert.equal(pane.previewTab(33, 2, 1), true);
  assert.equal(answers()[1].domain, "example.org", "reader view names the page it shows");
  assert.equal(pane.previewTab(44, 3, 1), true);
  assert.equal(answers()[2].thumbnail, false, "an unloaded tab has nothing to capture");
  assert.equal(pane.previewTab(99, 4, 1), false, "a tab no longer in the strip is refused");

  // The domain answers at once; the capture follows, sized to the card's
  // scale but held within the native pixel bound.
  assert.equal(pane.previewTab(11, 5, 2), true);
  assert.deepEqual(answers()[3], { tab: 11, request: 5, domain: "bbc.co.uk", thumbnail: true });
  assert.equal(captures.length, 1);
  const { canvas } = captures[0];
  assert.ok(canvas.width * canvas.height <= 100000 && canvas.width > 400, `${canvas.width}x${canvas.height}`);
  // A newer hover while capturing waits; the stale capture is never published.
  assert.equal(pane.previewTab(11, 6, 1), true);
  assert.equal(captures.length, 1, "one capture at a time");
  captures[0].resolve(true);
  await new Promise(resolve => setTimeout(resolve));
  assert.equal(captures.length, 2);
  assert.deepEqual([captures[1].canvas.width, captures[1].canvas.height], [280, 140]);
  captures[1].resolve(true);
  await new Promise(resolve => setTimeout(resolve));
  const last = answers().at(-1);
  assert.equal(answers().filter(a => a.image).length, 1);
  assert.equal(last.request, 6);
  assert.deepEqual([last.image.width, last.image.height], [280, 140]);
  assert.equal(last.image.pixels.length, 280 * 140 * 8);
  assert.equal(last.image.pixels.slice(0, 8), "64646480", "premultiplied BGRA");
  assert.equal(pane.previewCapturing, false);

  // A failed capture withdraws the reserved thumbnail rather than leaving it blank.
  assert.equal(pane.previewTab(11, 7, 1), true);
  captures[2].resolve(false);
  await new Promise(resolve => setTimeout(resolve));
  assert.deepEqual(answers().at(-1), { tab: 11, request: 7, domain: "bbc.co.uk", thumbnail: false, image: null });
  prefs["browser.tabs.hoverPreview.showThumbnails"] = false;
  assert.equal(pane.previewTab(11, 8, 1), true);
  assert.equal(answers().at(-1).thumbnail, false, "Firefox's thumbnail preference is honoured");
  assert.equal(captures.length, 3);
});


test("Alt search reveal publishes even when Firefox availability is already true", () => {
  const f = fixture([]);
  let invalidations = 0;
  f.context.gURLBar = {setUnifiedSearchButtonAvailability() {}};
  f.pane.addressSession = {sessionId:"reveal-test", modeReveal:0};
  f.pane.invalidateAddress = () => invalidations++;
  for (let i = 0; i < 2; ++i) {
    assert.equal(f.pane.executeAddressRequest({version:1,session:"reveal-test",kind:"mode-reveal"}),1);
  }
  assert.equal(f.pane.addressSession.modeReveal,2);
  assert.equal(invalidations,2);
});
