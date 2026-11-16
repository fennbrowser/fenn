import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture(options={}) {
  const source=readFileSync(new URL("../upstream/firefox/browser/base/content/WinUINewTabPage.sys.mjs",import.meta.url),"utf8")
    .replace(/^import\b[^;]*;$/m,"")
    .replace("export class WinUINewTabPage","this.WinUINewTabPage = class WinUINewTabPage")
    .replace(/^export (async function|function) /gm,"$1 ");
  const attributes=new Set(),prefs=new Map([["fenn.winui.newtab.enabled",true]]),opened=[];
  const loadFlags={STATE_START:1,STATE_IS_NETWORK:2,STATE_STOP:4};
  const element={setAttribute:k=>attributes.add(k),removeAttribute:k=>attributes.delete(k)};
  const browser={currentURI:{spec:"about:newtab"},webProgress:{isLoadingDocument:false},
    browsingContext:{currentWindowGlobal:{innerWindowId:1}},closest:()=>element,getAttribute:()=>"2"};
  const about={newTabURL:"about:newtab",newTabURLOverridden:false,getTopSites:()=>[
    {url:"https://example.com/",title:"Example"},{url:"https://ad.example/",sponsored_position:1},
    {url:"javascript:alert(1)",title:"Bad"}]};
  const window={document:{documentElement:element},PrivateBrowsingUtils:{isWindowPrivate:()=>false},
    addEventListener(){},removeEventListener(){},MutationObserver:class {observe(){}disconnect(){}},
    gDialogBox:{isOpen:false},gBrowser:{selectedBrowser:browser,browsers:[browser],selectedTab:{},
      tabContainer:{addEventListener(){},removeEventListener(){}},addTabsProgressListener(){},removeTabsProgressListener(){}},
    openTrustedLinkIn:(...args)=>opened.push(args)};
  const owner={addressToolbar:{active:true},addressSession:{sessionId:"session-1"},invalidateAddress(){},
    localization:()=>({formatValueSync:id=>id})};
  const files=new Map(),timers=[];let uuid=0;
  const context=vm.createContext({authorizeNewTabWindow(){},ChromeUtils:{registerWindowActor(){},importESModule:()=>({AboutNewTab:about,BrowserUtils:{whereToOpenLink:e=>e.ctrlKey?"tab":"current"},NewTabPagePreloading:{removePreloadedBrowser(){}},
      PrivateBrowsingUtils:{permanentPrivateBrowsing:false},setTimeout:callback=>timers.push(callback)})},
    Services:{prefs:{getBoolPref:(key,fallback)=>prefs.get(key)??fallback,getIntPref:(key,fallback)=>prefs.get(key)??fallback,addObserver(){},removeObserver(){}},
      obs:{addObserver(){},removeObserver(){}},uuid:{generateUUID:()=>`{0000000${++uuid}-0000-4000-8000-000000000000}`}},
    Ci:{nsIWebProgressListener:loadFlags},console,
    ...(options.io?{IOUtils:{
      readJSON:async path=>{if(!files.has(path))throw new Error("missing");return JSON.parse(files.get(path));},
      writeJSON:async(path,data)=>{files.set(path,JSON.stringify(data));}},
      PathUtils:{profileDir:"P",join:(...parts)=>parts.join("/")}}:{})});
  vm.runInContext(source,context);
  const model=new context.WinUINewTabPage(window,owner);model.loadImage=()=>{};
  const action=id=>({version:1,command:"newtab-shortcut",id,session:"session-1",modifiers:0});
  return {model,window,browser,about,owner,prefs,attributes,opened,action,loadFlags,context,files,timers};
}

function openPending(f) {
  f.browser.currentURI.spec="about:blank";
  f.browser.browsingContext.currentWindowGlobal=null;
  f.window.gBrowser.selectedTab.linkedBrowser=f.browser;
  f.model.tabsChanged({type:"TabOpen",target:f.window.gBrowser.selectedTab,detail:{nativeNewTab:true}});
  f.model.changed({type:"TabSelect",target:f.window.gBrowser.selectedTab});
}

