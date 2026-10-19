import assert from "node:assert/strict";
import {test} from "node:test";
import {WinUIProductMenu, PRODUCT_MENU_PREF, PRODUCT_MENU_LIMITS} from "../upstream/firefox/browser/base/content/WinUIProductMenu.sys.mjs";

const LABELS = {
  ...Object.fromEntries(["recently-visited", "recently-closed", "show-history", "recent-bookmarks",
    "add-extension", "manage-extensions", "show-sidebar", "autohide-tabs", "reset-zoom", "developer-tools",
    "inspect-element", "browser-console", "set-default", "bookmarks-toolbar", "bookmarks-menu", "other-bookmarks",
    "empty", "empty-folder", "next", "previous", "back"].map(key => [`winui-menu-${key}`, key])),
  "winui-history-empty":"No recent history", "appmenuitem-extensions":"Extensions", "menu-view":"View",
  "menu-view-full-zoom-enlarge":"Zoom in", "menu-view-full-zoom-reduce":"Zoom out",
  "menu-tools-task-manager":"Task manager", "appmenuitem-print":"Print", "menu-help":"Help",
  "menu-get-help":"Get help", "menu-help-more-troubleshooting-info":"Troubleshooting", "menu-about":"About Fenn",
  ...Object.fromEntries(["menu-report-broken-site","menu-help-share-ideas","menu-help-enter-troubleshoot-mode2",
    "menu-help-report-deceptive-site","menu-help-not-deceptive","menu-help-switch-device"].map(key=>[key,key])),
  "appmenuitem-new-tab": "New Tab", "appmenuitem-new-window": "New Window",
  "appmenuitem-new-private-window": "New Private Window", "appmenuitem-history": "History",
  "menu-history-show-all-history": "Show All History", "menu-history-undo-menu": "Recently Closed Tabs",
  "appmenuitem-downloads": "Downloads", "menu-bookmarks-menu": "Bookmarks",
  "toolbar-context-menu-bookmark-selected-tab": "Bookmark Tab", "menu-bookmarks-toolbar": "Bookmarks Toolbar",
  "toolbar-context-menu-bookmarks-toolbar-always-show-2": "Always Show",
  "toolbar-context-menu-bookmarks-toolbar-on-new-tab-2": "Only Show on New Tab",
  "toolbar-context-menu-bookmarks-toolbar-never-show-2": "Never Show",
  "library-bookmarks-menu": "Manage Bookmarks", "appmenuitem-settings": "Settings",
  "toolbar-context-menu-view-customize-toolbar-2": "Customize Toolbar…",
  "menu-file-close-window": "Close Window", "appmenuitem-exit2": "Exit",
  "appmenu-menu-button-closed2": "Fenn",
  "winui-update-check": "Check for updates",
  ...Object.fromEntries(["title", "checking", "current", "available-store", "available-feed",
    "unavailable", "failed", "open-store", "open-feed", "close",
    "downloading", "ready", "install", "installing"]
    .map(key => [`winui-update-${key}`, `Localized update ${key}`])),
};

