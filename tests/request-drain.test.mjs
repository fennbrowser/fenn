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
    gBrowser: {isTabPendingPermitUnload: () => false, removeTab() { throw new Error("controlled Firefox command failure"); }},
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

test("identical complete address containers coalesce, while dynamic state and sessions publish", () => {
  const f=fixture([]),sent=[];
  f.context.window.windowUtils.syncWinUIAddress=value=>sent.push(JSON.parse(value));
  const state={version:1,enabled:true,session:'one',text:'same',navigation:{back:{enabled:false}}};
  f.pane.publishAddress(state);f.pane.publishAddress({...state});
  assert.equal(sent.length,1);
  f.pane.publishAddress({...state,navigation:{back:{enabled:true}}});
  f.pane.publishAddress({...state,session:'two'});
  assert.equal(sent.length,3);
  f.context.window.windowUtils.syncWinUIAddress=()=>{throw Error('bridge failure');};
  const before=f.pane.addressPublication;
  assert.throws(()=>f.pane.publishAddress({...state,text:'retry'}));
  assert.equal(f.pane.addressPublication,before);
  f.context.window.windowUtils.syncWinUIAddress=value=>sent.push(JSON.parse(value));
  f.pane.publishAddress({...state,text:'retry'});
  assert.equal(sent.length,4);
});

test("completion of rejected native address input permits identical state to be applied again", () => {
  for(const kind of [6,7]) {
    const f=fixture([{id:1,kind,action:{}}]);
    f.pane.addressPublication='previously sent';
    f.pane.executeRequest=()=>2;
    f.pane.drain();
    assert.equal(f.pane.addressPublication,null);
    assert.equal(f.pane.addressDirty,true);
    assert.equal(f.completions[0].outcomes[0].outcome,2);
  }
});

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

test("background metadata skips address refresh; selected and unknown changes retain it", () => {
  const f=fixture([]), selected={}, background={}, calls=[];
  f.context.gBrowser.selectedTab=selected;
  f.pane.schedule=refresh=>calls.push(refresh);
  for(const target of [background,selected]) for(const name of ["label","busy","progress","image","muted","unknown"]) {
    f.pane.handleEvent({type:"TabAttrModified",target,detail:{changed:[name]}});
    assert.equal(calls.pop(),target===selected || !["label","busy","progress","image"].includes(name),name);
  }
  for(const changed of [undefined,[],["label","muted"]]) {
    f.pane.handleEvent({type:"TabAttrModified",target:background,detail:{changed}});
    assert.equal(calls.pop(),true);
  }
  f.pane.handleEvent({type:"TabSelect",target:selected});
  assert.equal(calls.pop(),true);
});

test("only background label text mutations omit the address refresh", () => {
  const f=fixture([]), selected={}, background={}, root={}, calls=[];
  const label=tab=>{const node={closest:()=>tab};tab.textLabel=node;return node;};
  const selectedLabel=label(selected), backgroundLabel=label(background);
  f.context.gBrowser.selectedTab=selected;
  f.context.document={documentElement:root};
  f.pane.urlbar={};f.pane.modeSwitcher={};f.pane.schedule=refresh=>calls.push(refresh);
  const record={type:"childList",target:backgroundLabel};
  for(const records of [[record],[record,record]]) {
    f.pane.addressDirty=false;f.pane.handleMutations(records);
    assert.equal(calls.pop(),false);assert.equal(f.pane.addressDirty,false);
  }
  for(const records of [[],[{...record,target:selectedLabel}], [{...record,type:"attributes"}],
    [{...record,target:{closest:()=>background}}], [record,{type:"attributes",target:root}],
    [record,{type:"attributes",target:f.pane.urlbar}], [record,{type:"attributes",target:f.pane.modeSwitcher}]]) {
    f.pane.handleMutations(records);assert.equal(calls.pop(),true);
  }
  assert.equal(f.pane.addressDirty,true);
  assert.equal(f.pane.stripDirty,true);
});

