import assert from "node:assert/strict";
import {test} from "node:test";
globalThis.JSWindowActorParent = class {};
globalThis.JSWindowActorChild = class {};
globalThis.ChromeUtils = {registerWindowActor(){}};
const {revealStartupPage} = await import("../upstream/firefox/browser/base/content/WinUIStartupPaint.sys.mjs");
const {WinUIStartupPaintChild} = await import("../upstream/firefox/browser/base/content/WinUIStartupPaintChild.sys.mjs");
const tick = () => new Promise(resolve => setImmediate(resolve));
function fixture(initial="about:home") {
  let painted=false, queries=0, releases=0, serial=0, progress;
  const timers=new Map(), events=new Map();
  const global={documentURI:{spec:initial},getActor:()=>({sendQuery:async()=>{++queries;return painted;}})};
  const browser={currentURI:{spec:initial},browsingContext:{currentWindowGlobal:global}};
  const target={addEventListener:(k,v)=>events.set(k,v),removeEventListener:k=>events.delete(k)};
  const window={...target,gWinUIPane:{},closed:false,document:{hidden:false},gBrowserInit:{firstContentWindowPaintPromise:Promise.resolve(),uriToLoadPromise:initial},
    gBrowser:{selectedBrowser:browser,selectedTab:{},tabContainer:target,addTabsProgressListener:p=>progress=p,removeTabsProgressListener:()=>progress=null},
    windowUtils:{syncWinUITabMenu:()=>++releases},setTimeout:(fn,ms)=>{timers.set(++serial,{fn,ms});return serial;},clearTimeout:id=>timers.delete(id)};
  return {window,browser,global,events,timers,get queries(){return queries;},get releases(){return releases;},paint:()=>painted=true,
    run:ms=>{const pair=[...timers].find(([,t])=>t.ms===ms);assert.ok(pair);timers.delete(pair[0]);pair[1].fn();},
    navigate:uri=>{browser.currentURI.spec=uri;progress?.onLocationChange(browser,{isTopLevel:true},null,{spec:uri});}};
}
test("builtin initial document waits for paint, deduplicates callers and cleans up",async()=>{
 const f=fixture();revealStartupPage(f.window);revealStartupPage(f.window);await tick();assert.equal(f.queries,1);assert.equal(f.releases,0);
 f.events.get("TabSelect")();assert.equal(f.releases,0,"same-browser selection notification keeps the gate");
 f.paint();f.run(50);await tick();assert.equal(f.releases,1);assert.equal(f.timers.size,0);assert.equal(f.events.size,0);assert.equal(f.window.gWinUIPane.startupPaintStatus.reason,"document-painted");
});
test("blank, external, restored and hidden startup keep the existing barrier",async()=>{
 for(const kind of ['about:blank','https://example.test',null,'hidden','restored']){
  const f=fixture(kind==='hidden'||kind==='restored'?'about:home':kind);
  if(kind==='hidden')f.window.document.hidden=true;
  if(kind==='restored')f.window.SessionStore={isTabRestoring:()=>true};
  revealStartupPage(f.window);await tick();assert.equal(f.releases,1);assert.equal(f.queries,0);
 }
});
test("early navigation, tab selection, closure and timeout release or retire the waiter",async()=>{
 for(const action of ['navigate','select','close','timeout']){
  const f=fixture();revealStartupPage(f.window);await tick();
  if(action==='navigate')f.navigate('https://example.test');
  if(action==='select'){f.window.gBrowser.selectedBrowser={};f.events.get('TabSelect')();}
  if(action==='close'){f.window.closed=true;f.events.get('unload')();}
  if(action==='timeout')f.run(10000);
  assert.equal(f.releases,action==='close'?0:1);assert.equal(f.timers.size,0);
 }
});
test("a reply from a replaced document cannot reveal its successor",async()=>{
 const f=fixture();let answer;f.global.getActor=()=>({sendQuery:()=>new Promise(r=>answer=r)});
 revealStartupPage(f.window);await tick();f.browser.browsingContext.currentWindowGlobal={documentURI:{spec:'about:home'},getActor:()=>({sendQuery:async()=>false})};
 answer(true);await tick();assert.equal(f.releases,0);f.run(50);await tick();assert.equal(f.releases,0);f.run(10000);
});
test("actor rejects unrelated documents and accepts contentful or intentionally empty builtin pages",()=>{
 const actor=new WinUIStartupPaintChild();let entries=[];
 actor.contentWindow={document:{documentURI:'about:home',readyState:'interactive',body:{firstElementChild:{}}},performance:{getEntriesByType:()=>entries}};
 assert.equal(actor.receiveMessage({name:'Painted'}),false);entries=[{name:'first-contentful-paint'}];assert.equal(actor.receiveMessage({name:'Painted'}),true);
 actor.contentWindow.document.documentURI='https://example.test';assert.equal(actor.receiveMessage({name:'Painted'}),false);
 actor.contentWindow.document={documentURI:'about:newtab',readyState:'complete',body:{firstElementChild:null}};entries=[];assert.equal(actor.receiveMessage({name:'Painted'}),true);
});
