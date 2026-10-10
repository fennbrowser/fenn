import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";
import {readActionStamp,actionStampMatches} from "../upstream/firefox/browser/base/content/WinUIActionStamp.sys.mjs";

const source = readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIAddressToolbar.sys.mjs", import.meta.url), "utf8")
  .replace("export class WinUIAddressToolbar", "this.WinUIAddressToolbar = class WinUIAddressToolbar");

function fixture() {
  const calls = [];
  const elements = new Map();
  const listeners = new Map();
  function element() {
    const attributes = new Map();
    return {
      attributes, setAttribute(k,v) { attributes.set(k,v); calls.push(["attribute",k,v]); },
      hasAttribute(k) { return attributes.has(k); },
      style:{},
      removeAttribute(k) { attributes.delete(k); }, getAttribute(k) { return attributes.get(k); },
      append() {}, after() {}, remove() { this.removed = true; }, focus() {calls.push(["proxy-focus"]);}, addEventListener(k,fn) { listeners.set(k,fn); }, removeEventListener() {},
      getBoundingClientRect() { return {x:100,y:40,width:600,height:32}; },
    };
  }
  const slot = element(), input = element(), switcher = element(), root = element();
  input.closest = () => slot;
  input.setAttribute("placeholder", "Localized address");
  const prefs = new Map([["fenn.winui.address.toolbar.enabled",true]]);
  const owner = {attached:true,element:element(),schedule() {},invalidateAddress() {},
    addressSession:{releaseNativePresentation() {calls.push(["release"]);}},
    addressFallback() {calls.push(["fallback"]); toolbar.restore();}};
  const window = {document:{documentElement:root,querySelectorAll:()=>[...elements.values()],createElementNS:element,getElementById:id=>elements.get(id)},
    gURLBar:{inputField:input,querySelector:()=>switcher,focus() {calls.push(["firefox-focus"]);}},
    gBrowser:{addTabsProgressListener() {},removeTabsProgressListener() {}},
    windowUtils:{updateWinUIAddress(...args) {calls.push(["host",...args]); return window.acceptHost;}},
    acceptHost:true,requestAnimationFrame(callback) {callback(); return 0;},cancelAnimationFrame() {},
    PrivateBrowsingUtils:{isWindowPrivate:()=>false},ResizeObserver:class {constructor(callback) {this.callback=callback;} observe() {} disconnect() {}},
    MutationObserver:class {observe() {} disconnect() {}},
    MouseEvent:class {constructor(type, args) {this.type=type;Object.assign(this,args);}}};
  root.hasAttribute = k => root.attributes.has(k);
  const context = vm.createContext({Services:{prefs:{getBoolPref:(k,fallback)=>prefs.get(k) ?? fallback,addObserver(){},removeObserver(){}}},console});
  Object.assign(context,{readActionStamp,actionStampMatches});
  vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIHistoryMenu.sys.mjs",import.meta.url),"utf8").replace(/^import .*;$/m,"").replace("export class WinUIHistoryMenu","this.WinUIHistoryMenu=class WinUIHistoryMenu"),context);
  context.ChromeUtils={importESModule:()=>({WinUIHistoryMenu:context.WinUIHistoryMenu})};
  vm.runInContext(source, context);
  const toolbar = new context.WinUIAddressToolbar(window, owner);
  calls.length = 0;
  return {toolbar,window,owner,prefs,calls,listeners,input,root,switcher,elements,element};
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
  f.prefs.set("fenn.winui.address.toolbar.enabled",false);
  f.toolbar.restore();
  assert.equal(cancelled,17);
  frame();
  assert.equal(f.toolbar.active,false);
  assert.equal(f.input.attributes.has("aria-hidden"),false);
  assert.equal(f.owner.attached,true);
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
    f=>f.prefs.set("fenn.winui.address.toolbar.enabled",false)]) {
    const f=fixture(); f.toolbar.update(); f.toolbar.activate(); unsupported(f);
    assert.equal(f.toolbar.update(),false);
    assert.equal(f.owner.attached,true);
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

function navigationFixture() {
  const f = fixture();
  f.prefs.set("fenn.winui.address.toolbar.enabled", false);
  f.prefs.set("fenn.winui.navigation.toolbar.enabled", true);
  f.toolbar.row.getBoundingClientRect = () => ({x:0,y:40,width:1000,height:48});
  return f;
}

test("navigation toolbar reserves a full row independently of the original input slot", () => {
  const f = navigationFixture();
  assert.equal(f.toolbar.update(), true);
  assert.equal(f.toolbar.row.hidden, false);
  assert.equal(f.toolbar.bounds().width, 1000);
  assert.equal(f.toolbar.bounds().height, 48);
  assert.equal(f.input.attributes.has("aria-hidden"), false);
  f.toolbar.activate();
  assert.equal(f.input.attributes.has("aria-hidden"), true);
  f.toolbar.close();
  assert.equal(f.toolbar.row.hidden, true);
  assert.equal(f.toolbar.row.removed, true);
  assert.equal(f.input.attributes.has("aria-hidden"), false);
});

test("navigation refusal and unsupported contexts release reserved space", () => {
  for (const reject of [f=>f.window.acceptHost=false, f=>f.window.fullScreen=true,
    f=>f.root.setAttribute("customizing", ""),
    f=>f.window.PrivateBrowsingUtils.isWindowPrivate=()=>true,
    f=>f.toolbar.row.getBoundingClientRect=()=>({x:0,y:40,width:240,height:48})]) {
    const f = navigationFixture();
    reject(f);
    assert.equal(f.toolbar.update(), false);
    assert.equal(f.toolbar.row.hidden, true);
    assert.equal(f.input.attributes.has("aria-hidden"), false);
  }
});

test("changing presentation releases old geometry and retains the strip", () => {
  const f = navigationFixture();
  f.toolbar.update(); f.toolbar.activate();
  f.prefs.set("fenn.winui.navigation.toolbar.enabled", false);
  f.prefs.set("fenn.winui.address.toolbar.enabled", true);
  assert.equal(f.toolbar.update(), true);
  assert.equal(f.toolbar.row.hidden, true);
  assert.equal(f.toolbar.bounds().width, 600);
  assert.equal(f.toolbar.active, false);
  f.toolbar.activate();
  assert.equal(f.toolbar.active, true);
  assert.equal(f.owner.attached, true);
});

function navigationCommandsFixture() {
  const f = navigationFixture();
  f.toolbar.update(); f.toolbar.activate();
  f.owner.addressSession.sessionId = "current";
  f.owner.addressTab = f.window.gBrowser.selectedTab = {};
  f.owner.addressDocument = 42;
  f.window.gBrowser.selectedBrowser = {browsingContext:{currentWindowGlobal:{innerWindowId:42}}};
  f.window.BrowserCommands = {};
  for (const item of f.toolbar.navigationCommands) {
    f.elements.set(item.command, f.element());
    const button = f.element(); button.setAttribute("label", `Localized ${item.id}`);
    f.elements.set(item.element, button);
    f.window.BrowserCommands[item.method] = event => f.calls.push(["command",item.method,event]);
  }
  f.action = {version:1,kind:"navigation",session:"current",command:"back",modifiers:0};
  f.elements.get("Browser:Stop").setAttribute("disabled", "true");
  return f;
}

test("navigation uses Firefox labels, live command eligibility and original modifier handlers", () => {
  const f = navigationCommandsFixture();
  assert.equal(f.toolbar.navigationState().back.label, "Localized back");
  f.elements.get("Browser:Back").setAttribute("disabled", "true");
  assert.equal(f.toolbar.navigationState().back.enabled, false);
  assert.equal(f.toolbar.executeNavigation(f.action), 2);
  f.elements.get("Browser:Back").removeAttribute("disabled");
  assert.equal(f.toolbar.executeNavigation(f.action), 1);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"forward",modifiers:1}), 1);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"reload",modifiers:4}), 1);
  const commands=f.calls.filter(c=>c[0]==="command");
  assert.deepEqual(commands.map(c=>c[1]),["back","forward","reloadOrDuplicate"]);
  assert.equal(commands[1][2].ctrlKey,true);
  assert.equal(commands[2][2].shiftKey,true);
});

