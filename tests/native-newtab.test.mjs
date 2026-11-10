import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture() {
  const source=readFileSync(new URL("../upstream/firefox/browser/base/content/WinUINewTabPage.sys.mjs",import.meta.url),"utf8")
    .replace(/^import .*;$/m,"")
    .replace("export class WinUINewTabPage","this.WinUINewTabPage = class WinUINewTabPage");
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
  const context=vm.createContext({authorizeNewTabWindow(){},ChromeUtils:{registerWindowActor(){},importESModule:()=>({AboutNewTab:about,BrowserUtils:{whereToOpenLink:e=>e.ctrlKey?"tab":"current"},NewTabPagePreloading:{removePreloadedBrowser(){}}})},
    Services:{prefs:{getBoolPref:(key,fallback)=>prefs.get(key)??fallback,addObserver(){},removeObserver(){}},
      obs:{addObserver(){},removeObserver(){}}},Ci:{nsIWebProgressListener:loadFlags}});
  vm.runInContext(source,context);
  const model=new context.WinUINewTabPage(window,owner);model.loadImage=()=>{};
  const action=id=>({version:1,command:"newtab-shortcut",id,session:"session-1",modifiers:0});
  return {model,window,browser,about,owner,prefs,attributes,opened,action,loadFlags};
}
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
