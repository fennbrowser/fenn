import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

globalThis.JSWindowActorChild = class {};
let source = await readFile(new URL("../upstream/firefox/browser/components/fennadblock/FennAdblockChild.sys.mjs", import.meta.url), "utf8");
let stubbed = false;
source = source.replace(/^import .*;$/gm, () => stubbed ? "" : (stubbed = true,
  "const setTimeout=(...a)=>globalThis.fixtureTimer(...a); const clearTimeout=()=>{};" +
  "const isSupportedSelectorList=()=>true, compileStyleActions=()=>({css:'',counts:{}}), compileDomActions=()=>[];" +
  "class FennAdblockDomRunner { start() {} stop() {} }"));
const {FennAdblockChild} = await import("data:text/javascript;base64," + Buffer.from(source).toString("base64"));

async function run({elapsed=5,large=false,revoke=false,scriptable=()=>true}={}) {
  let now=0,enabled=true;
  const events=[];
  globalThis.Services={prefs:{getBoolPref:name=>name.endsWith("enabled") ? enabled : false,getIntPref:()=>100},
    cpmm:{sharedData:new Map([["FennAdblock:ResourceToken","current"]])}};
  const compiled={executeInGlobal:()=>events.push("execute")};
  globalThis.ChromeUtils={now:()=>now,fennScriptsAllowed:win=>{assert.equal(win,actor.contentWindow);return scriptable(events);},
    compileFennScript:()=>{now=elapsed;return compiled;},
    compileScript:async()=>{now=elapsed;return compiled;}};
  globalThis.fixtureTimer=(callback,delay)=>{
    if(delay===0)queueMicrotask(()=>{if(revoke)enabled=false;callback();});
    return delay+1;
  };
  const actor=new FennAdblockChild();
  actor.document={nodePrincipal:{origin:"https://example.com"},blockParsing:promise=>promise.then(()=>events.push("parser"))};
  actor.contentWindow={};
  actor.manager={innerWindowId:1,windowContext:{topWindowContext:{innerWindowId:1}}};
  actor.configureCosmetics=()=>{};
  actor.sendQuery=async()=>({token:"current",generation:1,innerWindowId:"1",topInnerWindowId:"1",origin:"https://example.com",
    resources:{generation:1,injected_script:large ? " ".repeat(140*1024) : "void 0;"}});
  await actor.run(actor.document);
  return {events,timing:actor.timing};
}

test("inline completion inside budget injects before parser release",async()=>{
  const {events,timing}=await run();
  assert.deepEqual(events,["execute","parser"]);assert.equal(timing.compileMode,"inline");assert.equal(timing.timedOut,false);
});
test("overdue synchronous compilation yields to parser even before timer callback",async()=>{
  const {events,timing}=await run({elapsed:105});
  assert.deepEqual(events,["parser","execute"]);assert.equal(timing.timedOut,true);
});
test("permission revocation during overdue yield prevents late execution",async()=>{
  const {events,timing}=await run({elapsed:105,revoke:true});
  assert.deepEqual(events,["parser"]);assert.equal(timing.stale,true);
});
test("documents that may not run script never compile or run scriptlets",async()=>{
  const {events,timing}=await run({scriptable:()=>false});
  assert.deepEqual(events,["parser"]);assert.equal(timing.injected,false);assert.equal(timing.compileMode,undefined);
});
test("script disabled after compilation prevents execution",async()=>{
  let checks=0;
  const {events,timing}=await run({scriptable:()=>++checks===1});
  assert.equal(checks,2);assert.deepEqual(events,["parser"]);assert.equal(timing.injected,false);assert.equal(timing.compileMode,"inline");
});
test("a throwing script check fails closed",async()=>{
  const {events}=await run({scriptable:()=>{throw new Error("dead window");}});
  assert.deepEqual(events,["parser"]);
});
test("null-principal documents start only with a content precursor",()=>{
  globalThis.Services={prefs:{getBoolPref:()=>true}};
  for(const [principal,expected] of [
    [{isNullPrincipal:true,precursorPrincipal:{isContentPrincipal:true}},true],
    [{isNullPrincipal:true,precursorPrincipal:null},false],
    [{isNullPrincipal:true,precursorPrincipal:{isContentPrincipal:false}},false],
    [{isContentPrincipal:false},false]]){
    const actor=new FennAdblockChild();let started=false;
    actor.run=async()=>{started=true;};
    actor.document={nodePrincipal:principal,contentType:"text/html"};
    actor.handleEvent({type:"DOMDocElementInserted"});
    assert.equal(started,expected);
  }
});
test("oversize payload keeps the asynchronous compiler",async()=>{
  const {events,timing}=await run({large:true});
  assert.deepEqual(events,["execute","parser"]);assert.equal(timing.compileMode,"async");
});

test("channel preparation is scoped to current document, principal, context and top document",()=>{
  globalThis.Ci={nsIPropertyBag2:{}};
  globalThis.Services={io:{newURI:spec=>({spec})},prefs:{getBoolPref:()=>true},cpmm:{sharedData:new Map([["FennAdblock:ResourceToken","current"]])}};
  const original={token:"current",generation:1,contextId:"7",uri:"https://example.com/page",origin:"https://example.com^userContextId=2",
    topInnerWindowId:null,resources:{generation:1,injected_script:"void 0;"}};
  let value=original;
  const actor=new FennAdblockChild();
  actor.browsingContext={id:7};actor.browsingContext.top=actor.browsingContext;
  actor.manager={isCurrentGlobal:true,innerWindowId:8,windowContext:{topWindowContext:{innerWindowId:8}}};
  actor.document={documentURI:original.uri,nodePrincipal:{origin:original.origin}};
  actor.docShell={currentDocumentChannel:{URI:{spec:original.uri,equalsExceptRef:uri=>uri.spec.split('#')[0]===original.uri},
    QueryInterface:()=>({getPropertyAsAString:()=>JSON.stringify(value)})}};
  assert.equal(actor.preparedIsCurrent(actor.channelPreparation()),true);
  actor.document.documentURI+="#anchor";
  assert.equal(actor.preparedIsCurrent(actor.channelPreparation()),true);
  for(const change of [{contextId:"9"},{origin:"https://example.com"},{uri:"https://example.com/other"},{topInnerWindowId:"9"}]){
    value={...original,...change};assert.equal(actor.channelPreparation(),null);
  }
  value={...original,token:"old"};assert.equal(actor.preparedIsCurrent(actor.channelPreparation()),false);
  value=original;actor.manager.isCurrentGlobal=false;assert.equal(actor.channelPreparation(),null);
  actor.manager.isCurrentGlobal=true;actor.browsingContext.top={id:1};assert.equal(actor.channelPreparation(),null);
  value={...original,topInnerWindowId:"8"};assert.equal(actor.preparedIsCurrent(actor.channelPreparation()),true);
});
