import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIAddressToolbar.sys.mjs", import.meta.url), "utf8")
  .replace("export class WinUIAddressToolbar", "this.WinUIAddressToolbar = class WinUIAddressToolbar");

function fixture() {
  const calls = [];
  const listeners = new Map();
  function element() {
    const attributes = new Map();
    return {
      attributes, setAttribute(k,v) { attributes.set(k,v); calls.push(["attribute",k,v]); },
      removeAttribute(k) { attributes.delete(k); }, getAttribute(k) { return attributes.get(k); },
      append() {}, remove() {}, focus() {calls.push(["proxy-focus"]);}, addEventListener(k,fn) { listeners.set(k,fn); }, removeEventListener() {},
      getBoundingClientRect() { return {x:100,y:40,width:600,height:32}; },
    };
  }
  const slot = element(), input = element(), switcher = element(), root = element();
  input.closest = () => slot;
  input.setAttribute("placeholder", "Localized address");
  const prefs = new Map([["widget.windows.winui-prototype.address.toolbar.enabled",true]]);
  const owner = {attached:true,tabStrip:true,schedule() {},invalidateAddress() {},
    addressSession:{releaseNativePresentation() {calls.push(["release"]);}},
    addressFallback() {calls.push(["fallback"]); toolbar.restore();}};
  const window = {document:{documentElement:root,createElementNS:element,getElementById:()=>null},
    gURLBar:{inputField:input,querySelector:()=>switcher,focus() {calls.push(["firefox-focus"]);}},
    gBrowser:{addTabsProgressListener() {},removeTabsProgressListener() {}},
    windowUtils:{updateWinUIAddress(...args) {calls.push(["host",...args]); return window.acceptHost;}},
    acceptHost:true,requestAnimationFrame(callback) {callback(); return 0;},cancelAnimationFrame() {},
    PrivateBrowsingUtils:{isWindowPrivate:()=>false},ResizeObserver:class {observe() {} disconnect() {}}};
  root.hasAttribute = k => root.attributes.has(k);
  const context = vm.createContext({Services:{prefs:{getBoolPref:(k,fallback)=>prefs.get(k) ?? fallback}},console});
  vm.runInContext(source, context);
  const toolbar = new context.WinUIAddressToolbar(window, owner);
  calls.length = 0;
  return {toolbar,window,owner,prefs,calls,listeners,input,root,switcher};
}

test("replacement starts only after host and publication acceptance", () => {
  const f=fixture();
  assert.equal(f.toolbar.update(),true);
  assert.equal(f.toolbar.active,undefined);
  assert.equal(f.input.attributes.has("aria-hidden"),false);
  f.toolbar.activate();
  assert.equal(f.input.attributes.get("aria-hidden"),"true");
  assert.equal(f.switcher.inert,true,"the replaced search button must leave the tab order");
  const count=f.calls.length;
  f.toolbar.activate();
  assert.equal(f.calls.length,count,"unchanged activation must not schedule another mutation");
});

test("a cancelled startup frame cannot hide the restored Firefox editor", () => {
  const f=fixture(); let frame, cancelled;
  f.window.requestAnimationFrame = callback => {frame=callback; return 17;};
  f.window.cancelAnimationFrame = id => {cancelled=id;};
  f.toolbar.update(); f.toolbar.activate();
  assert.equal(f.input.attributes.has("aria-hidden"),false);
  f.prefs.set("widget.windows.winui-prototype.address.toolbar.enabled",false);
  f.toolbar.restore();
  assert.equal(cancelled,17);
  frame();
  assert.equal(f.toolbar.active,false);
  assert.equal(f.input.attributes.has("aria-hidden"),false);
  assert.equal(f.owner.tabStrip,true);
});

test("host refusal preserves original editable presentation", () => {
  const f=fixture(); f.window.acceptHost=false;
  assert.equal(f.toolbar.update(),false);
  assert.equal(f.toolbar.failed,true);
  assert.equal(f.input.attributes.has("aria-hidden"),false);
  assert.equal(f.toolbar.active,false);
});

test("layout and preference fallback detach only the address host", () => {
  for (const unsupported of [f=>f.window.fullScreen=true,
    f=>f.root.setAttribute("customizing",""),
    f=>f.window.PrivateBrowsingUtils.isWindowPrivate=()=>true,
    f=>f.prefs.set("sidebar.verticalTabs",true),
    f=>f.prefs.set("widget.windows.winui-prototype.address.toolbar.enabled",false)]) {
    const f=fixture(); f.toolbar.update(); f.toolbar.activate(); unsupported(f);
    assert.equal(f.toolbar.update(),false);
    assert.equal(f.owner.attached,true);
    assert.equal(f.owner.tabStrip,true);
    assert.equal(f.input.attributes.has("aria-hidden"),false);
    assert.equal(f.switcher.inert,false);
    assert.ok(f.calls.some(c=>c[0]==="host" && c[5]===false));
  }
});

test("failed focus hands the same interaction to Firefox", () => {
  const f=fixture(); f.toolbar.update(); f.toolbar.activate(); f.window.acceptHost=false;
  let prevented=false;
  f.listeners.get("beforeselect")({type:"beforeselect",preventDefault() {prevented=true;}});
  assert.equal(prevented,false);
  assert.equal(f.toolbar.active,false);
  assert.ok(f.calls.some(c=>c[0]==="fallback"));
});

test("an undersized toolbar keeps the original field", () => {
  const f=fixture();
  f.toolbar.slot.getBoundingClientRect = () => ({x:100,y:40,width:600,height:24});
  assert.equal(f.toolbar.update(),false);
  assert.equal(f.input.attributes.has("aria-hidden"),false);
});

test("a restored Firefox toolbar can fit the stock 32-DIP editor", () => {
  const f=fixture();
  f.toolbar.slot.getBoundingClientRect = () => ({x:100,y:44,width:600,height:28});
  f.window.document.getElementById = () => ({getBoundingClientRect:()=>({height:36})});
  assert.equal(f.toolbar.update(),true);
  assert.equal(f.toolbar.bounds().height,32);
  assert.equal(f.toolbar.bounds().y,42);
});

test("focus restoration is independent of suggestion visibility", () => {
  const f=fixture(); f.toolbar.update(); f.toolbar.activate();
  f.toolbar.focusOwned=true;
  f.owner.addressSession.nativeOpen=false;
  f.toolbar.restore();
  assert.ok(f.calls.some(c=>c[0]==="firefox-focus"));
});

test("restoring Gecko widget focus does not reenter an already owned native field", () => {
  const f=fixture(); f.toolbar.update(); f.toolbar.activate(); f.toolbar.focus(false);
  f.calls.length=0;
  f.listeners.get("focus")();
  assert.equal(f.calls.length,0,"the retained Gecko focus stop must not steal an outside click back into WinUI");
  f.toolbar.focusOwned=false;
  f.listeners.get("focus")();
  assert.ok(f.calls.some(c=>c[0]==="host" && c[6]===true),"keyboard entry after a real blur still enters WinUI");
});
