import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIChromeLayout.sys.mjs", import.meta.url), "utf8")
  .replace("export class WinUIChromeLayout", "this.WinUIChromeLayout = class WinUIChromeLayout");
function fixture() {
  const attributes = new Set(["winui-replaces-tabs", "winui-hides-nav-bar"]);
  const properties = new Map([["unrelated", "keep"]]);
  const calls = [];
  const root = {hasAttribute:key=>attributes.has(key),setAttribute:key=>attributes.add(key),removeAttribute:key=>attributes.delete(key),
    style:{setProperty:(key,value)=>properties.set(key,value),removeProperty:key=>properties.delete(key)},append(){}};
  const prefs = new Map([["fenn.winui.chrome.layout",true],["height",40]]);
  const observers = new Map();
  const window = {document:{documentElement:root,createElementNS:()=>({remove(){}}),getElementById:()=>null,querySelectorAll:()=>window.bars},
    bars:[],frame:"", addEventListener(){},removeEventListener(){},
    getComputedStyle:element=>element === root ? {getPropertyValue:()=>window.frame} : {display:"block",visibility:"visible"},
    MutationObserver:class {observe(){}disconnect(){}},
    ResizeObserver:class {observe(){}disconnect(){}},
    windowUtils:{getWinUIChromeLayout(...args){calls.push(["measure",...args]);if(window.measureError)throw Error();return window.values;},
      updateWinUIPane(...args){calls.push(["strip",...args]);return !window.refuse;},
      updateWinUIAddress(...args){calls.push(["navigation",...args]);if(window.applyError)throw Error();return !window.refuse;}}};
  window.values = [0,0,1000,40,4,40,992,40,4,80,992,716];
  const owner = {attached:true,stripHeightPref:"height",addressToolbar:{active:true,navigation:true,supported:()=>!window.unsupported},
    stripVisibility:()=>({replace:true,hidden:!!window.hidden}),schedule(){calls.push(["schedule"]);},occlusions:()=>[]};
  const context = vm.createContext({Services:{prefs:{getBoolPref:(key,fallback)=>prefs.get(key)??fallback,
    getIntPref:(key,fallback)=>prefs.get(key)??fallback,addObserver:(key,value)=>observers.set(key,value),removeObserver:key=>observers.delete(key)}}});
  vm.runInContext(source, context);
  const layout = new context.WinUIChromeLayout(window, owner);
  layout.link.onload();
  calls.length = 0;
  return {layout,owner,window,root,attributes,properties,prefs,calls,observers};
}
test("native rectangles place both islands before enabling Gecko content allocation", () => {
  const f=fixture(), p=f.layout.update();
  assert.deepEqual(f.calls.map(call=>call[0]),["measure","strip","navigation"]);
  assert.equal(p.content.height,716);
  assert(f.attributes.has("winui-native-layout"));
  assert.equal(f.properties.get("--winui-layout-content-y"),"80px");
  assert.equal(f.properties.get("--winui-layout-navigation-x"),"4px");
  assert.equal(f.properties.get("unrelated"),"keep");
  f.calls.length=0;f.layout.update();
  assert.deepEqual(f.calls.map(call=>call[0]),["measure"]);
});
test("resize and fractional device conversion use native output without measuring DOM rows", () => {
  const f=fixture();f.layout.update();
  f.window.values=[0,0,1200,40.25,4,40.25,1192,39.75,4,80,1192,900.5];
  const p=f.layout.update();
  assert.equal(p.content.height,900.5);
  assert.equal(f.properties.get("--winui-layout-strip-height"),"40.25px");
  assert.equal(f.properties.get("--winui-layout-content-width"),"1192px");
});
test("fallback decisions restore flow without altering unrelated style and can recover", () => {
  for(const change of [f=>f.prefs.set("fenn.winui.chrome.layout",false),f=>f.owner.attached=false,
    f=>f.owner.addressToolbar.active=false,f=>f.owner.addressToolbar.navigation=false,
    f=>f.window.unsupported=true,f=>f.window.hidden=true,f=>f.attributes.delete("winui-hides-nav-bar"),
    f=>f.attributes.delete("winui-replaces-tabs"),f=>f.attributes.add("mini-window"),
    f=>f.window.bars=[{id:"PersonalToolbar",getBoundingClientRect:()=>({height:28})}]]) {
    const f=fixture();f.layout.update();change(f);assert.equal(f.layout.update(),null);
    assert(!f.attributes.has("winui-native-layout"));assert.equal(f.properties.size,1);
  }
  const f=fixture();f.layout.update();f.window.hidden=true;f.layout.update();f.window.hidden=false;
  assert(f.layout.update());
});
test("minimized or insufficient native space and malformed results cannot retain content bounds", () => {
  for(const values of [[],[0],Array(12).fill(NaN),Array(12).fill(0),Array(12).fill(100001)]) {
    const f=fixture();f.layout.update();f.window.values=values;
    assert.equal(f.layout.update(),null);assert(!f.attributes.has("winui-native-layout"));
  }
  const f=fixture();f.layout.update();f.window.measureError=true;assert.equal(f.layout.update(),null);
  f.window.measureError=false;assert(f.layout.update());
});
test("native refusal or exception does not commit DOM allocation or spin on retries", () => {
  for(const kind of ["refuse","applyError"]) {
    const f=fixture();f.window[kind]=true;assert.equal(f.layout.update(),null);
    assert(!f.attributes.has("winui-native-layout"));assert.equal(f.properties.size,1);
    f.window[kind]=false;f.calls.length=0;assert.equal(f.layout.update(),null);assert.equal(f.calls.length,0);
    f.observers.get("fenn.winui.chrome.layout").observe();assert(f.layout.plan);
  }
});
test("unsupported frame values fall back and unsupported row preference uses the native default", () => {
  const f=fixture();f.prefs.set("height",123);f.layout.update();assert.deepEqual(f.calls[0],["measure",40,true,4]);
  f.window.frame="3.5px";assert.equal(f.layout.update(),null);
  f.window.frame="0px";f.calls.length=0;f.layout.update();assert.deepEqual(f.calls[0],["measure",40,true,0]);
});
test("close revokes allocations, observers and late stylesheet callbacks", () => {
  const f=fixture();f.layout.update();f.layout.close();
  assert.equal(f.layout.plan,null);assert(!f.attributes.has("winui-native-layout"));
  assert.equal(f.layout.link.onload,null);assert.equal(f.observers.size,0);
  assert.equal(f.layout.update(),null);
});
test("reentrant teardown during native attachment cannot resurrect layout", () => {
  const f=fixture();
  f.window.windowUtils.updateWinUIAddress=()=>{f.layout.close();return true;};
  assert.equal(f.layout.update(),null);assert(!f.attributes.has("winui-native-layout"));
  assert.equal(f.properties.size,1);
});