test("new-tab intent presents before any document exists and throughout backing-document loading",()=>{
  const f=fixture();openPending(f);
  const first=f.model.snapshot();
  assert.equal(first.enabled,true);assert.equal(first.animate,true);
  assert.equal(first.shortcuts.length,1);
  assert.equal(f.model.execute(f.action(first.shortcuts[0].id)),2);
  f.browser.webProgress.isLoadingDocument=true;
  const request={QueryInterface:()=>({originalURI:{spec:"about:newtab"}})};
  f.model.progress.onStateChange(f.browser,{isTopLevel:true},request,3);
  assert.equal(f.model.snapshot().enabled,true);
  f.browser.currentURI.spec="about:newtab";
  f.browser.browsingContext.currentWindowGlobal={innerWindowId:2};
  f.model.progress.onLocationChange(f.browser);
  const loading=f.model.snapshot();
  assert.equal(loading.enabled,true);
  assert.equal(f.model.execute(f.action(loading.shortcuts[0].id)),2);
  f.browser.webProgress.isLoadingDocument=false;
  f.model.progress.onStateChange(f.browser,{isTopLevel:true},request,6);
  const ready=f.model.snapshot();
  assert.equal(ready.enabled,true);
  assert.equal(f.model.execute(f.action(loading.shortcuts[0].id)),2);
  assert.equal(f.model.execute(f.action(ready.shortcuts[0].id)),1);
});

test("early presentation cancels on other navigation, failed load or tab closure",()=>{
  for(const cancel of [
    f=>f.model.progress.onStateChange(f.browser,{isTopLevel:true},{QueryInterface:()=>({originalURI:{spec:"https://example.com/"}})},3),
    f=>{f.browser.currentURI.spec="https://example.com/";f.model.progress.onLocationChange(f.browser);},
    f=>f.model.progress.onStateChange(f.browser,{isTopLevel:true},null,6),
    f=>f.model.tabsChanged({type:"TabClose",target:f.window.gBrowser.selectedTab}),
  ]) {
    const f=fixture();openPending(f);assert.equal(f.model.snapshot().enabled,true);
    cancel(f);assert.equal(f.model.snapshot().enabled,false);
    assert.equal(f.attributes.size,0);
  }
});