function schedulingFixture() {
  const f=fixture([]), tasks=new Map(), frames=new Map();let serial=0;
  f.context.window.setTimeout=callback=>{const id=++serial;tasks.set(id,callback);return id;};
  f.context.window.clearTimeout=id=>tasks.delete(id);
  f.context.requestAnimationFrame=callback=>{const id=++serial;frames.set(id,callback);return id;};
  f.context.cancelAnimationFrame=id=>frames.delete(id);
  f.context.Services.prefs={getBoolPref:()=>false,removeObserver(){}};
  f.context.Services.obs={removeObserver(){}};
  f.context.document={fullscreenElement:null,documentElement:{hasAttribute:()=>false}};
  f.context.gBrowser.tabContainer={removeEventListener(){}};
  f.context.window.removeEventListener=()=>{};
  f.context.window.windowUtils.updateWinUIPane=()=>true;
  f.pane.element={style:{},getBoundingClientRect:()=>({x:0,y:0,width:100,height:40}),remove(){}};
  f.pane.addressToolbar={close(){}};f.pane.addressDirty=false;
  f.pane.observer=f.pane.mutation={disconnect(){}};
  f.pane.occlusions=()=>[];
  f.pane.setReplacementActive=f.pane.setToolbarHidden=f.pane.updateToolbarHidden=()=>{};
  const take=queue=>{const [id,callback]=queue.entries().next().value;queue.delete(id);callback();};
  return {...f,tasks,frames,task:()=>take(tasks),frame:()=>take(frames)};
}

test("address publication is task-bound while strip publication retains its frame budget", () => {
  for(const order of [[false,false],[false,true],[true,false]]) {
    const f=schedulingFixture(),sent=[],strip=[];
    f.pane.publish=(force,includeAddress)=>{
      strip.push(f.context.gBrowser.selectedTab);
      if(includeAddress){sent.push([f.context.gBrowser.selectedTab,f.pane.addressDirty]);f.pane.addressDirty=false;}
    };
    for(const refresh of order)f.pane.schedule(refresh);
    assert.equal(f.tasks.size,Number(order.includes(true)));assert.equal(f.frames.size,1);
    f.context.gBrowser.selectedTab='latest';
    f.frame();assert.deepEqual(strip,['latest']);
    assert.equal(sent.length,0,"frame work must not construct or publish address state");
    if(order.includes(true)) {f.task();assert.deepEqual(sent,[['latest',true]]);}
    else assert.deepEqual(sent,[]);
    f.pane.schedule(false);f.pane.invalidateAddress();f.task();
    assert.equal(sent.at(-1)[1],true);
    f.pane.schedule();f.task();assert.equal(sent.at(-1)[1],true);
  }
});

test("suspended strip frames retain latest titles and metadata without scheduling tasks", () => {
  const f=schedulingFixture(),titles=new Map(),sent=[];
  const tab={},other={};
  f.pane.dirtyTitles=new Set();f.pane.stripDirty=false;
  f.pane.publish=(force,includeAddress)=>{
    sent.push({titles:[...f.pane.dirtyTitles].map(t=>titles.get(t)),metadata:f.pane.stripDirty,includeAddress});
    f.pane.dirtyTitles.clear();f.pane.stripDirty=false;
  };
  for(let i=0;i<12;i++) {
    titles.set(tab,`latest-${i}`);f.pane.dirtyTitles.add(tab);
    f.pane.stripDirty=true;f.pane.schedule(false);
  }
  titles.set(other,'other');f.pane.dirtyTitles.add(other);f.pane.schedule(false);
  assert.equal(f.tasks.size,0);assert.equal(f.frames.size,1);
  assert.equal(sent.length,0);assert.equal(f.pane.dirtyTitles.size,2);
  f.frame();
  assert.deepEqual(sent,[{titles:['latest-11','other'],metadata:true,includeAddress:false}]);
  assert.equal(f.pane.dirtyTitles.size,0);assert.equal(f.tasks.size,0);
});

test("address invalidation while a strip frame is suspended publishes live state promptly", () => {
  const f=schedulingFixture(),sent=[];
  f.pane.publish=(force,includeAddress)=>sent.push([f.context.gBrowser.selectedTab,includeAddress]);
  f.pane.schedule(false);assert.equal(f.tasks.size,0);
  f.pane.invalidateAddress();assert.equal(f.tasks.size,1);
  f.context.gBrowser.selectedTab='new document';f.task();
  assert.deepEqual(sent,[['new document',true]]);
  assert.equal(f.frames.size,1,"address work must not cancel the pending strip frame");
});

