import assert from "node:assert/strict";
import {test} from "node:test";
import {WinUIProductMenu, PRODUCT_MENU_PREF, PRODUCT_MENU_LIMITS} from "../upstream/firefox/browser/base/content/WinUIProductMenu.sys.mjs";

const LABELS = {
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
    ["history", "submenu"], ["downloads", "item"], ["bookmarks", "submenu"], ["settings", "item"],
    ["customize-toolbar", "item"], ["check-updates", "item"], ["close-window", "item"], ["quit", "item"]]);
  const bookmarks = model.items.find(item => item.id === "bookmarks");
  const toolbar = bookmarks.items.find(item => item.id === "bookmarks-toolbar");
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
