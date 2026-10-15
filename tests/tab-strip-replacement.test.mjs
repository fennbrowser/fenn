import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";
import {runInNewContext} from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
function fixture() {
  const prefs = new Map(), observers = new Map(), posted = [], attributes = new Map();
  const element = () => ({style: {}, attributes: new Map(), listeners: new Map(),
    setAttribute(name, value) {this.attributes.set(name, String(value));},
    addEventListener(name, listener) {this.listeners.set(name, listener);},
    removeEventListener(name) {this.listeners.delete(name);},
    appendChild() {}, remove() {},
    getBoundingClientRect() {return {x: 0, y: 0, width: 900, height: 40};}});
  const root = {...element(), hasAttribute: name => attributes.has(name),
    toggleAttribute(name, active) {if (active) attributes.set(name, ""); else attributes.delete(name);}};
  const menu = element(), state = {attached: true, revision: 1, locked: false, published: 0};
  const context = {AppConstants: {platform: "win"}, gDialogBox: {isOpen: false},
    window: {toolbar: {visible: true}, addEventListener() {}, windowUtils: {
      updateWinUIPane: () => state.attached}},
    getComputedStyle: () => ({display: "flex"}),
    document: {documentElement: root, getElementById: () => menu,
      createElementNS: element, createXULElement: element},
    Services: {tm: {dispatchToMainThread: callback => posted.push(callback)}, prefs: {
      getBoolPref: (name, fallback) => prefs.get(name) ?? fallback,
      prefIsLocked: () => state.locked,
      setBoolPref(name, value) {prefs.set(name, value); observers.get(name)?.observe();},
      addObserver(name, observer) {observers.set(name, observer);},
      removeObserver(name) {observers.delete(name);}}}, console: {error() {}}};
  runInNewContext(source, context);
  const pane = context.gWinUIPane;
  pane.localization = () => ({formatMessagesSync: () => [{attributes: [{name: "label", value: "Localized native tabs"}]}]});
  pane.element = element();
  pane.occlusions = () => [];
  pane.publishState = () => {state.published++; pane.nativeRevision = state.revision;};
  pane.uninit = () => {pane.setReplacementActive(false); pane.element = null; pane.attached = false; pane.nativeRevision = 0;};
  prefs.set(pane.replaceTabsPref, false); // the shipped default is on; these tests start from the opt-out
  return {pane, context, prefs, observers, posted, root, menu, state};
}

test("replacement hides Firefox tabs only after attachment and a tab publication", () => {
  const f = fixture();
  f.pane.update();
  assert.equal(f.root.hasAttribute("winui-replaces-tabs"), false);
  f.prefs.set(f.pane.replaceTabsPref, true);
  f.state.revision = 0;
  f.pane.update();
  assert.equal(f.root.hasAttribute("winui-replaces-tabs"), false);
  f.state.revision = 4;
  f.pane.update();
  assert.equal(f.root.hasAttribute("winui-replaces-tabs"), true);
  assert.equal(f.state.published, 3);
});

for (const failure of ["attachment", "publication"]) {
  test(`${failure} failure restores Firefox tabs and retires the native reservation`, () => {
    const f = fixture();
    f.prefs.set(f.pane.replaceTabsPref, true);
    f.pane.update();
    assert(f.root.hasAttribute("winui-replaces-tabs"));
    if (failure === "attachment") f.state.attached = false;
    else f.pane.publishState = () => {throw new Error("controlled publication failure");};
    f.pane.update();
    assert.equal(f.root.hasAttribute("winui-replaces-tabs"), false);
    assert.equal(f.pane.element, null);
    assert.equal(f.prefs.get(f.pane.replaceTabsPref), true);
  });
}

for (const mode of ["fullscreen", "customization", "vertical-tabs"]) {
  test(`${mode} uses Firefox presentation and returning resumes replacement`, () => {
    const f = fixture();
    f.prefs.set(f.pane.replaceTabsPref, true);
    f.pane.update();
    const change = value => {
      if (mode === "fullscreen") f.context.window.fullScreen = value;
      if (mode === "customization") f.root.toggleAttribute("customizing", value);
      if (mode === "vertical-tabs") f.prefs.set("sidebar.verticalTabs", value);
    };
    change(true);
    f.pane.update();
    assert.equal(f.root.hasAttribute("winui-replaces-tabs"), false);
    assert.equal(f.pane.element.style.display, "none");
    change(false);
    f.pane.update();
    assert(f.root.hasAttribute("winui-replaces-tabs"));
    assert.equal(f.pane.element.style.display, "");
  });
}

