import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { test } from "node:test";

// Exercise the actual Firefox helpers, with browser operations recorded at the
// boundary. Physical native input and browser dialogs need integration tests.
const source = readFileSync(new URL(
  "../upstream/firefox/browser/components/tabbrowser/content/tab-context-menu.js",
  import.meta.url), "utf8");

function fixture() {
  const calls = [];
  const selectedTabs = [];
  const browser = {
    selectedTabs,
    TabMetrics: {
      METRIC_SOURCE: { TAB_MENU: "tab-menu" },
      userTriggeredContext: source => ({ source }),
    },
    removeTab: (...args) => calls.push(["close", ...args]),
    removeMultiSelectedTabs: (...args) => calls.push(["close-selected", ...args]),
    explicitUnloadTabs: tabs => { calls.push(["unload", tabs]); return "result"; },
    moveTabTo: (...args) => calls.push(["move", ...args]),
  };
  const context = vm.createContext({
    gBrowser: browser,
    gDialogBox: { isOpen: false },
    window: {},
    XPCOMUtils: { defineLazyPreferenceGetter() {} },
    ChromeUtils: { defineESModuleGetters() {} },
    Glean: { tabgroup: { tabInteractions: {
      duplicate: { add: () => calls.push(["group-metric"]) },
    } } },
    SessionStore: { duplicateTab: (_window, tab) => {
      calls.push(["duplicate", tab]);
      selectedTabs.length = 0; // Simulate synchronous selection mutation.
      return { original: tab };
    } },
  });
  vm.runInContext(source, context);
  return { menu: context.TabContextMenu, browser, calls, selectedTabs, context };
}

test("background target and multiselection resolve without changing DOM context", () => {
  const f = fixture();
  const active = { multiselected: true };
  const other = { multiselected: true };
  const background = { multiselected: false };
  f.menu.contextTab = active;
  f.selectedTabs.push(active, other);
  assert.deepEqual(Array.from(f.menu.getCommandTabs(background)), [background]);
  const targets = f.menu.getCommandTabs(other);
  f.selectedTabs.length = 0;
  assert.deepEqual(Array.from(targets), [active, other]);
  assert.equal(f.menu.contextTab, active);
});

test("explicit close preserves background target, selected close, and metrics", () => {
  const f = fixture();
  const domTarget = {};
  const target = { multiselected: false };
  f.menu.contextTab = domTarget;
  f.menu.closeContextTabs(target);
  assert.equal(f.calls[0][0], "close");
  assert.equal(f.calls[0][1], target);
  assert.equal(f.calls[0][2].animate, true);
  assert.equal(f.calls[0][2].metricsContext.source, "tab-menu");
  target.multiselected = true;
  f.menu.closeContextTabs(target);
  assert.equal(f.calls[1][0], "close-selected");
  assert.equal(f.menu.contextTab, domTarget);
  f.menu.closeContextTabs();
  assert.equal(f.calls[2][1], domTarget);
});

test("duplicate snapshots targets before selection mutation and retains insertion order", () => {
  const f = fixture();
  const first = { index: 2, group: {} };
  const last = { index: 4 };
  f.selectedTabs.push(first, last);
  f.menu.contextTabs = [last];
  f.menu.duplicateSelectedTabs(f.selectedTabs);
  assert.deepEqual(f.calls.filter(c => c[0] === "duplicate").map(c => c[1]), [first, last]);
  assert.deepEqual(f.calls.filter(c => c[0] === "move").map(c => c[2].tabIndex), [5, 6]);
  assert.equal(f.calls.filter(c => c[0] === "group-metric").length, 1);
  assert.deepEqual(f.menu.contextTabs, [last]);
  const count = f.calls.length;
  f.menu.duplicateSelectedTabs([]);
  assert.equal(f.calls.length, count);
  f.menu.duplicateSelectedTabs();
  assert.equal(f.calls[count][1], last);
});

test("explicit unload snapshots targets and retains default DOM calling convention", () => {
  const f = fixture();
  const target = {};
  const targets = [target];
  f.menu.contextTabs = [{}];
  assert.equal(f.menu.explicitUnloadTabs(targets), "result");
  targets.length = 0;
  assert.deepEqual(Array.from(f.calls[0][1]), [target]);
  f.menu.explicitUnloadTabs();
  assert.equal(f.calls[1][1][0], f.menu.contextTabs[0]);
});

function coreFixture() {
  const f = fixture();
  const makeTab = index => ({ index, isConnected: true, isOpen: true,
    linkedPanel: true, linkedBrowser: { isRemoteBrowser: true, currentURI: { spec: "https://example.com/" + index } },
    hasAttribute(name) { return !!this[name]; },
    toggleMuteAudio() { this.muted = !this.muted; },
  });
  const tabs = [makeTab(0), makeTab(1), makeTab(2)];
  Object.assign(f.browser, { tabs, openTabs: tabs, pinnedTabCount: 0,
    tabContainer: { findNextTab: t => tabs[t.index + 1] },
    _getTabsToTheStartFrom: t => tabs.filter(x => x.index < t.index && !x.pinned && !x.multiselected),
    _getTabsToTheEndFrom: t => tabs.filter(x => x.index > t.index && !x.pinned && !x.multiselected),
    getDuplicateTabsToClose: () => [],
    allTabsSelected: () => tabs.every(t => t.multiselected),
    reloadTab: t => f.calls.push(["reload", t]),
    reloadMultiSelectedTabs: () => f.calls.push(["reload-selected"]),
  });
  f.selectedTabs.push(tabs[0]);
  f.context.SessionStore.getLastClosedTabCount = () => 0;
  f.menu._unloadTabInContextMenu = true;
  const describe = (tab = tabs[1]) => {
    const result = new Map();
    const walk = items => items.forEach(item => {
      if (item.id) result.set(item.id, item);
      if (item.items) walk(item.items);
    });
    walk(f.menu.describeCoreCommands(tab));
    return result;
  };
  return { ...f, tabs, describe };
}

