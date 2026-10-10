import assert from 'node:assert/strict';
import test from 'node:test';
import {WinUIResultContextMenu} from '../upstream/firefox/browser/base/content/WinUIResultContextMenu.sys.mjs';
function fixture() {
  let invoked=0;
  const item={action:{openIn:'tab'},label:'Localized new tab',enabled:true};
  const panel=[item];
  const browser={currentURI:{spec:'https://example.test/'},browsingContext:{currentWindowGlobal:{innerWindowId:1}}};
  const result={};
  const active={revision:4,browser,rows:[{id:'row'}],results:[result],resultIndexes:[0],context:{results:[result]}};
  const session={sessionId:'session',editId:3,active,nativeVisible:['row'],nativeOpen:true,
    window:{gBrowser:{selectedBrowser:browser},MouseEvent:class{}},changed(){},
    input:{inputField:{value:'typed'},view:{},resultActions:{describeMenu:async()=>panel,invokeMenu:()=>invoked++}}};
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
  const f=fixture();let done;f.session.input.resultActions.describeMenu=()=>new Promise(r=>done=r);
  const pending=f.adapter.open(f.request);f.adapter.close();done(f.panel);await pending;
  assert.equal(f.session.resultMenuSnapshot,undefined);
});
test('execution rechecks Firefox command eligibility and rejects removed commands',async()=>{
  const f=fixture();await f.adapter.open(f.request);const action=f.action();
  f.session.input.resultActions.describeMenu=async()=>null;
  f.adapter.execute(action);await tick();assert.equal(f.invoked,0);
});
test('page replacement during asynchronous execution cannot activate an old result',async()=>{
  const f=fixture();await f.adapter.open(f.request);let done;
  f.session.input.resultActions.describeMenu=()=>new Promise(r=>done=r);
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

test('tip dropdown uses only its current Firefox button menu',async()=>{
 const f=fixture();
 f.session.active.results[0].payload={buttons:[{name:'choice',menu:[{name:'dismiss'}]}]};
 const result=f.session.active.results[0];
 f.session.active.rows[0].tipButtons=[{id:'tip',name:'choice',menu:true,enabled:true,document:1,signature:JSON.stringify(result.payload.buttons)}];
 let names=[];
 f.session.input.resultActions.describeMenu=async (r,name)=>{names.push(name);return f.panel;};
 await f.adapter.open({...f.request,buttonId:'missing'});assert.equal(f.session.resultMenuSnapshot,undefined);
 f.browser.browsingContext.currentWindowGlobal.innerWindowId=2;
 await f.adapter.open({...f.request,buttonId:'tip'});assert.equal(f.session.resultMenuSnapshot,undefined);
 f.browser.browsingContext.currentWindowGlobal.innerWindowId=1;
 await f.adapter.open({...f.request,buttonId:'tip'});
 assert.deepEqual(names,['choice']);
 assert.equal(f.adapter.execute(f.action()),1);await tick();
 assert.deepEqual(names,['choice','choice']);assert.equal(f.invoked,1);
 await f.adapter.open({...f.request,buttonId:'tip'});const action=f.action();
 result.payload.buttons[0].menu=[];
 assert.equal(f.adapter.execute(action),2);assert.equal(f.invoked,1);
});

test('result menu preserves Firefox checkbox state and delegates its command',async()=>{
  const f=fixture();
  f.item.checkable=true;f.item.checked=true;
  f.item.action={command:'toggle-keyboard-accessible'};
  await f.adapter.open(f.request);
  assert.equal(f.session.resultMenuSnapshot.items[0].checkable,true);
  assert.equal(f.session.resultMenuSnapshot.items[0].checked,true);
  assert.equal(f.adapter.execute(f.action()),1);await tick();assert.equal(f.invoked,1);
  f.item.checked=false;await f.adapter.open(f.request);
  assert.equal(f.session.resultMenuSnapshot.items[0].checked,false);
});

test('a provider opening another menu during invocation does not lose that menu',async()=>{
  const f=fixture();await f.adapter.open(f.request);
  let next;
  f.session.input.resultActions.invokeMenu=()=>{next=f.adapter.open(f.request);};
  f.adapter.execute(f.action());await tick();await next;
  assert.ok(f.session.resultMenuSnapshot);
  assert.equal(f.adapter.state.invoking,undefined);
});

test('late command localization cannot overwrite a newer menu generation',async()=>{
  const f=fixture();await f.adapter.open(f.request);const old=f.action();let resolve;
  f.session.input.resultActions.describeMenu=()=>new Promise(r=>resolve=r);
  f.adapter.execute(old);
  f.session.input.resultActions.describeMenu=async()=>f.panel;
  await f.adapter.open(f.request);const generation=f.session.resultMenuSnapshot.generation;
  resolve(f.panel);await tick();
  assert.equal(f.invoked,0);assert.equal(f.session.resultMenuSnapshot.generation,generation);
});

test('native menu enforces its own capacity without publishing a partial menu',async()=>{
  const f=fixture();
  f.session.input.resultActions.describeMenu=async()=>Array.from({length:129},()=>f.item);
  await assert.rejects(f.adapter.open(f.request),/too large/);
  assert.equal(f.session.resultMenuSnapshot,undefined);
  assert.equal(f.adapter.state, null);
});