test("the option survives disabled native UI and schedules reinitialization outside its command stack", () => {
  const f = fixture();
  f.pane.element = null;
  f.pane.initOptions();
  assert(f.observers.has(f.pane.replaceTabsPref));
  assert.equal(f.menu.listeners.has("popupshowing"), false, "no toolbar-context item is installed");
  let initialized = 0;
  f.pane.init = () => initialized++;
  f.pane.toggleReplacement();
  assert.equal(f.prefs.get(f.pane.replaceTabsPref), true);
  assert.equal(initialized, 0);
  assert.equal(f.posted.length, 1);
  f.posted.shift()();
  assert.equal(initialized, 1);
  f.state.locked = true;
  f.pane.toggleReplacement();
  assert.equal(f.prefs.get(f.pane.replaceTabsPref), true);
  f.state.locked = false;
  f.pane.toggleReplacement();
  f.pane.uninitOptions();
  f.posted.shift()();
  assert.equal(initialized, 1);
  assert.equal(f.observers.size, 0);
});

test("visible custom Firefox tab-toolbar controls remain accessible through fallback", () => {
  const f = fixture();
  f.prefs.set(f.pane.replaceTabsPref, true);
  f.menu.children = [{id: "tabbrowser-tabs", localName: "tabs"},
    {id: "custom-extension", localName: "toolbarbutton"}];
  f.pane.update();
  assert.equal(f.root.hasAttribute("winui-replaces-tabs"), false);
  assert.equal(f.pane.element.style.display, "none");
  f.menu.children.pop();
  f.pane.update();
  assert(f.root.hasAttribute("winui-replaces-tabs"));
});

test("preference changes retain the live native session when the effective presentation is unchanged", () => {
  const f = fixture();
  f.prefs.set(f.pane.replaceTabsPref, true);
  f.pane.initOptions();
  const original = f.pane.element;
  let scheduled = 0;
  f.pane.schedule = () => scheduled++;
  f.context.Services.prefs.setBoolPref(f.pane.pref, false);
  f.posted.shift()();
  assert.equal(f.pane.element, original);
  assert.equal(scheduled, 1);
});

test("a preference notification delivered inside a nested native drain waits for the outer drain", () => {
  const f = fixture();
  // Replacement is the only thing enabling the pane, so clearing it tears down.
  f.prefs.set(f.pane.pref, false);
  f.pane.initOptions();
  const original = f.pane.element;
  f.pane.draining = true;
  f.context.Services.prefs.setBoolPref(f.pane.replaceTabsPref, false);
  f.posted.shift()();
  assert.equal(f.pane.element, original);
  assert.equal(f.pane.optionsRefreshDeferred, true);
  assert.equal(f.posted.length, 0);
  f.pane.draining = false;
  f.pane.attached = true;
  f.pane.init = () => {};
  f.pane.drain();
  assert.equal(f.posted.length, 1);
  f.posted.shift()();
  assert.equal(f.pane.element, null);
});

test("the retained Firefox toolbar hides only while the native navigation row is active", () => {
  const f = fixture(), {pane, root} = f;
  const hidden = () => root.hasAttribute("winui-hides-nav-bar");
  pane.attached = true;
  pane.addressToolbar = {active: true, navigation: true};
  pane.updateToolbarHidden();
  assert.equal(hidden(), true, "on by default");
  f.prefs.set(pane.hideToolbarPref, false);
  pane.updateToolbarHidden();
  assert.equal(hidden(), false, "opt-out keeps the toolbar");
  f.prefs.set(pane.hideToolbarPref, true);
  pane.updateToolbarHidden();
  assert.equal(hidden(), true);
  pane.addressToolbar.navigation = false;
  pane.updateToolbarHidden();
  assert.equal(hidden(), false, "input-slot mode keeps the toolbar");
  pane.addressToolbar.navigation = true;
  pane.updateToolbarHidden();
  root.toggleAttribute("customizing", true);
  pane.updateToolbarHidden();
  assert.equal(hidden(), false, "customization shows the toolbar");
});

test("group popups anchor to the native strip when Firefox's label has no frame", () => {
  const f = fixture(), {pane} = f;
  f.context.window.mozInnerScreenX = 100;
  f.context.window.mozInnerScreenY = 50;
  assert.deepEqual({...pane.groupMenuFallbackRect()}, {x: 100, y: 50, width: 900, height: 40});
  pane.element.style.display = "none";
  assert.equal(pane.groupMenuFallbackRect(), null);
});