test("core availability follows boundaries, unload preference and closed history", () => {
  const f = coreFixture();
  const first = f.describe(f.tabs[0]);
  assert.equal(first.get("move-start").enabled, false);
  assert.equal(first.get("close-start").enabled, false);
  assert.equal(first.get("move-end").enabled, true);
  assert.equal(first.get("restore").enabled, false);
  assert.equal(first.get("close-duplicates").enabled, false);
  assert.equal(f.describe(f.tabs[2]).get("move-end").enabled, false);
  assert.equal(first.get("unload").tabCount, 1);
  f.menu._unloadTabInContextMenu = false;
  assert.equal(f.describe().has("unload"), false);
  f.menu._unloadTabInContextMenu = true;
  f.tabs[1].linkedPanel = false;
  assert.equal(f.describe().has("unload"), false);
  f.context.SessionStore.getLastClosedTabCount = () => 2;
  assert.equal(f.describe().get("restore").tabCount, 2);
  assert.equal(f.describe().get("restore").enabled, true);
});

test("selected count, mute state and restricted pin labels use the target's context", () => {
  const f = coreFixture();
  f.tabs[0].multiselected = f.tabs[1].multiselected = true;
  f.selectedTabs.push(f.tabs[1]);
  let description = f.describe();
  assert.equal(description.get("r").resource, "reload-tabs");
  assert.equal(description.get("close").tabCount, 2);
  assert.equal(description.get("bookmark").resource, "bookmark-selected-tabs");
  assert.equal(f.describe(f.tabs[2]).get("close").tabCount, 1);
  f.tabs[1].muted = true;
  assert.equal(f.describe().get("mute").resource, "tabbrowser-context-unmute-selected-tabs");
  f.tabs[0].linkedBrowser.currentURI.spec = "about:opentabs";
  assert.equal(f.describe().has("p"), false);
  f.tabs[1].pinned = true;
  assert.equal(f.describe().get("u").resource, "unpin-selected-tabs");
});

test("core execution rechecks live state and preserves Firefox target operations", () => {
  const f = coreFixture();
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "mute"), true);
  assert.equal(f.tabs[1].muted, true);
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "r"), true);
  assert.equal(f.calls.at(-1)[1], f.tabs[1]);
  f.tabs[1].multiselected = true;
  f.selectedTabs.push(f.tabs[1]);
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "r"), true);
  assert.equal(f.calls.at(-1)[0], "reload-selected");
  const count = f.calls.length;
  f.tabs[0]._pendingPermitUnload = true;
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "close"), false);
  assert.equal(f.calls.length, count);
  f.tabs[0]._pendingPermitUnload = false;
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "not-a-command"), false);
  f.tabs[1].isConnected = false;
  assert.equal(f.menu.executeCoreCommand(f.tabs[1], "close"), false);
});

test("every core action forwards to the corresponding Firefox operation", () => {
  const operations = {
    new: "addAdjacentNewTab", p: "pinTab", u: "unpinTab",
    "select-all": "selectAllTabs", "close-duplicates": "removeDuplicateTabs",
    "close-start": "removeTabsToTheStartFrom", "close-end": "removeTabsToTheEndFrom",
    "close-others": "removeAllTabsBut", "move-start": "moveTabsToStart",
    "move-end": "moveTabsToEnd", "move-window": "replaceTabsWithWindow",
  };
  for (const [id, operation] of Object.entries(operations)) {
    const f = coreFixture();
    if (id === "u") f.tabs[1].pinned = true;
    f.browser.getDuplicateTabsToClose = () => [f.tabs[2]];
    f.browser[operation] = (...args) => f.calls.push([operation, ...args]);
    f.context.window.focus = () => {};
    assert(f.menu.executeCoreCommand(f.tabs[1], id), id);
    assert.equal(f.calls.at(-1)[0], operation, id);
    if (id !== "select-all") assert.equal(f.calls.at(-1)[1], f.tabs[1], id);
  }
  const f = coreFixture();
  f.context.PlacesCommandHook = { bookmarkTabs: tabs => f.calls.push(["bookmark", tabs]) };
  f.context.SessionWindowUI = { undoCloseTab: win => f.calls.push(["restore", win]) };
  f.context.SessionStore.getLastClosedTabCount = () => 1;
  assert(f.menu.executeCoreCommand(f.tabs[1], "bookmark"));
  assert.equal(f.calls.at(-1)[1][0], f.tabs[1]);
  assert(f.menu.executeCoreCommand(f.tabs[1], "restore"));
  assert.equal(f.calls.at(-1)[0], "restore");
  assert(f.menu.executeCoreCommand(f.tabs[1], "unload"));
  assert.equal(f.calls.at(-1)[0], "unload");
  assert.equal(f.calls.at(-1)[1][0], f.tabs[1]);
});
