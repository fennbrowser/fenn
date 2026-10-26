import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";
import {runInNewContext} from "node:vm";
import {WinUITabMenuSession} from "../upstream/firefox/browser/base/content/WinUITabMenuSession.sys.mjs";

const source=readFileSync(new URL("../upstream/firefox/browser/base/content/browser-winui-pane.js",import.meta.url),"utf8");
function fixture(rejectFull=false) {
  const tabs=[{},{}], ids=[1,2], messages=[];
  let selected=1,fail=true;
  const context={AppConstants:{platform:"win"},window:{addEventListener(){},windowUtils:{syncWinUITabMenu(text){
    const value=JSON.parse(text);messages.push(value);
    if ((value.state.base && fail) || (rejectFull && !value.state.base)) {fail=false;throw Error("injected refusal");}
  }}}};
  runInNewContext(source,context);
  const pane=context.gWinUIPane;
  pane.tabIds=new Map(tabs.map((tab,i)=>[tab,ids[i]]));
  pane.tabMenuSession=new WinUITabMenuSession({context:()=>({selected,selection:[selected]}),
    resolveTab:id=>tabs[id-1],describe:()=>({items:[]}),execute(){}});
  pane.tabMenuSession.update(new Map(ids.map((id,i)=>[id,tabs[i]])));
  selected=2;
  return {pane,tabs,ids,messages,select:id=>{selected=id;}};
}
test("a refused compact context receives one full snapshot and the next delta continues",()=>{
  const f=fixture();
  f.pane.publishTabContext(f.tabs,f.ids,true);
  assert.deepEqual(f.messages.map(x=>!!x.state.base),[true,false]);
  assert.equal(f.pane.publicationRecoveries,1);
  assert.deepEqual(f.messages[1].state.tabs,[{tab:1,selected:false},{tab:2,selected:true}]);
  const revision=f.messages[1].state.revision;
  f.select(1);f.pane.publishTabContext(f.tabs,f.ids,true);
  assert.equal(f.messages.length,3);
  assert.equal(f.messages[2].state.base,revision);
  assert.deepEqual(f.messages[2].state.selection,[1]);
});
test("a refused full context recovery propagates instead of retrying",()=>{
  const f=fixture(true);
  assert.throws(()=>f.pane.publishTabContext(f.tabs,f.ids,true),/injected refusal/);
  assert.equal(f.messages.length,2);
  assert.equal(f.pane.publicationRecoveries,1);
});