test("explicit reentrant publication recovery retains its task even with clean address state", () => {
  const f=schedulingFixture();runInNewContext(source,f.context);
  f.pane.publish=f.context.gWinUIPane.publish;
  let calls=0;
  f.pane.publishState=()=>{if(++calls===1)f.pane.publish(true);};
  f.pane.publish();assert.equal(f.pane.addressDirty,false);assert.equal(f.tasks.size,1);
  f.task();assert.equal(calls,2);assert.equal(f.pane.structuralDirty,true);
});

test("frame publication sends selection without consuming pending address state", () => {
  const f=schedulingFixture(),tabs=[{},{}],selected=[],addresses=[];
  runInNewContext(source,f.context);f.pane.publish=f.context.gWinUIPane.publish;
  f.pane.nativeRevision=1;f.pane.stripDirty=false;
  tabs.forEach((tab,i)=>f.pane.tabIds.set(tab,i+1));f.pane.publishedIds=new Set([1,2]);
  f.pane.visibleTabs=()=>tabs;f.pane.publishTabContext=()=>{};
  f.pane.publishTabDelta=()=>{selected.push(f.context.gBrowser.selectedTab);f.pane.dirtySelection=false;};
  f.pane.syncAddress=()=>addresses.push(f.context.gBrowser.selectedTab);
  f.context.gBrowser.selectedTab=tabs[0];f.pane.dirtySelection=true;
  f.pane.schedule();f.frame();
  assert.deepEqual(selected,[tabs[0]]);assert.equal(addresses.length,0);assert.equal(f.pane.addressDirty,true);
  f.context.gBrowser.selectedTab=tabs[1];f.pane.dirtySelection=true;
  f.task();assert.deepEqual(selected,tabs);assert.deepEqual(addresses,[tabs[1]]);
  assert.equal(f.pane.addressDirty,false);
});

test("a task before initial attachment preserves dirty work until geometry wakes it", () => {
  const f=schedulingFixture();f.pane.attached=false;
  f.pane.schedule();f.task();assert.equal(f.publications(),0);
  assert.equal(f.pane.addressDirty,true);assert.equal(f.tasks.size,0);
  f.frame();assert.equal(f.publications(),1);assert.equal(f.tasks.size,1);
  assert.equal(f.pane.addressDirty,true);
  f.task();assert.equal(f.publications(),2);
});

test("nested operations defer publication without polling and resume at their boundary", () => {
  for(const guard of ['processingAction','draining','publishing']) {
    const f=schedulingFixture();f.pane.schedule();f.pane[guard]=true;
    f.task();assert.equal(f.publications(),0);assert.equal(f.tasks.size,0);
    f.pane.resumePublication();assert.equal(f.tasks.size,0);
    f.pane[guard]=false;f.pane.resumePublication();f.task();
    assert.equal(f.publications(),1);assert.equal(f.pane.publicationDeferred,false);
  }
});

test("publication changes and reentrant force requests survive the current pass", () => {
  const f=schedulingFixture();runInNewContext(source,f.context);
  const originalPublish=f.context.gWinUIPane.publish;
  f.pane.publish=originalPublish;let calls=0;
  f.pane.publishState=()=>{
    calls++;
    if(calls===1){f.pane.invalidateAddress();f.pane.publish(true);}
  };
  f.pane.publish();assert.equal(calls,1);assert.equal(f.tasks.size,1);
  assert.equal(f.pane.structuralDirty,true,"nested force requires a subsequent snapshot");
  f.task();assert.equal(calls,2);assert.equal(f.pane.publishing,false);
});

test("teardown cancels both queues and old callbacks cannot mutate a new attachment", () => {
  const f=schedulingFixture();f.pane.schedule();
  f.pane.addressFailed=true;
  const oldTask=[...f.tasks.values()][0],oldFrame=[...f.frames.values()][0];
  f.pane.uninit();assert.equal(f.tasks.size,0);assert.equal(f.frames.size,0);
  assert.equal(f.pane.addressFailed,false,"publication failure cannot disable a replacement endpoint");
  f.pane.element={};f.pane.uninitializing=false;
  f.pane.schedule(false);const timer=f.pane.publicationTimer,frame=f.pane.frame;
  oldTask();oldFrame();
  assert.equal(f.pane.publicationTimer,timer);assert.equal(f.pane.frame,frame);
  assert.equal(f.publications(),0);
});

