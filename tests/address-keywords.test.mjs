import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console,UrlbarShared:{RESULT_TYPE:{URL:3,TAB_SWITCH:4,KEYWORD:5,OMNIBOX:9,SEARCH:2,DYNAMIC:6}}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIAddressSession','this.Session = class WinUIAddressSession'),context);
function fixture() {
 const s=Object.create(context.Session.prototype),calls=[];
 const result={type:5,providerName:'UrlbarProviderBookmarkKeywords',payload:{url:'https://example.test/',postData:'q=a%2Bb',input:'kw a+b',keyword:'kw'}};
 const browser={browserId:7,currentURI:{spec:'page'},browsingContext:{currentWindowGlobal:{innerWindowId:9}}};
 const row={id:'r',keywordDocument:9};
 Object.assign(s,{_revision:2,_sequence:0,nativeVisible:['r'],window:{gBrowser:{selectedBrowser:browser},KeyboardEvent:class {constructor(type,args){Object.assign(this,args);}}},
  input:{inputField:{value:'kw a+b'},view:{},focus(){},pickResult(args){assert.equal(s.active,null);calls.push(args);}},
  active:{revision:2,rows:[row],results:[result],resultIndexes:[0],context:{results:[result]},inputValue:'kw a+b',browser,page:'page'}});
 s.input.controller ??= {};
 s.input.view.nativePresentation=s;
 s.nativeOpen=true;
 return {s,row,result,browser,calls};
}
test('only Firefox bookmark keywords are admitted and pickResult retains POST and modifier metadata',()=>{
 const f=fixture();assert.equal(f.s.canActivate(f.result),true);
 for(const intent of [{},{altKey:true},{ctrlKey:true,shiftKey:true},{metaKey:true}]) {
  const f=fixture();assert.equal(f.s.activate(2,'r',intent),'activated');
  assert.equal(f.calls.length,1);assert.equal(f.calls[0].result,f.result);
  assert.equal(f.calls[0].browserId,7);assert.equal(f.calls[0].result.payload.postData,'q=a%2Bb');
  for(const [k,v] of Object.entries(intent)) assert.equal(f.calls[0].event[k],v);
  assert.equal(f.s.activate(2,'r',intent),'stale');
 }
 f.result.providerName='Extension';assert.equal(f.s.canActivate(f.result),false);
});
test('keyword activation rejects hidden, replaced-document, modal, readonly and newer-edit state',()=>{
 for(const change of [f=>f.s.nativeVisible=[],f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,
  f=>f.s.window.gDialogBox={isOpen:true},f=>f.s.input.readOnly=true,f=>f.s.input.inputField.value='new',f=>f.s.nativeOpen=false]) {
  const f=fixture();change(f);assert.equal(f.s.activate(2,'r'),'stale');assert.equal(f.calls.length,0);
 }
});
test('bare keyword uses Firefox localized Visit and stale localization cannot alter a new query',async()=>{
 const f=fixture();f.result.payload.input='kw';let finish;
 f.s.window.document={l10n:{formatValue(id){assert.equal(id,'urlbar-result-action-visit');return new Promise(r=>finish=r);}}};
 const pending=f.s.loadNativeDetails(f.s.active,f.row,f.result);finish('Localized Visit');await pending;assert.equal(f.row.actionLabel,'Localized Visit');
 delete f.row.actionLabel;const stale=f.s.loadNativeDetails(f.s.active,f.row,f.result);f.s.active=null;finish('stale');await stale;assert.equal(f.row.actionLabel,undefined);
});

test('omnibox commands preserve Firefox result identity, modifiers, and content label',async()=>{
 const f=fixture();f.result.type=9;f.result.providerName='UrlbarProviderOmnibox';
 f.result.payload={title:'Extension command',content:'ext non-url command',keyword:'ext'};
 assert.equal(f.s.canActivate(f.result),true);
 await f.s.loadNativeDetails(f.s.active,f.row,f.result);
 assert.equal(f.row.actionLabel,'ext non-url command');
 assert.equal(f.s.activate(2,'r',{altKey:true}),'activated');
 assert.equal(f.calls[0].result,f.result);assert.equal(f.calls[0].event.altKey,true);
 assert.equal(f.s.activate(2,'r'),'stale');
 f.result.providerName='Other';assert.equal(f.s.canActivate(f.result),false);
});
test('omnibox rejects invisible, replaced-document and readonly results',()=>{
 for(const change of [f=>f.s.nativeVisible=[],f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.s.input.readOnly=true]) {
  const f=fixture();f.result.type=9;f.result.providerName='UrlbarProviderOmnibox';change(f);
  assert.equal(f.s.activate(2,'r'),'stale');assert.equal(f.calls.length,0);
 }
});
