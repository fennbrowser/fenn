import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
function fixture() {
  const identities = [{userContextId: 1, name: "Personal", color: "blue"},
    {userContextId: 2, name: "Work", color: "orange"}];
  const calls = [], state = {enabled: true, private: false};
  const context = {
    window: {addEventListener() {}}, gDialogBox: {isOpen: false}, BROWSER_NEW_TAB_URL: "about:newtab",
    Services: {prefs: {getBoolPref: () => state.enabled}},
    PrivateBrowsingUtils: {isWindowPrivate: () => state.private},
    ContextualIdentityService: {getPublicIdentities: () => identities,
      getUserContextLabel: id => identities.find(i => i.userContextId === id).name,
      getContainerColorCode: color => color === "blue" ? "#37adff" : "#ff9400",
      formatContextLabel: id => id},
    TabContextMenu: {getCommandTabs: tab => tab.selected || [tab],
      reopenTabsInContainer: (...args) => calls.push(["reopen", ...args])},
    ContainerCreationPanel: {open: (...args) => calls.push(["add", ...args])},
    openPreferences: (...args) => calls.push(["manage", ...args]),
    openTrustedLinkIn: (...args) => calls.push(["new", ...args]),
  };
  runInNewContext(source, context);
  const tab = {isConnected: true, linkedBrowser: {}, userContextId: 1};
  return {pane: context.gWinUIPane, tab, identities, calls, state, context};
}
test("container menu follows public identities and excludes the target container", () => {
  const f = fixture();
  const items = f.pane.describeContainerMenu(f.tab).items;
  assert.deepEqual(Array.from(items.filter(i => i.kind === "command"), i => i.id),
    ["container:0", "container:2", "container-add", "container-manage"]);
  assert.equal(items.find(i => i.id === "container:2").color, 0xffff9400);
  assert.equal(f.pane.describeContainerMenu(null).items.filter(i => i.id?.startsWith("container:")).length, 3);
  f.tab.userContextId = 0;
  assert(!f.pane.describeContainerMenu(f.tab).items.some(i => i.id === "container:0"));
});
test("container commands preserve target sets, new-tab semantics and management entrypoints", () => {
  const f = fixture();
  f.tab.selected = [f.tab, {...f.tab, userContextId: 0}];
  f.pane.runTabCommand(f.tab, "container:2");
  assert.deepEqual(f.calls[0], ["reopen", f.tab.selected, 2]);
  f.pane.runTabCommand(null, "container:2");
  assert.equal(f.calls[1][1], "about:newtab");
  assert.equal(f.calls[1][3].userContextId, 2);
  f.pane.runTabCommand(null, "container-add");
  assert.equal(f.calls[2][2], "new_tab_button");
  f.pane.runTabCommand(f.tab, "container-manage");
  assert.equal(f.calls[3][2].urlParams.entrypoint, "tab_context_menu");
});
test("deleted, disabled, private and blocked container commands never execute", () => {
  for (const mutate of [f => f.identities.pop(), f => f.state.enabled = false,
    f => f.state.private = true, f => f.context.gDialogBox.isOpen = true,
    f => f.tab.closing = true]) {
    const f = fixture(); mutate(f);
    f.pane.runTabCommand(f.tab, "container:2");
    assert.equal(f.calls.length, 0);
  }
  const f = fixture();
  f.state.private = true;
  assert.deepEqual(Array.from(f.pane.describeContainerMenu(null).items, i => i.id), ["container:0"]);
  f.pane.runTabCommand(null, "container:999");
  assert.equal(f.calls.length, 0);
});

test("menu images are presentation-only, deduplicated and support nested container submenus", async () => {
  const f = fixture();
  f.context.ContextualIdentityService.getContainerIconURL = icon => `resource://usercontext-content/${icon}.svg`;
  const calls = [];
  f.pane.renderContainerMenuIcon = async (...args) => {
    calls.push(args);
    return {width: 1, height: 1, pixels: "0000ffff"};
  };
  const item = {kind: "command", id: "container:1", containerIcon: "fingerprint", containerColorName: "blue"};
  const source = {version: 2, menu: {session: 1, items: [item, {kind: "submenu", items: [item]}]}};
  const answer = await f.pane.prepareContainerMenu(source);
  assert.equal(answer.menu.items[0].image.pixels, "0000ffff");
  assert.equal(answer.menu.items[1].items[0].image.pixels, "0000ffff");
  assert.equal(calls.length, 1);
  assert.equal("image" in item, false, "semantic description stays unchanged while images decode");
});

test("late icon completion cannot publish into a dismissed or replaced menu", async () => {
  for (const change of [s => {s.active = null;}, s => {s.active.session = 2;}]) {
    const f = fixture(), sent = [];
    let finish;
    f.pane.prepareContainerMenu = () => new Promise(resolve => {finish = resolve;});
    const session = {active: {session: 1}, receive: () => true,
      menuPublication: () => ({menu: {session: 1}}), revalidateActive: () => false};
    f.pane.tabMenuSession = session;
    f.pane.publishMenu = value => sent.push(value);
    assert.equal(f.pane.executeRequest({kind: 2, action: {kind: "open", session: 1}}), 1);
    change(session);
    finish({menu: {session: 1}});
    await Promise.resolve();
    assert.equal(sent.length, 0);
  }
});