test("tab-only geometry pass preserves pending address work without manufacturing it", () => {
  const f=fixture([]);
  f.pane.element={style:{},getBoundingClientRect:()=>({x:0,y:0,width:100,height:40})};
  f.pane.addressToolbar={};
  f.context.Services.prefs={getBoolPref:()=>false};
  f.context.document={fullscreenElement:null,documentElement:{hasAttribute:()=>false}};
  f.context.window.windowUtils.updateWinUIPane=()=>true;
  f.pane.occlusions=()=>[];
  f.pane.setReplacementActive=f.pane.updateToolbarHidden=()=>{};
  for(const dirty of [false,true]) {
    f.pane.addressDirty=dirty;
    f.pane.update(false,false);
    assert.equal(f.pane.addressDirty,dirty);
  }
  f.pane.addressDirty=false;
  f.pane.update();
  assert.equal(f.pane.addressDirty,true,"direct updates preserve the default refresh");
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

test("single-tab identity changes keep context fresh without rebuilding strip metadata", () => {
  const f = fixture([]), tabs = [{}, {}, {}];
  f.pane.schedule = () => {};
  f.pane.stripSingleSelection = true;
  f.pane.stripDirty = false;
  for (const tab of tabs) {
    f.context.gBrowser.selectedTabs = [tab];
    f.pane.contextDirty = f.pane.dirtySelection = false;
    f.pane.handleEvent({type:"TabSelect", target:tab});
    f.pane.handleEvent({type:"TabMultiSelect", target:tab});
    assert.equal(f.pane.stripDirty, false);
    assert.equal(f.pane.contextDirty, true);
    assert.equal(f.pane.dirtySelection, true);
  }
});

test("only selection eligibility transitions dirty metadata; coalescing cannot erase dirty work", () => {
  const f = fixture([]), tabs = [{}, {}, {}];
  f.pane.schedule = () => {};
  f.pane.stripSingleSelection = true;
  for (const [selection, dirty] of [[tabs.slice(0,2), true], [tabs, false],
      [tabs.slice(1), false], [[tabs[2]], true]]) {
    f.context.gBrowser.selectedTabs = selection;
    f.pane.stripDirty = f.pane.contextDirty = false;
    f.pane.handleEvent({type:"TabMultiSelect", target:selection[0]});
    assert.equal(f.pane.stripDirty, dirty);
    assert.equal(f.pane.contextDirty, true);
  }
  // A 1 -> 2 -> 1 burst before publication must retain the first invalidation.
  f.pane.stripDirty = false;
  for (const selection of [tabs.slice(0,2), [tabs[0]]]) {
    f.context.gBrowser.selectedTabs = selection;
    f.pane.handleEvent({type:"TabMultiSelect", target:selection[0]});
    assert.equal(f.pane.stripDirty, true);
  }
  for (const event of [{type:"TabAttrModified", detail:{changed:["image"]}},
      {type:"TabGroupCollapse"}, {type:"TabGroupExpand"}, {type:"TabGrouped"}]) {
    f.pane.stripDirty = false;
    f.pane.handleEvent({...event, target:tabs[0]});
    assert.equal(f.pane.stripDirty, true, event.type);
    f.pane.handleEvent({type:"TabMultiSelect", target:tabs[0]});
    assert.equal(f.pane.stripDirty, true, "unchanged eligibility preserves other dirty reasons");
  }
});

test("selection away from a collapsed retained member still forces an ordered snapshot", () => {
  const f = fixture([]), active = {}, other = {};
  f.pane.nativeRevision = 1;
  f.pane.publishedIds = new Set([1,2]);
  f.pane.tabIds.set(active, 1); f.pane.tabIds.set(other, 2);
  f.pane.visibleTabs = () => [other];
  f.pane.dirtySelection = true;
  f.pane.stripDirty = false;
  let snapshots = 0;
  f.pane.syncTabs = () => snapshots++;
  f.pane.publishState();
  assert.equal(snapshots, 1, "visible membership is checked independently of metadata invalidation");
});

test("unchanged selection eligibility retains a consumed gesture's publication boundary", () => {
  const f = fixture([]), tab = {};
  f.pane.schedule = () => {};
  f.context.gBrowser.selectedTabs = [tab];
  f.pane.stripSingleSelection = true;
  f.pane.stripDirty = false;
  f.pane.stripSession = {consumed:true};
  f.pane.handleEvent({type:"TabMultiSelect", target:tab});
  assert.equal(f.pane.stripDirty, true);
  assert.equal(f.pane.contextDirty, true);
});

test("multiselection republishes grouping eligibility as row deltas without rebuilding layout", () => {
  const tabs = [1, 2].map(() => ({isConnected:true, visible:true, hasAttribute:() => false}));
  const browser = {isTabPendingPermitUnload: () => false, tabs, selectedTabs:[tabs[0]], tabGroups:[], getIcon:() => ""};
  let selected = browser.selectedTabs, selectionReads = 0;
  Object.defineProperty(browser, "selectedTabs", {
    get() {selectionReads++; return selected;}, set(value) {selected = value;},
  });
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
  assert.equal(selectionReads, 1, "one selection read per description, not two per row");
  assert.ok(session.state.rows.every(row => row.groupSource && row.groupTarget));

  for (const selectedTabs of [tabs, [tabs[0]]]) {
    browser.selectedTabs = selectedTabs;
    pane.stripDirty = pane.structuralDirty = pane.contextDirty = false;
    pane.handleEvent({type:"TabMultiSelect", target:tabs[0]});
    assert.equal(pane.stripDirty, true, "eligibility changes must reach the native strip");
    assert.equal(pane.contextDirty, true, "menu multiselection state also changes");
    assert.equal(pane.structuralDirty, false, "membership and order remain intact");
    const readsBefore = selectionReads;
    const delta = session.update().strip;
    assert.equal(selectionReads - readsBefore, 1);
    const eligible = selectedTabs.length === 1;
    assert.equal(delta.base, previous);
    assert.deepEqual(delta.rows, [1, 2].map(id => ({id, groupSource:eligible, groupTarget:eligible})));
    assert.deepEqual(delta.removed, []);
    for (const key of ["layout", "groups", "groupingDelay"]) assert.equal(key in delta, false, key);
    previous = delta.revision;
    assert.equal(session.update(), null, "unchanged eligibility produces no publication");
  }
  // Action validation must read live state even before the next event arrives.
  browser.selectedTabs = tabs;
  assert.equal(pane.stripSingleSelection, true);
  assert.equal(pane.canDragGroupTab(tabs[0]), false);
  browser.selectedTabs = [tabs[0]];
  assert.equal(pane.canDragGroupTab(tabs[0]), true);
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

test("native close rejects Firefox's pending unload state without a tab property", () => {
  const f = fixture([]), tab = {};
  let closes = 0;
  f.pane.resolveStripTab = () => tab;
  f.context.gBrowser.isTabPendingPermitUnload = value => value === tab;
  f.context.gBrowser.removeTab = () => closes++;
  assert.equal(f.pane.executeCloseRequest({tab:9}), 2);
  assert.equal(closes, 0);
  f.context.gBrowser.isTabPendingPermitUnload = () => false;
  assert.equal(f.pane.executeCloseRequest({tab:9}), 1);
  assert.equal(closes, 1);
});


test("address request completion republishes state skipped while native input was pending", () => {
  for (const kind of [6, 7, 8]) {
    for (const outcome of [1, 2]) {
      const f = fixture([{id: 1, kind, action: {}}]);
      f.pane.addressDirty = false;
      f.pane.executeRequest = () => {
        // A publication during execution can be deferred by native input.
        f.pane.addressDirty = false;
        return outcome;
      };
      let afterCompletion;
      f.pane.publish = () => {
        assert.equal(f.completions.length, 1);
        afterCompletion = f.pane.addressDirty;
      };
      f.pane.drain();
      assert.equal(afterCompletion, kind !== 8,
        "editor/mode outcomes must republish; visibility reports must not loop");
      assert.equal(f.pane.structuralDirty, false);
    }
  }
});


test("locale refresh retains the full resource set and tolerates missing native attributes", () => {
  const f=fixture([]),resources=[];
  f.context.Localization=class {
    constructor(files){resources.push(Array.from(files));}
    formatValueSync(id){return id;}
    formatMessagesSync(){return [null];}
  };
  f.pane.element={setAttribute(){}};f.pane.schedule=()=>{};
  const before=f.pane.localization();
  assert.doesNotThrow(()=>f.pane.nativeLabels());
  f.pane.observe(null,"intl:app-locales-changed");
  assert.notEqual(f.pane.localization(),before);
  assert.deepEqual(resources[1],resources[0]);
  assert.ok(resources[1].includes("browser/browserContext.ftl"));
  assert.ok(resources[1].includes("branding/brand.ftl"));
  assert.equal(f.pane.nativeLabels().navigation.back.label,undefined);
});
