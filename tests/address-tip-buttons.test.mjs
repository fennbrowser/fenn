import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console,UrlbarShared:{RESULT_TYPE:{TIP:7}}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);
function fixture(){
 const s=Object.create(context.Session.prototype),row={id:'r'},result={type:7,payload:{buttons:[{name:'0'}],text:'Firefox tip'}};
 const element={textContent:'Localized button',getAttribute:()=>null,hasAttribute:()=>false};
 const browser={currentURI:{spec:'page'},browserId:7,browsingContext:{currentWindowGlobal:{innerWindowId:4}}};
 let picked=0;
 Object.assign(s,{sessionId:'s',_sequence:0,changed(){},nativeOpen:true,nativeVisible:['r'],
  window:{gBrowser:{selectedBrowser:browser},KeyboardEvent:class{}},
  input:{inputField:{value:'tip'},view:{prepareNativeTipButtons:async()=>[element]},pickResult(){picked++;}},
  active:{revision:2,rows:[row],results:[result],resultIndexes:[0],context:{results:[result]},inputValue:'tip',browser,page:'page'}});
 s.input.view.nativePresentation=s;
 const request={version:1,session:'s',revision:2,id:'r',actionId:'r:tip:0'};
 return {s,row,result,element,browser,request,get picked(){return picked;}};
}
test('tip descriptors use Firefox elements, labels and title',async()=>{
 const f=fixture();await f.s.loadNativeTipButtons(f.s.active,f.row,f.result);
 assert.equal(f.row.title,'Firefox tip');assert.equal(f.row.tipButtons[0].label,'Localized button');
 assert.equal(f.s.activateTipButton(f.request),'activated');assert.equal(f.picked,1);
 assert.equal(f.s.activateTipButton(f.request),'stale');
});
test('late tip preparation cannot update another query',async()=>{
 const f=fixture();let resolve;f.s.input.view.prepareNativeTipButtons=()=>new Promise(r=>resolve=r);
 const promise=f.s.loadNativeTipButtons(f.s.active,f.row,f.result);f.s.active={};resolve([f.element]);await promise;
 assert.equal(f.row.tipButtons,undefined);
});
test('hidden, changed, stale and disabled tip actions are rejected',async()=>{
 for(const change of [f=>f.s.nativeVisible=[],f=>f.s.active.revision++,f=>f.browser.currentURI.spec='new',
 f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.s.input.inputField.value='new',
 f=>f.s.window.gBrowser.selectedBrowser={},f=>f.element.hidden=true,f=>f.result.payload.buttons=[],
 f=>f.request.session='other',f=>f.request.intent={arbitrary:true}]) {
 const f=fixture();await f.s.loadNativeTipButtons(f.s.active,f.row,f.result);change(f);
 assert.equal(f.s.activateTipButton(f.request),'stale');assert.equal(f.picked,0);
 }
});

test('a document replacement during tip preparation cannot publish old buttons',async()=>{
 const f=fixture();let resolve;f.s.input.view.prepareNativeTipButtons=()=>new Promise(r=>resolve=r);
 const promise=f.s.loadNativeTipButtons(f.s.active,f.row,f.result);
 f.browser.browsingContext.currentWindowGlobal.innerWindowId++;resolve([f.element]);await promise;
 assert.equal(f.row.tipButtons,undefined);
});

test('tip title and description use Firefox localization',async()=>{
 const f=fixture();f.result.payload.titleL10n={id:'title'};f.result.payload.descriptionL10n={id:'description'};
 f.s.window.document={l10n:{formatValue:async id=>'Localized '+id}};
 await f.s.loadNativeTipButtons(f.s.active,f.row,f.result);
 assert.equal(f.row.title,'Localized title');assert.equal(f.row.description,'Localized description');
});

test('feedback is localized and late feedback cannot cross a navigation',async()=>{
 for(const stale of [false,true]) {
  const f=fixture();f.result.id=4;let resolve;
  f.s.window.document={l10n:{formatValue:()=>new Promise(r=>resolve=r)}};
  const pending=f.s.acknowledgeNativeFeedback(4);
  if(stale)f.browser.currentURI.spec='new';
  resolve('Thanks for your feedback');await pending;
  assert.equal(f.row.feedback,stale?undefined:'Thanks for your feedback');
 }
});

test('Firefox input-button restart preserves native presentation only for its scoped text',async()=>{
 for(const stale of [false,true]) {
  const f=fixture();f.s.editId=1;f.element.dataset={input:'rewritten',command:'rewrite'};
  f.s.cancelImages=()=>{};f.s.active.context.id=9;
  await f.s.loadNativeTipButtons(f.s.active,f.row,f.result);
  assert.equal(f.s.activateTipButton(f.request),'activated');
  f.s.onQueryCancelled({id:9});assert.ok(f.s.active);
  if(stale)f.s.editId++;
  f.s.input.inputField.value='rewritten';
  f.s.onQueryStarted({id:10,searchString:'rewritten'});
  if(stale)assert.equal(f.s.active,null);
  else {assert.equal(f.s.active.text,'rewritten');assert.equal(f.s.active.context.id,10);assert.equal(f.s.nativeVisible.length,0);}
 }
});

test('inline help retains localized ordering and uses the original Firefox element',async()=>{
 const f=fixture();f.result.payload.descriptionLearnMoreTopic='topic';f.result.payload.descriptionL10n={id:'description'};
 f.s.input.view.prepareNativeTipDescription=async()=>({before:'Before ',after:' after',label:'Learn more',element:f.element});
 await f.s.loadNativeTipDescription(f.s.active,f.row,f.result);
 assert.equal(f.row.description,'Before Learn more after');
 assert.equal(f.s.activateTipButton({...f.request,actionId:f.row.helpLink.id}),'activated');assert.equal(f.picked,1);
 assert.equal(f.row.helpLink,null);
});
test('inline help rejects changed topics and late document replacements',async()=>{
 const f=fixture();f.result.payload.descriptionLearnMoreTopic='topic';f.result.payload.descriptionL10n={id:'description'};
 f.s.input.view.prepareNativeTipDescription=async()=>({before:'',after:'',label:'Help',element:f.element});
 await f.s.loadNativeTipDescription(f.s.active,f.row,f.result);const id=f.row.helpLink.id;
 f.result.payload.descriptionLearnMoreTopic='other';
 assert.equal(f.s.activateTipButton({...f.request,actionId:id}),'stale');assert.equal(f.picked,0);
 f.row.helpLink=null;let resolve;
 f.s.input.view.prepareNativeTipDescription=()=>new Promise(r=>resolve=r);
 const pending=f.s.loadNativeTipDescription(f.s.active,f.row,f.result);
 f.browser.browsingContext.currentWindowGlobal.innerWindowId++;
 resolve({before:'',after:'',label:'Help',element:f.element});await pending;assert.equal(f.row.helpLink,null);
});
