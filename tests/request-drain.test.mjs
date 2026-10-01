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
    window:{addEventListener() {}, docShell:{usePrivateBrowsing:false}},
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
    const sourceWindow = {docShell:{usePrivateBrowsing:false,
      treeOwner:{QueryInterface:() => ({nativeHandle:"0x1234"})}},
      gBrowser:{tabs:[tab]},
      gWinUIPane:{resolveStripTab: id => id === 7 ? tab : null,
        stripSession:{revision:4, windowDragSession:12}}};
    const adopted = [];
    const context = {window:{addEventListener() {}, docShell:{usePrivateBrowsing:false}},
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
    f => { f.sourceWindow.docShell.usePrivateBrowsing = true; },
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
