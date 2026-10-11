import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console,UrlbarShared:{RESULT_TYPE:{DYNAMIC:6,SEARCH:2,URL:3,TAB_SWITCH:4}}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);
function fixture(kind='calculator') {
 const s=Object.create(context.Session.prototype), row={id:'r'};
 const result={type:6,providerName:kind==='calculator'?'UrlbarProviderCalculator':'UrlbarProviderUnitConversion',payload:{dynamicType:kind}};
 const browser={currentURI:{spec:'page'},browserId:7,browsingContext:{currentWindowGlobal:{innerWindowId:4}}};
 const provider={getViewUpdate:()=>({input:{textContent:'= 4',attributes:{dir:'ltr'}},output:{textContent:'3.937 inches'},action:{l10n:{id:'copy'}}})};
 const picks=[];
 Object.assign(s,{_sequence:0,revision:2,nativeVisible:['r'],
  window:{document:{l10n:{formatValue:async id=>'Localized '+id}},gBrowser:{selectedBrowser:browser},KeyboardEvent:class {constructor(_,args){Object.assign(this,args);}}},
  input:{controller:{parentController:{manager:{getProvider:()=>provider}}},view:{},inputField:{value:'query'},pickResult:args=>picks.push(args)},
  active:{revision:2,rows:[row],results:[result],resultIndexes:[0],context:{results:[result]},inputValue:'query',browser,page:'page'}});
 s.input.controller ??= {};
 s.input.view.nativePresentation=s;
 s.nativeOpen=true;
 return {s,row,result,browser,provider,picks};
}
test('copy rows use provider view output and Firefox localization',async()=>{
 for(const kind of ['calculator','unitConversion']) {
  const f=fixture(kind);await f.s.loadNativeCopyResult(f.s.active,f.row,f.result);
  assert.equal(f.row.title,kind==='calculator'?'= 4':'3.937 inches');
  assert.equal(f.row.actionLabel,'Localized copy');assert.equal(f.row.titleLtr,true);
  assert.equal(f.s.canActivate(f.result),true);
  assert.equal(f.s.activate(2,'r',{ctrlKey:true}),'activated');
  assert.equal(f.picks[0].result,f.result);assert.equal(f.picks[0].event.ctrlKey,true);
  assert.equal(f.s.activate(2,'r'),'stale');
 }
});
test('undefined calculator output uses its localized message',async()=>{
 const f=fixture();f.provider.getViewUpdate=()=>({input:{l10n:{id:'undefined'}},action:{l10n:{id:'copy'}}});
 await f.s.loadNativeCopyResult(f.s.active,f.row,f.result);
 assert.equal(f.row.title,'Localized undefined');assert.equal(f.row.titleLtr,false);
});
test('copy admission is limited to the matching Firefox provider and dynamic template',()=>{
 for(const change of [r=>r.providerName='Extension',r=>r.payload.dynamicType='weather',r=>r.type=2]) {
  const f=fixture();change(f.result);assert.equal(f.s.nativeCopyProvider(f.result),null);
 }
 const f=fixture();f.result.isHiddenExposure=true;assert.equal(f.s.canActivate(f.result),false);
});
test('late preparation cannot cross row, query, tab, or document replacement',async()=>{
 for(const change of [f=>f.s.active={},f=>f.s.active.rows=[],f=>f.browser.currentURI.spec='new',
  f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.s.window.gBrowser.selectedBrowser={},f=>f.s.closed=true]) {
  const f=fixture();const update=f.provider.getViewUpdate();let resolve;
  f.provider.getViewUpdate=()=>new Promise(r=>resolve=r);
  const pending=f.s.loadNativeCopyResult(f.s.active,f.row,f.result);change(f);resolve(update);await pending;
  assert.equal(f.row.copyDocument,undefined);
 }
});
test('hidden, unprepared, and replaced-document copy actions cannot reach Firefox',async()=>{
 for(const change of [f=>f.s.nativeVisible=[],f=>delete f.row.copyDocument,f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++]) {
  const f=fixture();await f.s.loadNativeCopyResult(f.s.active,f.row,f.result);change(f);
  assert.equal(f.s.activate(2,'r'),'stale');assert.equal(f.picks.length,0);
 }
});