test("early presentation retains private, override, layout and selection boundaries",()=>{
  for(const change of [f=>f.window.PrivateBrowsingUtils.isWindowPrivate=()=>true,
    f=>f.about.newTabURLOverridden=true,f=>f.window.fullScreen=true,
    f=>f.window.SidebarController={isOpen:true},f=>f.owner.addressToolbar.active=false,
    f=>f.window.gBrowser.selectedTab.splitview=true,
    f=>f.window.gBrowser.selectedBrowser={...f.browser},
    f=>f.prefs.set("fenn.winui.newtab.enabled",false)]) {
    const f=fixture();openPending(f);change(f);
    assert.equal(f.model.snapshot().enabled,false);
  }
  const f=fixture();f.browser.currentURI.spec="about:blank";
  f.model.tabsChanged({type:"TabOpen",target:{linkedBrowser:f.browser},detail:{}});
  assert.equal(f.model.snapshot().enabled,false);
});
test("Firefox's ordinary home and new-tab pages respect private/custom/fullscreen/loading fallback",()=>{
  for(const url of ["about:home","about:newtab"]) {
    for(const change of [f=>f.prefs.set("fenn.winui.newtab.enabled",false),f=>f.prefs.set("browser.newtabpage.enabled",false),
      f=>f.window.PrivateBrowsingUtils.isWindowPrivate=()=>true,f=>f.about.newTabURLOverridden=true,
      f=>f.about.newTabURL="moz-extension://override/",f=>f.browser.currentURI.spec="https://example.com/",
      f=>f.window.fullScreen=true,f=>f.window.gDialogBox.isOpen=true,
      f=>f.browser.webProgress.isLoadingDocument=true,f=>f.owner.addressToolbar.active=false]) {
      const f=fixture();f.browser.currentURI.spec=url;
      assert.equal(f.model.snapshot().enabled,true);change(f);
      assert.equal(f.model.snapshot().enabled,false);assert.equal(f.attributes.size,0);
    }
  }
});
test("the existing-profile startup page publishes native presentation once its load finishes",()=>{
  const f=fixture();
  f.browser.currentURI.spec="about:home";
  f.browser.webProgress.isLoadingDocument=true;
  let publications=0;
  f.owner.invalidateAddress=()=>{
    publications++;
    assert.equal(f.model.snapshot().enabled,true);
  };
  assert.equal(f.model.snapshot().enabled,false);
  f.browser.webProgress.isLoadingDocument=false;
  const {STATE_STOP,STATE_IS_NETWORK}=f.loadFlags;
  f.model.progress.onStateChange(f.browser,{isTopLevel:true},null,STATE_STOP|STATE_IS_NETWORK);
  assert.equal(publications,1);
  assert.ok(f.attributes.has("winui-native-newtab"));
});
test("shortcuts exclude sponsorship and unsafe destinations; stable snapshots retain IDs",()=>{
  const f=fixture(),first=f.model.snapshot(),second=f.model.snapshot();
  assert.equal(first.shortcuts.length,1);assert.equal(first.shortcuts[0].label,"Example");
  assert.equal(first.shortcuts[0].id,second.shortcuts[0].id);
  assert.equal("url" in first.shortcuts[0],false);
  const action=f.action(first.shortcuts[0].id);action.modifiers=1;
  assert.equal(f.model.execute(action),1);assert.equal(f.opened[0][1],"tab");
});
test("shortcut activation revalidates session, document, selection and eligibility",()=>{
  for(const change of [f=>f.owner.addressSession.sessionId="new",f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,
    f=>f.browser.currentURI.spec="https://other.example/",f=>f.model.restore(),
    f=>f.window.gBrowser.selectedBrowser={...f.browser},f=>f.model.close()]) {
    const f=fixture(),id=f.model.snapshot().shortcuts[0].id;change(f);
    assert.equal(f.model.execute(f.action(id)),2);assert.equal(f.opened.length,0);
  }
});
test("shortcut removal revokes earlier IDs and turning shortcuts off publishes none",()=>{
  const f=fixture(),id=f.model.snapshot().shortcuts[0].id;
  f.about.getTopSites=()=>[];assert.equal(f.model.snapshot().shortcuts.length,0);
  assert.equal(f.model.execute(f.action(id)),2);
  f.prefs.set("browser.newtabpage.activity-stream.feeds.topsites",false);
  assert.equal(f.model.snapshot().shortcuts.length,0);
});

test("bundled shortcut artwork replaces history misses without replacing actions",()=>{
  const f=fixture(),calls=[];
  const site={url:"https://example.com/",title:"Example",tippyTopIcon:"chrome://activity-stream/content/data/example.png"};
  f.about.getTopSites=()=>[site];f.model.loadImage=(...args)=>calls.push(args);
  const first=f.model.snapshot();
  assert.equal(calls[0][2],site.tippyTopIcon);
  assert.equal(calls[1][0],"chrome://branding/content/icon128.png");
  const pixels={width:32,height:32,pixels:"00"};f.model.images.set(site.tippyTopIcon,pixels);
  const second=f.model.snapshot();
  assert.equal(second.shortcuts[0].id,first.shortcuts[0].id);
  assert.equal(second.shortcuts[0].image,pixels);
  site.tippyTopIcon="https://unrequested.example/icon.png";
  assert.equal(f.model.snapshot().shortcuts[0].image,null);
  assert.equal(calls.at(-2)[2],"");
});


test("internal stock restoration preserves current typing once without changing navigation policy",()=>{
  for (const [change,expected] of [[()=>{},1],[f=>f.browser.currentURI.spec="https://example.com/",0],
    [f=>f.browser.isNavigating=true,0],[f=>f.browser.browsingContext.currentWindowGlobal={innerWindowId:2},0],
    [f=>f.target="https://example.com/",0]]) {
    const f=fixture();f.target="about:newtab";
    let value="latest edit",retained=0;
    Object.defineProperty(f.browser,"userTypedValue",{get:()=>value,set:v=>{value=v;retained++;}});
    f.model.stockReloads.set(f.browser,f.browser.browsingContext.currentWindowGlobal);
    change(f);
    const request={QueryInterface:()=>({originalURI:{spec:f.target}})};
    f.model.progress.onStateChange(f.browser,{isTopLevel:true},request,3);
    assert.equal(retained,expected);
    const once=retained;
    f.model.progress.onStateChange(f.browser,{isTopLevel:true},request,3);
    assert.equal(retained,once);assert.equal(value,"latest edit");
  }
});