function fixture(overrides = {}) {
  const prefs = new Map([[PRODUCT_MENU_PREF, true], ["browser.toolbars.bookmarks.visibility", "newtab"]]);
  const observers = new Map(), calls = [];
  globalThis.Services = {prefs: {
    getBoolPref: (name, fallback) => prefs.get(name) ?? fallback,
    getCharPref: (name, fallback) => prefs.get(name) ?? fallback,
    setCharPref: (name, value) => { calls.push(["pref", name, value]); prefs.set(name, value); },
    addObserver: (name, observer) => observers.set(name, observer),
    removeObserver: name => observers.delete(name),
  }};
  const record = name => (...args) => calls.push([name, ...args]);
  const window = {
    gDialogBox: {isOpen: false},
    SessionStore: {getLastClosedTabCount: () => overrides.closed ?? 0},
    SessionWindowUI: {undoCloseTab: record("undo")},
    BrowserCommands: {openTab: record("new-tab"), tryToCloseWindow: record("close")},
    OpenBrowserWindow: record("window"),
    PlacesCommandHook: {showPlacesOrganizer: record("organizer"), bookmarkTabs: record("bookmark")},
    CustomizableUI: {setToolbarVisibility: record("visibility")},
    gBrowser: {selectedTabs: ["tab"]},
    openPreferences: record("settings"), goQuitApplication: record("quit"),
    ShortcutUtils: {prettifyShortcut: key => key.shortcut},
    document: {getElementById: id => id === "key_newNavigatorTab" ? {shortcut: "Ctrl+T"} : null},
    Localization: class {
      constructor(resources) {
        assert.ok(resources.includes("branding/brand.ftl"), "product/update labels can resolve brand terms");
      }
      formatMessagesSync([{id}]) {
        const value = LABELS[id];
        if (value === undefined) throw new Error(`missing ${id}`);
        return [{value, attributes: [{name: "label", value}, {name: "tooltiptext", value: `${value} menu`}]}];
      }
    },
  };
  const host = {invalidate: record("invalidate"), openMenu: (...args) => { calls.push(["menu", ...args]); return 1; }};
  return {menu: new WinUIProductMenu(window, host), prefs, observers, calls, window};
}

test("a disabled preference publishes nothing and refuses every command", () => {
  const f = fixture();
  f.prefs.set(PRODUCT_MENU_PREF, false);
  assert.deepEqual(f.menu.model(), {enabled: false});
  assert.equal(f.menu.execute("new-tab", {}, {}), 2);
  assert.deepEqual(f.calls, []);
});

test("the model is a bounded cascading menu with History and Bookmarks as submenus", () => {
  const model = fixture().menu.model();
  assert.equal(model.enabled, true);
  assert.equal(model.label, "Fenn");
  const top = model.items.filter(item => item.kind !== "separator").map(item => [item.id, item.kind]);
  assert.deepEqual(top, [["new-tab", "item"], ["new-window", "item"], ["new-private-window", "item"],
    ["history", "submenu"], ["downloads", "item"], ["bookmarks", "submenu"], ["extensions", "submenu"],
    ["view", "submenu"], ["print", "item"], ["help", "submenu"], ["set-default", "item"], ["settings", "item"],
    ["customize-toolbar", "item"], ["check-updates", "item"], ["close-window", "item"], ["quit", "item"]]);
  const bookmarks = model.items.find(item => item.id === "bookmarks");
  assert.ok(!bookmarks.items.some(item => item.id === "bookmarks-toolbar"));
  const toolbar = model.items.find(item => item.id === "view").items.find(item => item.id === "bookmarks-toolbar");
  assert.equal(toolbar.kind, "submenu");
  assert.deepEqual(toolbar.items.map(item => [item.id, item.checked]),
    [["bookmarks-toolbar-always", false], ["bookmarks-toolbar-newtab", true], ["bookmarks-toolbar-never", false]]);
  const ids = [];
  const walk = (items, depth) => {
    assert.ok(depth <= PRODUCT_MENU_LIMITS.depth);
    assert.ok(items.length <= PRODUCT_MENU_LIMITS.items);
    for (const item of items) {
      if (item.kind === "separator") continue;
      assert.match(item.id, PRODUCT_MENU_LIMITS.idPattern);
      assert.ok(item.label.length > 0 && item.label.length <= PRODUCT_MENU_LIMITS.label);
      ids.push(item.id);
      if (item.items) walk(item.items, depth + 1);
    }
  };
  walk(model.items, 1);
  assert.equal(new Set(ids).size, ids.length, "ids are unique");
  assert.equal(model.items.at(0).shortcut, "Ctrl+T");
});

test("separators never lead, trail or double up", () => {
  const items = fixture().menu.model().items;
  assert.notEqual(items.at(0).kind, "separator");
  assert.notEqual(items.at(-1).kind, "separator");
  items.forEach((item, i) => item.kind === "separator" && assert.notEqual(items[i + 1].kind, "separator"));
});