test("navigation rejects stale tabs/documents/sessions, unavailable surfaces and malformed commands", () => {
  for (const alter of [
    f=>f.owner.addressSession.sessionId="new", f=>f.window.gBrowser.selectedTab={},
    f=>f.window.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId=43,
    f=>f.toolbar.active=false, f=>f.toolbar.navigation=false, f=>f.window.fullScreen=true,
    f=>f.action.command="stop", f=>f.action.modifiers=16, f=>f.action.version=2,
  ]) {
    const f=navigationCommandsFixture(); alter(f);
    assert.equal(f.toolbar.executeNavigation(f.action),2);
    assert.equal(f.calls.filter(c=>c[0]==="command").length,0);
  }
});

test("reload becomes localized Stop while Firefox allows stopping, and stale intents cannot reload", () => {
  const f = navigationCommandsFixture();
  assert.equal(f.toolbar.navigationState().reload.stopping, false);
  f.elements.get("Browser:Stop").removeAttribute("disabled");
  const state = f.toolbar.navigationState().reload;
  assert.equal(state.stopping, true);
  assert.equal(state.label, "Localized stop");
  assert.equal(state.enabled, true);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"reload"}), 2);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"stop"}), 1);
  assert.equal(f.calls.filter(c=>c[0]==="command").at(-1)[1], "stop");
  f.elements.get("Browser:Stop").setAttribute("disabled", "true");
  assert.equal(f.toolbar.navigationState().reload.stopping, false);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"stop"}), 2);
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"reload"}), 1);
});

