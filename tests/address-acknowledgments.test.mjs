import assert from 'node:assert/strict';
import test from 'node:test';
import {WinUIDismissalAcknowledgments} from '../upstream/firefox/browser/base/content/WinUIDismissalAcknowledgments.sys.mjs';
function fixture() {
  const browser={currentURI:{spec:'page'},browserId:7,browsingContext:{currentWindowGlobal:{innerWindowId:4}}};
  const rows=['a','b','c'].map(id=>({id}));
  let serial=0,picked=0;
  const s={sessionId:'s',sequence:0,editId:1,nativeOpen:true,nativeVisible:['a','b','c'],
    active:{browser,page:'page',inputValue:'query',revision:2,rows},
    window:{gBrowser:{selectedBrowser:browser},document:{l10n:{formatValue:async()=> 'Feedback'}},KeyboardEvent:class{}},
    input:{inputField:{value:'query'},view:{},resultActions:{
      createDismissalAcknowledgment:()=>({id:++serial,payload:{icon:'brand'}}),
      describeButtons:async()=>[{label:'Got it',enabled:true,action:{name:'0'}}]},pickResult(){picked++;}}};
  s.input.view.nativePresentation=s;
  s.resultActions=s.input.resultActions;
  const a=new WinUIDismissalAcknowledgments(s);
  async function add(index=0){await a.add({}, {id:'feedback'},index);return [...a.entries.values()].at(-1);}
  function request(entry){s.nativeVisible.push(entry.id);return {version:1,session:'s',revision:2,id:entry.id,actionId:entry.row.tipButtons[0].id};}
  return {s,a,browser,add,request,get picked(){return picked;}};
}
test('acknowledgments decorate presentation without changing real query rows',async()=>{
  const f=fixture(),e=await f.add();f.s.active.rows.shift();
  assert.deepEqual(f.a.decorate(f.s.active.rows).map(r=>r.id),[e.id,'b','c']);
  assert.deepEqual(f.s.active.rows.map(r=>r.id),['b','c']);
  assert.equal(e.row.tipButtons[0].label,'Got it');
  assert.equal(f.a.activate(f.request(e)),'activated');assert.equal(f.picked,1);
  assert.equal(f.a.activate(f.request(e)),'stale');assert.equal(f.picked,1);
});
test('out-of-order dismissals retain original display positions',async()=>{
  const f=fixture(),second=await f.add(1);f.s.active.rows.splice(1,1);
  const first=await f.add(0);f.s.active.rows.shift();
  assert.deepEqual(f.a.decorate(f.s.active.rows).map(r=>r.id),[first.id,second.id,'c']);
  f.a.remove(first.tip.id);
  assert.deepEqual(f.a.decorate(f.s.active.rows).map(r=>r.id),[second.id,'c']);
});
test('hidden, stale, altered, modal and replaced-document acknowledgments cannot activate',async()=>{
  for(const change of [f=>f.s.nativeVisible=[],f=>f.s.nativeOpen=false,f=>f.s.editId++,
    f=>f.browser.currentURI.spec='other',f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,
    f=>f.s.window.gBrowser.selectedBrowser={},f=>f.s.input.inputField.value='other',
    f=>f.s.input.readOnly=true,f=>f.s.window.gDialogBox={isOpen:true},f=>f.s.active={...f.s.active},
    f=>f.s.closed=true,f=>f.s.input.view.nativePresentation=null]){
    const f=fixture(),e=await f.add(),r=f.request(e);change(f);
    assert.equal(f.a.activate(r),'stale');assert.equal(f.picked,0);
  }
  for(const override of [{revision:3},{session:'other'},{version:2},{actionId:'wrong'}]){
    const f=fixture(),e=await f.add();assert.equal(f.a.activate({...f.request(e),...override}),'stale');
  }
});
test('late localization is discarded after document replacement or clear',async()=>{
  for(const clear of [false,true]){
    const f=fixture();let resolve;
    f.s.window.document.l10n.formatValue=()=>new Promise(r=>resolve=r);
    const pending=f.add();if(clear)f.a.clear();else f.browser.browsingContext.currentWindowGlobal.innerWindowId++;
    resolve('Feedback');await pending;assert.equal(f.a.entries.size,0);
  }
});
test('failed preparation releases its pending entry',async()=>{
  const f=fixture();f.s.resultActions.describeButtons=async()=>{throw Error('prepare');};
  await assert.rejects(f.add(),/prepare/);assert.equal(f.a.entries.size,0);
});
