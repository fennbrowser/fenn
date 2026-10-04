import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";
import {runInNewContext} from "node:vm";
import {WinUITabMenuSession} from "../upstream/firefox/browser/base/content/WinUITabMenuSession.sys.mjs";
const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
function fixture() {
  const calls = [], state = {hidden: true, closed: 0, all: false};
  const toolbars = {"toolbar-menubar": {hasAttribute: name => name === "toolbarname" || name === "autohide" && state.hidden,
    getAttribute: name => name === "toolbarname" ? "Menu Bar" : ""},
    PersonalToolbar: {hasAttribute: name => name === "toolbarname", getAttribute: () => "Bookmarks Toolbar"}};
  const context = {window: {addEventListener() {}}, document: {getElementById: id => toolbars[id]},
    gDialogBox: {isOpen: false}, gBookmarksToolbarVisibility: "newtab",
    gBrowser: {selectedTabs: [{}], allTabsSelected: () => state.all,
      reloadMultiSelectedTabs: () => calls.push("reload"), selectAllTabs: () => calls.push("select-all")},
    SessionStore: {getLastClosedTabCount: () => state.closed},
    BrowserCommands: {openTab: () => calls.push("new")},
    SessionWindowUI: {undoCloseTab: () => calls.push("restore")},
    PlacesCommandHook: {bookmarkTabs: tabs => calls.push(["bookmark", tabs])},
    CustomizableUI: {setToolbarVisibility: (...args) => calls.push(["toolbar", ...args])},
    BrowserUsageTelemetry: {recordToolbarVisibility() {}},
    Services: {prefs: {getBoolPref: (key, fallback) => state[key] ?? fallback,
      prefIsLocked: () => false,
      setBoolPref: (key, value) => {state[key] = value;},
      setCharPref: (...args) => calls.push(["pref", ...args])}}};
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.localization = () => ({formatMessagesSync: ([{id}]) => [{attributes: [{name: "label", value: id}]}]});
  return {pane, context, calls, state};
}
test("empty-space menu exposes only requested commands and authoritative checked states", () => {
  const f = fixture(), items = f.pane.describeTabBarMenu().items;
  assert.deepEqual(Array.from(items.filter(i => i.id), i => i.id),
    ["bar-new", "bar-reload", "bar-bookmark", "bar-select-all", "bar-restore", "bar-native-tabs", "bar-menubar"]);
  assert.equal(items.find(i => i.id === "bar-restore").enabled, false);
  assert.equal(items.find(i => i.id === "bar-menubar").checked, false);
  assert.deepEqual(Array.from(items.find(i => i.items).items, i => i.checked), [false, true, false]);
  f.state.all = true; f.context.gBrowser.selectedTabs.push({});
  const updated = f.pane.describeTabBarMenu().items;
  assert.equal(updated.find(i => i.id === "bar-select-all").enabled, false);
  assert.equal(updated.find(i => i.id === "bar-reload").label, "toolbar-context-menu-reload-selected-tabs");
});
test("empty-space commands reuse Firefox selection, history and toolbar operations", () => {
  const f = fixture(); f.state.closed = 1;
  for (const id of ["bar-new", "bar-reload", "bar-select-all", "bar-restore", "bar-bookmark", "bar-menubar", "bar-bookmarks-always"])
    f.pane.runTabBarCommand(id);
  assert.deepEqual(f.calls.slice(0, 4), ["new", "reload", "select-all", "restore"]);
  assert.equal(f.calls[4][1][0], f.context.gBrowser.selectedTabs[0]);
  assert.deepEqual(f.calls[5], ["toolbar", "toolbar-menubar", true]);
  assert.deepEqual(f.calls[6], ["pref", "browser.toolbars.bookmarks.visibility", "always"]);
  assert.deepEqual(f.calls[7], ["toolbar", "PersonalToolbar", "always"]);
  f.context.gDialogBox.isOpen = true;
  f.pane.runTabBarCommand("bar-new");
  f.pane.runTabBarCommand("bar-bookmarks-invalid");
  assert.equal(f.calls.length, 8);
});

test("tab-bar session rejects a toolbar toggle after its displayed state changed", () => {
  const f = fixture(), target = {};
  const session = new WinUITabMenuSession({resolveTab: () => target,
    describe: (_tab, surface) => {assert.equal(surface, "tab-bar"); return f.pane.describeTabBarMenu();},
    execute: (_tab, id, surface) => {assert.equal(surface, "tab-bar"); f.pane.runTabBarCommand(id);}});
  session.update(new Map([[1, target]]));
  const event = {session: 1, tab: 1, revision: session.revision, surface: "tab-bar"};
  assert(session.receive({...event, kind: "open"}));
  f.state.hidden = false;
  assert(!session.receive({...event, kind: "activate", id: "bar-menubar"}));
  assert.equal(f.calls.length, 0);
  event.session++;
  assert(session.receive({...event, kind: "open"}));
  assert(session.receive({...event, kind: "activate", id: "bar-menubar"}));
  assert.deepEqual(f.calls, [["toolbar", "toolbar-menubar", false]]);
});

test("a stale native replacement toggle cannot undo another window's choice", () => {
  const f = fixture(), target = {};
  const session = new WinUITabMenuSession({resolveTab: () => target,
    describe: () => f.pane.describeTabBarMenu(),
    execute: (_tab, id) => f.pane.runTabBarCommand(id)});
  session.update(new Map([[1, target]]));
  const event = {session: 1, tab: 1, revision: session.revision, surface: "tab-bar"};
  assert(session.receive({...event, kind: "open"}));
  f.state[f.pane.replaceTabsPref] = true;
  assert(!session.receive({...event, kind: "activate", id: "bar-native-tabs"}));
  assert.equal(f.state[f.pane.replaceTabsPref], true);
  event.session++;
  assert(session.receive({...event, kind: "open"}));
  assert(session.receive({...event, kind: "activate", id: "bar-native-tabs"}));
  assert.equal(f.state[f.pane.replaceTabsPref], false);
});