test("native trailing buttons open Firefox controllers at validated screen anchors", () => {
  const f = navigationCommandsFixture();
  f.window.devicePixelRatio=2;
  f.window.mozInnerScreenX=100; f.window.mozInnerScreenY=50;
  const opened=[];
  f.window.gSync={toggleAccountPanel:(anchor,event)=>{opened.push(["profile",anchor,event]);return Promise.resolve();}};
  f.window.gUnifiedExtensions={panel:{state:"closed"},togglePanel:(event,reason,anchor)=>{opened.push(["extensions",anchor,event]);return Promise.resolve();}};
  f.window.PanelUI={panel:{state:"closed"},show:(event,anchor)=>opened.push(["appmenu",anchor,event])};
  for (const command of ["profile","extensions","appmenu"]) {
    const action={...f.action,command,x:800,y:200,width:64,height:64};
    assert.equal(f.toolbar.executeNavigation({...action,width:-1}),2);
    assert.equal(f.toolbar.executeNavigation({...action,x:Infinity}),2);
    assert.equal(f.toolbar.executeNavigation(action),1);
    assert.equal(opened.at(-1)[0],command);
    assert.equal(opened.at(-1)[1].style.left,"300px");
    assert.equal(opened.at(-1)[1].style.top,"50px");
    assert.equal(opened.at(-1)[1].style.width,"32px");
  }
  f.window.gDialogBox={isOpen:true};
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"appmenu",x:1,y:1,width:32,height:32}),2);
  assert.equal(opened.length,3);
});

test("Back/Forward history use the original popup with validated native anchors", () => {
  const f=navigationCommandsFixture(), opened=[];
  f.window.devicePixelRatio=1;f.window.mozInnerScreenX=0;f.window.mozInnerScreenY=0;
  f.elements.set("backForwardMenu",{openPopup(...args){opened.push(args);}});
  for(const command of ["back-history","forward-history"]){
    const action={...f.action,command,x:20,y:40,width:32,height:32};
    assert.equal(f.toolbar.executeNavigation({...action,width:0}),2);
    assert.equal(f.toolbar.executeNavigation({...action,session:"stale"}),2);
    assert.equal(f.toolbar.executeNavigation(action),1);
    assert.equal(opened.at(-1)[0].style.left,"20px");
  }
  assert.equal(opened.length,2);
  assert.equal(f.calls.filter(c=>c[0]==="command").length,0,"opening history never navigates");
  f.elements.get("Browser:Back").setAttribute("disabled","true");
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"back-history",x:20,y:40,width:32,height:32}),2);
});

// A queued frame without address dirtiness cannot restore a widened toolbar.
test("address geometry changes invalidate publication", () => {
  const f=fixture(); let invalidated=0;
  f.owner.invalidateAddress=()=>invalidated++;
  f.toolbar.resize.callback();
  assert.equal(invalidated,1);
});


test("overflow requires live contents and uses a validated per-call native anchor",()=>{
 const f=navigationCommandsFixture(),opened=[];
 f.window.devicePixelRatio=1;f.window.mozInnerScreenX=0;f.window.mozInnerScreenY=0;
 f.elements.set('widget-overflow-fixed-list',{children:[{hidden:false}]});
 const panel={state:'closed'};f.elements.set('widget-overflow',panel);
 f.elements.set('nav-bar',{overflowable:{show:(event,anchor)=>{opened.push(anchor);return Promise.resolve();}}});
 f.window.PanelMultiView={hidePopup:p=>{assert.equal(p,panel);panel.state='closed';}};
 const action={...f.action,command:'overflow',x:20,y:40,width:32,height:32};
 assert.equal(f.toolbar.executeNavigation({...action,session:'old'}),2);
 assert.equal(f.toolbar.executeNavigation({...action,width:0}),2);
 assert.equal(f.toolbar.executeNavigation(action),1);assert.equal(opened[0].style.left,'20px');
 panel.state='open';assert.equal(f.toolbar.executeNavigation(action),1);assert.equal(panel.state,'closed');assert.equal(opened.length,1);
 f.elements.get('widget-overflow-fixed-list').children=[];
 assert.equal(f.toolbar.executeNavigation(action),2);
});