test("restoring a closed tab is enabled only when one exists", () => {
  assert.equal(fixture().menu.find("history-restore-tab").enabled, false);
  assert.equal(fixture({closed: 2}).menu.find("history-restore-tab").enabled, true);
});

test("commands run Firefox's own handlers", () => {
  const f = fixture({closed: 1});
  const event = {type: "click"};
  for (const id of ["new-tab", "new-window", "new-private-window", "history-show-all",
    "history-restore-tab", "bookmarks-show-all", "settings", "close-window", "quit"]) {
    assert.equal(f.menu.execute(id, {}, event), 1, id);
  }
  assert.deepEqual(f.calls.map(call => call[0]),
    ["new-tab", "window", "window", "organizer", "undo", "organizer", "settings", "close", "quit"]);
  assert.deepEqual(f.calls[2], ["window", {private: true}]);
  assert.deepEqual(f.calls[3], ["organizer", "History"]);
});

test("downloads route through the shared panel opener with the native bounds", () => {
  const f = fixture();
  const bounds = {x: 1, y: 2, width: 32, height: 32};
  assert.equal(f.menu.execute("downloads", bounds, {}), 1);
  assert.deepEqual(f.calls, [["menu", "downloads", bounds, {}]]);
});

test("the bookmarks toolbar choice writes Firefox's preference and visibility", () => {
  const f = fixture();
  assert.equal(f.menu.execute("bookmarks-toolbar-never", {}, {}), 1);
  assert.equal(f.prefs.get("browser.toolbars.bookmarks.visibility"), "never");
  assert.deepEqual(f.calls.find(call => call[0] === "visibility"), ["visibility", "PersonalToolbar", "never"]);
  assert.equal(f.menu.find("bookmarks-toolbar-never").checked, true);
});

test("submenu parents, unknown, malformed and stale ids are refused", () => {
  const f = fixture();
  for (const id of ["history", "bookmarks", "bookmarks-toolbar", "nope", "../x", "", "New-Tab", null, 5,
    "bookmarks-toolbar-sometimes"]) {
    assert.equal(f.menu.execute(id, {}, {}), 2, String(id));
  }
  assert.deepEqual(f.calls, []);
});

test("the customize item is native-only and never runs a Firefox command", () => {
  const f = fixture();
  assert.equal(f.menu.find("customize-toolbar").kind, "item");
  assert.equal(f.menu.execute("customize-toolbar", {}, {}), 2);
  assert.deepEqual(f.calls, []);
});

test("update checks publish localized Windows UI and never invoke Firefox's updater", () => {
  const f = fixture();
  assert.equal(f.menu.find("check-updates").label, "Check for updates");
  const labels = f.menu.model().updateLabels;
  assert.equal(Object.keys(labels).length, 14);
  for (const [key, value] of Object.entries(labels)) assert.equal(value, `Localized update ${key}`);
  assert.equal(f.menu.execute("check-updates", {}, {}), 2, "handled by the native package host");
  assert.deepEqual(f.calls, []);
  f.window.gDialogBox.isOpen = true;
  assert.equal(f.menu.find("check-updates").enabled, false);
});

test("an open Firefox dialog disables every command", () => {
  const f = fixture();
  f.window.gDialogBox.isOpen = true;
  assert.equal(f.menu.execute("new-tab", {}, {}), 2);
});

test("preference changes invalidate the address publication and close removes observers", () => {
  const f = fixture();
  assert.ok(f.observers.has(PRODUCT_MENU_PREF));
  f.observers.get(PRODUCT_MENU_PREF).observe();
  assert.deepEqual(f.calls.at(-1), ["invalidate"]);
  f.menu.close();
  assert.equal(f.observers.size, 0);
});