test("startup offers the cached page only for a plain default-page launch",()=>{
  const f=fixture();const expected=initial=>f.context.startupNewTabExpected(initial);
  assert.equal(expected("about:home"),true);assert.equal(expected("about:newtab"),true);
  for(const initial of ["about:blank","about:welcome|about:home","https://example.com/",""])
    assert.equal(expected(initial),false,initial);
  for(const [key,value] of [["browser.startup.page",3],["browser.sessionstore.resume_session_once",true],
    ["fenn.winui.newtab.enabled",false],["browser.newtabpage.enabled",false],["fenn.winui.navigation.toolbar.enabled",false]]){
    const g=fixture();g.prefs.set(key,value);assert.equal(g.context.startupNewTabExpected("about:home"),false,key);
  }
  const g=fixture();g.about.newTabURLOverridden=true;assert.equal(g.context.startupNewTabExpected("about:home"),false);
});

test("the startup cache keeps destinations in Firefox's file and resolves queued shortcuts",async()=>{
  const f=fixture({io:true});
  f.files.set("P/fenn-startup-newtab-targets.json",JSON.stringify({version:1,targets:{
    abc:"https://example.com/",bad:"javascript:alert(1)","Upper":"https://example.org/"}}));
  assert.equal(f.context.offerStartupNewTab("about:home"),true);
  assert.equal(f.context.offerStartupNewTab("about:home"),false);
  assert.equal(await f.context.resolveStartupShortcut("abc"),"https://example.com/");
  assert.equal(await f.context.resolveStartupShortcut("bad"),null);
  assert.equal(await f.context.resolveStartupShortcut("Upper"),null);
  assert.equal(await f.context.resolveStartupShortcut("missing"),null);
  f.about.getTopSites=()=>[{url:"https://example.com/",title:"Example"},{url:"https://new.example/",title:"New"}];
  const live=f.model.snapshot();assert.equal(live.enabled,true);
  assert.equal(f.timers.length,1);f.timers.shift()();
  await new Promise(resolve=>setTimeout(resolve,10));
  const presentation=JSON.parse(f.files.get("P/fenn-startup-newtab.json"));
  const targets=JSON.parse(f.files.get("P/fenn-startup-newtab-targets.json")).targets;
  assert.equal(JSON.stringify(presentation).includes("https:"),false);
  assert.deepEqual(presentation.shortcuts.map(s=>s.label),["Example","New"]);
  assert.equal(presentation.shortcuts[0].id,"abc");
  assert.match(presentation.shortcuts[1].id,/^[0-9a-f]{32}$/);
  assert.deepEqual(Object.values(targets),["https://example.com/","https://new.example/"]);
  assert.equal(await f.context.resolveStartupShortcut(presentation.shortcuts[1].id),"https://new.example/");
});

test("the first startup window keeps its initial default page presented while it loads",()=>{
  const f=fixture({io:true});f.window.gBrowser.tabs=[{}];
  f.browser.currentURI.spec="about:blank";f.browser.browsingContext.currentWindowGlobal=null;
  f.context.offerStartupNewTab("about:home");
  const model=new f.context.WinUINewTabPage(f.window,f.owner);model.loadImage=()=>{};
  assert.equal(model.snapshot().enabled,true);
  const request={QueryInterface:()=>({originalURI:{spec:"about:home"}})};
  f.browser.webProgress.isLoadingDocument=true;
  model.progress.onStateChange(f.browser,{isTopLevel:true},request,3);
  f.browser.currentURI.spec="about:home";model.progress.onLocationChange(f.browser);
  assert.equal(model.snapshot().enabled,true);
  const other=new f.context.WinUINewTabPage(f.window,f.owner);
  assert.equal(other.pendingNewTabs.has(f.browser),false);
});