test("Home preserves Firefox modifier and middle-button semantics",()=>{
 const f=navigationCommandsFixture();
 for(const [command,modifiers] of [["home",0],["home",4],["home-middle",1]]) assert.equal(f.toolbar.executeNavigation({...f.action,command,modifiers}),1);
 const events=f.calls.filter(c=>c[0]==="command").map(c=>c[2]);
 assert.equal(events[1].shiftKey,true);assert.equal(events[2].button,1);assert.equal(events[2].ctrlKey,true);
});
test("Downloads mirrors Firefox hidden/progress/attention state and rejects hidden activation",()=>{
 const f=navigationCommandsFixture(),button=f.elements.get("downloads-button");
 button.hidden=true;assert.equal(f.toolbar.navigationState().downloads.shown,false);
 assert.equal(f.toolbar.executeNavigation({...f.action,command:"downloads"}),2);
 button.hidden=false;button.setAttribute("progress","true");button.setAttribute("attention","warning");
 f.window.DownloadsIndicatorView={_percentComplete:37};
 const state=f.toolbar.navigationState().downloads;
 assert.equal(state.shown,true);assert.equal(state.progress,37);assert.equal(state.attention,"warning");
 button.removeAttribute("progress");assert.equal(f.toolbar.navigationState().downloads.progress,-1);
});

test("deferred Downloads anchors reject changed documents, sessions and hidden buttons",()=>{
 for(const change of [f=>f.window.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.owner.addressSession={},f=>f.elements.get("downloads-button").hidden=true,f=>f.toolbar.active=false]) {
  const f=navigationCommandsFixture();let resolve;
  f.window.devicePixelRatio=1;f.window.mozInnerScreenX=0;f.window.mozInnerScreenY=0;
  f.window.DownloadsPanel={showPanel(manual,keyboard,callback){assert.equal(manual,true);assert.equal(keyboard,true);resolve=callback;}};
  assert.equal(f.toolbar.executeNavigation({...f.action,command:"downloads",keyboard:true,x:20,y:40,width:32,height:32}),1);
  assert.ok(resolve());change(f);assert.equal(resolve(),null);
 }
});

for (const command of ["history","bookmarks"]) test(`${command} uses Firefox's original menu with an activation-scoped anchor`, () => {
 const f=navigationCommandsFixture();let opened;
 f.window.devicePixelRatio=1;f.window.mozInnerScreenX=0;f.window.mozInnerScreenY=0;
 f.window.KeyboardEvent=f.window.MouseEvent;
 f.window.PanelUI={showSubView:(...args)=>opened=args};
 const popup={state:"closed",getAttribute:()=>"bottomright topright",openPopup:(...args)=>opened=args};f.elements.set("BMB_bookmarksPopup",popup);
 const action={...f.action,command,keyboard:true,x:20,y:40,width:32,height:32};
 assert.equal(f.toolbar.executeNavigation(action),1);
 const anchor=f.toolbar.menuAnchors.get(command);
 assert.equal(opened[command==="history"?1:0],anchor);
 assert.equal(anchor.getAttribute("aria-label"),`Localized ${command}`);
 if(command==="history")assert.equal(opened[0],"PanelUI-history");
 if(command==="bookmarks")assert.equal(opened[1],"bottomright topright");
 assert.equal(opened.at(-1).type,"keypress");assert.equal(opened.at(-1).key,"Enter");
 let closed=false;
 if(command==="history") {anchor.open=true;f.elements.set("customizationui-widget-panel",{hidePopup:()=>closed=true});}
 else {popup.state="open";popup.hidePopup=()=>closed=true;}
 assert.equal(f.toolbar.executeNavigation(action),1);assert.equal(closed,true);
 opened=null;f.owner.addressDocument++;
 assert.equal(f.toolbar.executeNavigation(action),2);assert.equal(opened,null);
});