function updateFixture({ refuse = false, saveError = false } = {}) {
  const f = fixture();
  f.window.canQuitApplication = reason => { f.calls.push(["quit-check", reason]); return !refuse; };
  f.window.CanCloseWindow = () => { f.calls.push(["unload"]); return true; };
  Services.wm = { getEnumerator: () => [f.window] };
  Services.prefs.setBoolPref = (key, value) => { f.prefs.set(key, value); f.calls.push(["restore", value]); };
  Services.prefs.savePrefFile = () => f.calls.push(["prefs-saved"]);
  globalThis.ChromeUtils = { importESModule: name => name.includes("TabStateFlusher")
    ? { TabStateFlusher: { flushWindow: async window => { assert.equal(window, f.window); f.calls.push(["flush"]); } } }
    : { SessionSaver: { run: async options => {
      assert.deepEqual(options, { rejectOnFailure: true });
      f.calls.push(["save"]);
      if (saveError) throw new Error("Disk full");
    } } } };
  return f;
}

async function updateSettled(menu) {
  for (let i = 0; i < 20 && menu.updateInstall?.status === "pending"; ++i) await Promise.resolve();
  assert.notEqual(menu.updateInstall?.status, "pending");
}

test("update approval follows unload checks, content flush and durable session save", async () => {
  const f = updateFixture();
  assert.equal(f.menu.execute("prepare-update-123-1", {}, {}), 1);
  assert.equal(f.menu.model().updateInstall.status, "pending");
  assert.equal(f.menu.execute("prepare-update-123-2", {}, {}), 2);
  await updateSettled(f.menu);
  assert.deepEqual(f.menu.model().updateInstall, { token: "123-1", status: "ready" });
  assert.deepEqual(f.calls.map(call => call[0]), ["quit-check", "unload", "flush", "restore", "prefs-saved", "save", "invalidate"]);
  assert.equal(f.menu.execute("update-failed-999-1", {}, {}), 2);
  assert.equal(f.menu.execute("update-failed-123-1", {}, {}), 1);
  assert.equal(f.prefs.get("browser.sessionstore.resume_session_once"), false);
  f.menu.close();
});

test("a failed session save refuses installation and restores the prior preference", async () => {
  const f = updateFixture({ saveError: true });
  f.menu.execute("prepare-update-123-3", {}, {});
  await updateSettled(f.menu);
  assert.equal(f.menu.model().updateInstall.status, "failed");
  assert.equal(f.prefs.get("browser.sessionstore.resume_session_once"), false);
  f.menu.close();
});

test("declined quit never flushes or grants an update", async () => {
  const f = updateFixture({ refuse: true });
  f.menu.execute("prepare-update-123-4", {}, {});
  await updateSettled(f.menu);
  assert.equal(f.menu.model().updateInstall.status, "failed");
  assert.equal(f.calls.some(call => call[0] === "save" || call[0] === "flush"), false);
  f.menu.close();
});

test("teardown during preparation cannot publish a late install grant", async () => {
  const f = updateFixture();
  f.menu.execute("prepare-update-123-5", {}, {});
  f.menu.close();
  await updateSettled(f.menu);
  assert.equal(f.menu.updateInstall.status, "failed");
  assert.equal(f.calls.some(call => call[0] === "save"), false);
});

test("update shutdown retains one-time session restoration", async () => {
  const f = updateFixture();
  f.menu.execute("prepare-update-123-6", {}, {});
  await updateSettled(f.menu);
  f.menu.close();
  assert.equal(f.prefs.get("browser.sessionstore.resume_session_once"), true);
});


test("preference persistence failure refuses installation even when rollback also fails", async () => {
  const f = updateFixture();
  Services.prefs.savePrefFile = () => { throw new Error("Preferences are read-only"); };
  f.menu.execute("prepare-update-123-7", {}, {});
  await updateSettled(f.menu);
  assert.equal(f.menu.updateInstall.status, "failed");
  assert.equal(f.calls.some(call => call[0] === "save"), false);
  f.menu.close();
});

