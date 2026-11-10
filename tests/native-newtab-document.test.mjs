import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture(parent=true) {
  const data=new Map(),pref=new Map([["fenn.winui.newtab.enabled",true]]),contexts=new Map();
  const sharedData={set:(k,v)=>data.set(k,v),get:k=>data.get(k),flush(){}};
  const context=vm.createContext({Services:{appinfo:{processType:parent?0:1,PROCESS_TYPE_DEFAULT:0},
    ppmm:{sharedData},cpmm:{sharedData},prefs:{getBoolPref:(k,d)=>pref.get(k)??d},
    io:{newURI:spec=>({spec}),newChannelFromURIWithLoadInfo:(uri,loadInfo)=>({URI:uri,loadInfo})}},
    BrowsingContext:{get:id=>contexts.get(id)}});
  vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUINewTabDocument.sys.mjs",import.meta.url),"utf8")
    .replace(/^#filter substitution\r?\n/,"")
    .replaceAll("export function ","function ").replaceAll("export let ","let "),context);
  contexts.set(10,{top:{browserId:20}});
  const window={gBrowser:{browsers:[{browsingContext:{id:20}}]}};
  const uri={spec:"about:newtab"},loadInfo={targetBrowsingContextID:20,originAttributes:{privateBrowsingId:0}};
  return {context,data,pref,window,uri,loadInfo};
}
test("minimal document requires explicit live native authorization and retains original URI",()=>{
  const f=fixture();assert.equal(f.context.nativeNewTabChannel(f.uri,f.loadInfo),null);
  f.context.authorizeNewTabWindow(f.window,true);
  const channel=f.context.nativeNewTabChannel(f.uri,f.loadInfo);
  assert.equal(channel.URI.spec,"chrome://browser/content/winui-newtab.html");
  assert.equal(channel.originalURI,f.uri);assert.equal(channel.loadInfo,f.loadInfo);
  f.context.authorizeNewTabWindow(f.window,false);
  assert.equal(f.context.nativeNewTabChannel(f.uri,f.loadInfo),null);
});
test("private, sub-URLs, disabled preferences and unknown contexts retain stock channels",()=>{
  for(const change of [f=>f.loadInfo.originAttributes.privateBrowsingId=1,
    f=>f.uri.spec="about:newtab?custom",f=>f.uri.spec="about:home",
    f=>f.loadInfo.targetBrowsingContextID=0,f=>f.pref.set("fenn.winui.newtab.enabled",false),
    f=>f.pref.set("browser.newtabpage.enabled",false)]) {
    const f=fixture();f.context.authorizeNewTabWindow(f.window,true);change(f);
    assert.equal(f.context.nativeNewTabChannel(f.uri,f.loadInfo),null);
  }
});
test("child fallback bypasses native authorization for exactly one reload",()=>{
  const f=fixture(false);f.data.set("fenn/native-newtab-contexts",[20]);
  f.context.useStockNewTabOnce(20);
  assert.equal(f.context.nativeNewTabChannel(f.uri,f.loadInfo),null);
  assert.ok(f.context.nativeNewTabChannel(f.uri,f.loadInfo));
});
