import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console,UrlbarShared:{RESULT_TYPE:{SEARCH:2,URL:3,TAB_SWITCH:4,DYNAMIC:6,RESTRICT:9}}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);
function fixture(type=2,providerName='UrlbarProviderTokenAliasEngines') {
 const s=Object.create(context.Session.prototype),row={id:'r'};
 const result={type,providerName,payload:{providesSearchMode:true,query:''}};
 const browser={currentURI:{spec:'page'},browserId:7,browsingContext:{currentWindowGlobal:{innerWindowId:4}}};
 let picked=0;
 Object.assign(s,{sessionId:'s',_sequence:0,_revision:2,editId:5,modeApplied:0,nativeVisible:['r'],cancelImages(){},
  window:{gBrowser:{selectedBrowser:browser},KeyboardEvent:class{}},
  input:{inputField:{value:'@'},view:{},resultPresentation:{searchModeTitle:async()=> 'Firefox mode label'},
   controller:{parentController:{onBeforeSelection(){},onSelection(){}}},
   setValueFromResult({result:chosen}){assert.equal(s.nativeSelected,chosen);this.inputField.value='';},
   pickResult(){picked++;this.inputField.value='';}},
  active:{revision:2,rows:[row],results:[result],resultIndexes:[0],context:{id:8,results:[result]},inputValue:'@',text:'@',browser,page:'page'}});
 s.input.controller ??= {};
 s.input.view.nativePresentation=s;
 s.nativeOpen=true;
 const request={version:1,session:'s',revision:2,id:'r'};
 return {s,row,result,browser,request,get picked(){return picked;}};
}
test('only token-engine and supported restrict providers get native mode admission',()=>{
 for(const [type,name] of [[2,'UrlbarProviderTokenAliasEngines'],[9,'UrlbarProviderRestrictKeywords'],[9,'UrlbarProviderRestrictKeywordsAutofill']]) {
  const f=fixture(type,name);assert.equal(f.s.canActivate(f.result),true);
 }
 for(const [type,name] of [[2,'UrlbarProviderTabToSearch'],[9,'Extension'],[6,'UrlbarProviderTokenAliasEngines']]) {
  const f=fixture(type,name);assert.equal(f.s.nativeModeResult(f.result),false);
 }
});
test('Firefox receives native selection before its search-mode preview logic runs',async()=>{
 const f=fixture();await f.s.loadNativeModeResult(f.s.active,f.row,f.result);
 assert.equal(f.row.title,'Firefox mode label');assert.equal(f.s.selectResult(f.request).status,'selected');
 assert.equal(f.s.active.inputValue,'');assert.equal(f.s.modeQueryRestart.text,'');
});
test('mode activation is single use and retains only its scoped Firefox replacement query',async()=>{
 const f=fixture();await f.s.loadNativeModeResult(f.s.active,f.row,f.result);
 assert.equal(f.s.activate(2,'r'),'activated');assert.equal(f.picked,1);
 assert.equal(f.s.activate(2,'r'),'stale');
 const active=f.s.active;f.s.onQueryCancelled({id:8});assert.equal(f.s.active,active);
 f.s.onQueryStarted({id:9,searchString:''});
 assert.equal(f.s.active,active);assert.equal(active.context.id,9);assert.equal(active.text,'');
 assert.equal(f.s.modeQueryRestart,null);assert.equal(active.rows.length,0);
});
test('an older edit cannot admit a replacement mode query',async()=>{
 const f=fixture();await f.s.loadNativeModeResult(f.s.active,f.row,f.result);
 f.s.activate(2,'r');f.s.editId++;f.s.onQueryStarted({id:9,searchString:''});assert.equal(f.s.active,null);
});
test('stale mode labels and hidden/document-replaced activations are rejected',async()=>{
 for(const change of [f=>f.s.active={},f=>f.browser.currentURI.spec='new',f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++]) {
  const f=fixture();let resolve;f.s.input.resultPresentation.searchModeTitle=()=>new Promise(r=>resolve=r);
  const pending=f.s.loadNativeModeResult(f.s.active,f.row,f.result);change(f);resolve('Firefox');await pending;
  assert.equal(f.row.modeDocument,undefined);
 }
 for(const change of [f=>f.s.nativeVisible=[],f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.s.input.readOnly=true]) {
  const f=fixture();await f.s.loadNativeModeResult(f.s.active,f.row,f.result);change(f);
  assert.equal(f.s.activate(2,'r'),'stale');assert.equal(f.picked,0);
 }
});

test('dismissal clears only previewed modes',()=>{
 for(const isPreview of [true,false]) {
  const f=fixture();f.s.input.searchMode={isPreview,engineName:'engine'};
  f.s.suspendNativePresentation();
  assert.equal(f.s.nativeOpen,false);
  assert.equal(f.s.input.searchMode===null,isPreview);
 }
});