function listFixture(count = 20) {
  const f = fixture();
  const roots = [];
  const makeRoot = rows => {
    const root = {containerOpen:true, childCount:rows.length, getChild:i=>rows[i]};
    for (const row of rows) row.parent = root;
    roots.push(root);
    return root;
  };
  const bookmarks = Array.from({length:count}, (_,i)=>({title:`Bookmark ${i}`,uri:`https://example.com/${i}`,bookmarkGuid:`bookmark-${i}`}));
  const folders = new Map([
    ["toolbar", [{title:"Nested folder",folder:true,bookmarkGuid:"nested"},...bookmarks]],
    ["menu", bookmarks.map(row=>({...row}))], ["unfiled", []], ["nested", [{title:"Nested bookmark",uri:"https://example.com/nested"}]],
  ]);
  let closed = [{closedId:5,sourceWindowId:"source",title:"Closed A",closedAt:1},
    {closedId:6,sourceWindowId:"source",title:"Closed B",closedAt:2}];
  f.window.PrivateBrowsingUtils = {isWindowPrivate:()=>false};
  Object.assign(f.window.SessionStore, {
    getClosedTabData:()=>closed.slice(), getWindowById:()=>f.window,
    getClosedTabDataForWindow:()=>closed.slice(),
  });
  f.window.BrowserAddonUI = {openAddonsMgr:view=>f.calls.push(["addons",view])};
  f.window.PlacesUIUtils = {openNodeIn:(...args)=>f.calls.push(["visit",...args])};
  const places = {
    history:{getNewQuery:()=>({}),getNewQueryOptions:()=>({}),executeQuery:()=>({root:makeRoot([{title:"Visited",uri:"https://example.com/visited"}])})},
    bookmarks:{toolbarGuid:"toolbar", menuGuid:"menu",unfiledGuid:"unfiled",fetch:async guid=>f.data.recent.find(row=>row.guid===guid)},
    getFolderContents:guid=>({root:makeRoot((folders.get(guid)||[]).map(row=>({...row})))}),
    nodeIsSeparator:node=>!!node.separator,nodeIsFolderOrShortcut:node=>!!node.folder,getConcreteItemGuid:node=>node.bookmarkGuid,
    nodeIsURI:node=>!!node.uri,nodeIsBookmark:node=>!!node.bookmarkGuid,
  };
  globalThis.Ci = {nsINavHistoryQueryOptions:{SORT_BY_DATE_DESCENDING:4},nsINavHistoryResultNode:{RESULT_TYPE_URI:0}};
  globalThis.ChromeUtils = {importESModule:name=>name.includes("BrowserUtils") ? {BrowserUtils:{whereToOpenLink:event=>event?.ctrlKey ? "tab" : "current"}}
    : {AddonManager:{getAddonByID:async id=>f.data.extensions.find(addon=>addon.id===id)}}};
  f.data = {PlacesUtils:places,recent:[{guid:"recent",title:"Recent bookmark",url:new URL("https://example.com/recent")}],
    extensions:Array.from({length:count},(_,i)=>({id:`addon-${i}@example`,name:`Extension ${i}`}))};
  f.menu.loadData = async()=>f.data;
  return {...f,roots,folderData:folders,removeClosed:()=>{closed=closed.slice(1);}};
}

test("View owns toolbar visibility and unavailable features cannot execute", () => {
  const f = fixture();
  for (const id of ["view-sidebar","view-autohide-tabs"]) {
    assert.equal(f.menu.find(id).enabled,false);
    assert.equal(f.menu.execute(id,{},{}),2);
  }
  const extensionIds = f.menu.find("extensions").items.map(item=>item.id);
  assert.deepEqual(extensionIds,["extensions-add","extensions-manage"]);
  f.prefs.set("devtools.policy.disabled",true);
  for (const id of ["developer-tools","inspect-element","browser-console"]) assert.equal(f.menu.execute(id,{},{}),2);
});

