import assert from 'node:assert/strict';
import test from 'node:test';
import {WinUIResultContextMenu} from '../upstream/firefox/browser/base/content/WinUIResultContextMenu.sys.mjs';
function fixture() {
  let invoked=0;
  const item={localName:'panel-item',dataset:{openIn:'tab'},textContent:'Localized new tab',
    querySelector:()=>null,getAttribute:()=>''};
  const panel={children:[item],querySelectorAll:()=>[item]};
  const browser={currentURI:{spec:'https://example.test/'},browsingContext:{currentWindowGlobal:{innerWindowId:1}}};
  const result={};
  const active={revision:4,browser,rows:[{id:'row'}],results:[result],resultIndexes:[0],context:{results:[result]}};
  const session={sessionId:'session',editId:3,active,nativeVisible:['row'],nativeOpen:true,
    window:{gBrowser:{selectedBrowser:browser},MouseEvent:class{}},changed(){},
    input:{inputField:{value:'typed'},view:{prepareNativeResultMenu:async()=>panel,
      activateNativeResultMenu:()=>invoked++}}};
  session.input.view.nativePresentation=session;
  const adapter=new WinUIResultContextMenu(session);
  const request={version:1,session:'session',editId:3,revision:4,resultId:'row'};
  const action=()=>({...request,kind:'context-action',generation:session.resultMenuSnapshot.generation,id:session.resultMenuSnapshot.items[0].id});
  return {session,adapter,request,action,item,panel,browser,get invoked(){return invoked;}};
}
const tick=()=>new Promise(resolve=>setImmediate(resolve));
test('result menu publishes Firefox labels and executes only an opaque current item',async()=>{
  const f=fixture();await f.adapter.open(f.request);
  assert.equal(f.session.resultMenuSnapshot.items[0].label,'Localized new tab');
  const action=f.action();assert.equal(f.adapter.execute(action),1);await tick();
  assert.equal(f.invoked,1);assert.equal(f.adapter.execute(action),2);
});
test('result menu requires current visible result, revision, and edit',async()=>{
  for(const patch of [{resultId:'hidden'},{revision:3},{editId:2},{version:2},{session:'other'}]) {
    const f=fixture();await f.adapter.open({...f.request,...patch});
    assert.equal(f.session.resultMenuSnapshot,undefined);
  }
});
test('result menu revokes query, page, tab, focus, and edit changes',async()=>{
  for(const change of [f=>f.session.active=null,f=>f.session.nativeVisible=[],f=>f.browser.currentURI.spec+='new',f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,
    f=>f.session.window.gBrowser.selectedBrowser={},f=>f.session.nativeOpen=false,
    f=>f.session.editId++,f=>f.session.input.inputField.value='new',f=>f.session.active.context.results=[]]) {
    const f=fixture();await f.adapter.open(f.request);const action=f.action();change(f);
    assert.equal(f.adapter.execute(action),2);await tick();assert.equal(f.invoked,0);
  }
});
test('late result-menu localization cannot publish after dismissal',async()=>{
  const f=fixture();let done;f.session.input.view.prepareNativeResultMenu=()=>new Promise(r=>done=r);
  const pending=f.adapter.open(f.request);f.adapter.close();done(f.panel);await pending;
  assert.equal(f.session.resultMenuSnapshot,undefined);
});
test('execution rechecks Firefox command eligibility and rejects removed commands',async()=>{
  const f=fixture();await f.adapter.open(f.request);const action=f.action();
  f.session.input.view.prepareNativeResultMenu=async()=>null;
  f.adapter.execute(action);await tick();assert.equal(f.invoked,0);
});
test('page replacement during asynchronous execution cannot activate an old result',async()=>{
  const f=fixture();await f.adapter.open(f.request);let done;
  f.session.input.view.prepareNativeResultMenu=()=>new Promise(r=>done=r);
  f.adapter.execute(f.action());f.browser.browsingContext.currentWindowGlobal.innerWindowId++;
  done(f.panel);await tick();assert.equal(f.invoked,0);
});
test('command closure does not cancel admitted activation and double action is rejected',async()=>{
  const f=fixture();await f.adapter.open(f.request);const action=f.action();
  assert.equal(f.adapter.execute(action),1);assert.equal(f.adapter.execute(action),2);
  f.adapter.execute({...action,kind:'context-close'});await tick();assert.equal(f.invoked,1);
});

// The notification bridge has no DOM row to mutate in native presentation.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
  .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);
test('provider command refresh updates the original result and revokes its menu',()=>{
  const session=Object.create(context.Session.prototype), result={id:7};let closed=0;
  session.active={context:{results:[result]}};session.resultMenuAdapter={close:()=>closed++};
  const commands=[];
  assert.equal(session.updateResultMenuCommands(8,commands),null);assert.equal(closed,0);
  assert.equal(session.updateResultMenuCommands(7,commands),result);
  assert.equal(result.commands,commands);assert.equal(closed,1);
});
test('provider result removal republishes current rows and ignores obsolete IDs',()=>{
  const session=Object.create(context.Session.prototype), first={id:7}, second={id:8};let published;
  session.active={results:[first,second],context:{results:[first,second]}};
  session.onQueryResults=context=>published=context;
  session.onQueryResultRemoved(9);assert.equal(published,undefined);
  session.onQueryResultRemoved(7);assert.deepEqual(published.results,[second]);
});