test("Print, Help, zoom, developer tools, add-ons and default-browser use Firefox entry points", () => {
  const f=fixture();
  const commands=new Map(["cmd_print","View:AboutProcesses","menu_openHelp","troubleShooting","aboutName"]
    .map(id=>[id,{doCommand:()=>f.calls.push(["command",id])}]));
  f.window.document.getElementById=id=>commands.get(id);
  f.window.FullZoom=Object.fromEntries(["enlarge","reduce","reset","resetScalingZoom"].map(id=>[id,()=>f.calls.push(["zoom",id])]));
  f.window.BrowserAddonUI={openAddonsMgr:view=>f.calls.push(["addons",view])};
  globalThis.ChromeUtils={importESModule:()=>({ShellService:{setAsDefault:async()=>f.calls.push(["default"])}})};
  globalThis.Cc={"@mozilla.org/devtools/startup-clh;1":{getService:()=>({wrappedJSObject:{onKey:(_window,key)=>f.calls.push(["devtools",key])}})}};
  globalThis.Ci={nsICommandLineHandler:{}};
  for(const id of ["print","task-manager","help-support","help-troubleshooting","help-about",
    "zoom-in","zoom-out","zoom-reset","developer-tools","inspect-element","browser-console",
    "extensions-add","extensions-manage","set-default"]) assert.equal(f.menu.execute(id,{},{}),1,id);
  assert.deepEqual(f.calls.filter(call=>call[0]==="command").map(call=>call[1]),[...commands.keys()]);
  assert.deepEqual(f.calls.filter(call=>call[0]==="zoom").map(call=>call[1]),["enlarge","reduce","reset","resetScalingZoom"]);
  assert.deepEqual(f.calls.filter(call=>call[0]==="devtools").map(call=>call[1]),[{toolId:"inspector"},{id:"browserConsole"}]);
  assert.ok(f.calls.some(call=>call[0]==="menu" && call[1]==="developer-tools"));
  commands.get("cmd_print").disabled=true;
  assert.equal(f.menu.execute("print",{},{}),2);
});

test("fresh lists preserve ordering, bounds, folder navigation and all extension pages", async () => {
  const f = listFixture(80);
  await f.menu.refresh("open-1-1");
  const history=f.menu.find("history").items;
  assert.equal(history[0].id,"history-recent-heading");
  assert.equal(history.at(-1).id,"history-show-all");
  assert.equal(history[1].label,"Visited");
  assert.equal(f.menu.find("bookmarks").items[1].label,"Recent bookmark");
  const seen=[];
  for (;;) {
    const model=f.menu.model(); let total=0;
    const walk=(items,depth)=>{assert.ok(depth<=3);assert.ok(items.length<=32);for(const item of items){total++;if(item.items)walk(item.items,depth+1);}};
    walk(model.items,1);assert.ok(total<=128,`bounded total ${total}`);
    assert.ok(JSON.stringify(model).length<65536);
    const extensions=f.menu.find("extensions").items;
    assert.deepEqual(extensions.slice(0,3).map(item=>item.id||item.kind),["extensions-add","extensions-manage","separator"]);
    seen.push(...extensions.filter(item=>item.label?.startsWith("Extension ")).map(item=>item.label));
    const next=extensions.find(item=>item.label==="next");
    if(!next)break;
    assert.equal(f.menu.execute(next.id,{},{}),1);
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(f.menu.model().request,next.id);
  }
  assert.equal(new Set(seen).size,80);
  const folder=f.menu.find("bookmarks-list-toolbar").items.find(item=>item.label==="Nested folder");
  assert.equal(f.menu.execute(folder.id,{},{}),1);
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(f.menu.find("bookmarks-list-toolbar").label,"Nested folder");
  assert.ok(f.menu.find("bookmarks-list-toolbar").items.some(item=>item.label==="Nested bookmark"));
  f.menu.close();
  assert.ok(f.roots.every(root=>!root.containerOpen));
});

test("live node, closed-tab and extension identities reject removed or superseded entries", async () => {
  const f=listFixture();await f.menu.refresh("open-2-1");
  const history=f.menu.find("history").items.find(item=>item.label==="Visited");
  const old=history.id;
  assert.equal(f.menu.execute(old,{}, {ctrlKey:true}),1);
  assert.equal(f.calls.at(-1)[2],"tab");
  f.roots[0].getChild(0).uri="https://changed.example/";
  assert.equal(f.menu.execute(old,{},{}),2);
  const closed=f.menu.find("history").items.find(item=>item.label==="Closed A");
  f.removeClosed();assert.equal(f.menu.execute(closed.id,{},{}),2);
  const addon=f.menu.find("extensions").items.find(item=>item.label==="Extension 0");
  f.data.extensions=[];assert.equal(f.menu.execute(addon.id,{},{}),1);
  await new Promise(resolve=>setImmediate(resolve));
  assert.ok(!f.calls.some(call=>call[0]==="addons"));
  await f.menu.refresh("open-2-2");assert.equal(f.menu.execute(old,{},{}),2);
});

test("dialogs disable loaded entries without permanently changing their state", async () => {
  const f=listFixture();await f.menu.refresh("open-3-1");
  const id=f.menu.find("history").items[1].id;
  f.window.gDialogBox.isOpen=true;assert.equal(f.menu.execute(id,{},{}),2);
  f.window.gDialogBox.isOpen=false;assert.equal(f.menu.find(id).enabled,true);
});

test("late loads cannot replace a newer menu or survive teardown", async () => {
  const f=listFixture();let resolve;
  f.menu.loadData=()=>new Promise(done=>{resolve=done;});
  const old=f.menu.refresh("open-4-1");const complete=resolve;
  f.menu.loadData=async()=>f.data;
  await f.menu.refresh("open-4-2");complete(f.data);await old;
  assert.equal(f.menu.model().request,"open-4-2");
  f.menu.loadData=()=>new Promise(done=>{resolve=done;});
  const closing=f.menu.refresh("open-4-3");f.menu.close();resolve(f.data);await closing;
  assert.equal(f.menu.actions.size,0);
});

test("an asynchronous recent bookmark cannot navigate a different selected document", async () => {
  const f=listFixture();await f.menu.refresh("open-5-1");
  const row=f.menu.find("bookmarks").items.find(item=>item.label==="Recent bookmark");
  let complete;f.data.PlacesUtils.bookmarks.fetch=()=>new Promise(resolve=>{complete=resolve;});
  f.window.gBrowser.selectedBrowser={browsingContext:{currentWindowGlobal:{innerWindowId:1}}};
  assert.equal(f.menu.execute(row.id,{},{}),1);
  f.window.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId=2;
  complete(f.data.recent[0]);await new Promise(resolve=>setImmediate(resolve));
  assert.ok(!f.calls.some(call=>call[0]==="visit"));
});

test("maximum folder paging, extension pages and all Help commands stay in native bounds", async () => {
  const f=listFixture(80);
  f.window.document.getElementById=()=>({});
  f.data.recent=Array.from({length:6},(_,i)=>({guid:`recent-${i}`,title:`Recent ${i}`,url:new URL(`https://example.com/${i}`)}));
  f.menu.pages.set("extensions",6);
  for(const key of ["toolbar","menu","unfiled"]) {
    f.menu.folders.set(key,[{guid:"toolbar",title:"Nested"}]);f.menu.pages.set(key,6);
  }
  await f.menu.refresh("open-6-1");
  let total=0;
  const walk=(items,depth)=>{assert.ok(items.length<=32);assert.ok(depth<=3);for(const item of items){total++;if(item.items)walk(item.items,depth+1);}};
  walk(f.menu.model().items,1);
  assert.ok(total+6<=128,`full native model, including eight closed-tab/pager rows, has ${total+6} entries`);
});
