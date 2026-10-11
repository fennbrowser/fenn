import assert from 'node:assert/strict';
import test from 'node:test';
import {WinUIAddressContextMenu} from '../upstream/firefox/browser/base/content/WinUIAddressContextMenu.sys.mjs';
function fixture() {
  let invoked=0, published=0, cleaned=0;
  const node=(attributes={})=>({localName:'menuitem',isConnected:true,children:[],
    getAttribute:key=>attributes[key] || '',dispatchEvent:()=>invoked++});
  const copy=node({command:'cmd_copy',label:'Localized copy'}), extra=node({label:'Localized browser action'});
  const hidden=node({label:'Hidden',hidden:'true'}), disabled=node({label:'Unavailable',disabled:'true'});
  const popup={state:'closed',children:[copy,extra,hidden,node({label:'Collapsed',collapsed:'true'}),disabled],querySelectorAll:()=>[],dispatchEvent:()=>cleaned++};
  const tab={linkedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}};
  const input={inputField:{value:'current text',controllers:{getControllerForCommand:()=>({isCommandEnabled:()=>true})}}};
  const session={sessionId:'s',editId:3,input,changed(){published++;},syncNativeSelection:()=>1,
    window:{gBrowser:{selectedTab:tab},MouseEvent:class{},Event:class{},
      document:{l10n:{translateElements:async()=>{}}},EditContextMenu:{prepare(){this.input=input.inputField;return popup;}}}};
  const adapter=new WinUIAddressContextMenu(session);
  const request={version:1,session:'s',editId:3,text:'current text'};
  return {session,adapter,request,extra,disabled,tab,popup,get invoked(){return invoked;},get published(){return published;},get cleaned(){return cleaned;}};
}
test('native context descriptors retain Firefox order, labels and eligibility',async()=>{
  const f=fixture();await f.adapter.open(f.request);
  assert.deepEqual(f.session.contextSnapshot.items.map(i=>i.label),['Localized copy','Localized browser action','Unavailable']);
  assert.equal(f.session.contextSnapshot.items[0].id,'cmd_copy');
  assert.equal(f.session.contextSnapshot.items[2].enabled,false);
  assert.equal(f.published,1);
});
test('current opaque browser action invokes original node and revokes the opening',async()=>{
  const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
  const action={...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id};
  assert.equal(f.adapter.execute(action),1);assert.equal(f.invoked,1);assert.equal(f.cleaned,1);
  assert.equal(f.session.contextSnapshot,null);assert.equal(f.adapter.execute(action),2);
});
test('stale page, tab, text, edit, disconnected node and disabled actions cannot execute',async()=>{
  for(const change of [f=>f.tab.linkedBrowser.browsingContext.currentWindowGlobal.innerWindowId++,
    f=>f.session.window.gBrowser.selectedTab={},f=>f.session.input.inputField.value='new',
    f=>f.session.editId++,f=>f.extra.isConnected=false,f=>f.extra.hidden=true]) {
    const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;change(f);
    assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),2);
    assert.equal(f.invoked,0);
  }
});
test('superseded asynchronous localization never publishes an old menu',async()=>{
  const f=fixture();let done;
  f.session.window.document.l10n.translateElements=()=>new Promise(resolve=>done=resolve);
  const pending=f.adapter.open(f.request);f.adapter.close();done();await pending;
  assert.equal(f.published,0);assert.equal(f.session.contextSnapshot,null);
});

test('close accepts a carried Undo/Redo edit before revoking the opening',async()=>{
  const f=fixture();await f.adapter.open(f.request);const generation=f.session.contextSnapshot.generation;
  let accepted;
  f.session.syncNativeSelection=request=>{accepted=request;return 1;};
  const close={...f.request,kind:'context-close',generation,editId:4,text:'undone'};
  assert.equal(f.adapter.execute(close),1);assert.equal(accepted,close);
  assert.equal(f.session.contextSnapshot,null);
});

test('disabled native action and wrong generation cannot reach the original controller',async()=>{
  const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
  assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation+1,id:menu.items[1].id}),2);
  assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[2].id}),2);
  assert.equal(f.invoked,0);
});


test('context close cannot apply carried edits to a different tab or document',async()=>{
  for (const change of [f=>f.session.window.gBrowser.selectedTab={},
    f=>f.tab.linkedBrowser.browsingContext.currentWindowGlobal.innerWindowId++]) {
    const f=fixture();await f.adapter.open(f.request);const generation=f.session.contextSnapshot.generation;
    let edited=false;f.session.syncNativeSelection=()=>{edited=true;return 1;};change(f);
    assert.equal(f.adapter.execute({...f.request,kind:'context-close',generation,editId:4,text:'old page edit'}),2);
    assert.equal(edited,false);
  }
});
test('context adapter rejects an unknown action kind or protocol version',async()=>{
  for (const patch of [{kind:'unexpected'},{version:2}]) {
    const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
    assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id,...patch}),2);
    assert.equal(f.invoked,0);
  }
});

test('a reused Firefox edit menu cannot run a prior native input action',async()=>{
  const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
  f.session.window.EditContextMenu.input={};f.popup.state='open';
  assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),2);
  assert.equal(f.invoked,0);assert.equal(f.cleaned,0,'do not synthesize closure of another input popup');
});
test('hidden or disabled submenu parents revoke their child actions',async()=>{
  for(const property of ['hidden','collapsed','disabled']) {
    const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
    f.extra.parentElement={getAttribute:key=>key===property?'true':'',parentElement:f.popup};
    assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),2);
    assert.equal(f.invoked,0);
  }
});

test('an intervening dialog blocks a previously prepared browser action',async()=>{
  const f=fixture();await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
  f.session.window.gDialogBox={isOpen:true};
  assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),2);
  assert.equal(f.invoked,0);
});


function pasteFixture() {
  const f=fixture();let called=0;
  f.extra.id='paste-and-go';f.session.input.canPasteAndGo=()=>true;
  f.session.input.pasteAndGo=()=>called++;
  return {f,get called(){return called;}};
}

test('native Paste and Go holds a semantic command, not the original node',async()=>{
  const p=pasteFixture(),{f}=p;await f.adapter.open(f.request);const menu=f.session.contextSnapshot;
  const id=menu.items[1].id;assert.equal(f.adapter.nodes.has(id),false);
  f.extra.isConnected=false;f.extra.dispatchEvent=()=>{throw Error('retained menu node');};
  assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id}),1);
  assert.equal(p.called,1);assert.equal(f.session.contextSnapshot,null);
});

test('Paste and Go rechecks live editor eligibility and rejects stale context',async()=>{
  for(const change of [f=>f.session.input.canPasteAndGo=()=>false,
    f=>f.session.input.inputField.value='new',f=>f.session.editId++,
    f=>f.tab.linkedBrowser.browsingContext.currentWindowGlobal.innerWindowId++,
    f=>f.session.window.gBrowser.selectedTab={},f=>f.session.window.gDialogBox={isOpen:true},
    f=>f.session.window.EditContextMenu.input={},f=>f.adapter.close()]) {
    const p=pasteFixture(),{f}=p;await f.adapter.open(f.request);const menu=f.session.contextSnapshot;change(f);
    assert.equal(f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),2);
    assert.equal(p.called,0);
  }
});

test('Paste and Go uses live eligibility on publication and revokes the menu on exceptions',async()=>{
  const p=pasteFixture(),{f}=p;f.session.input.canPasteAndGo=()=>false;
  await f.adapter.open(f.request);assert.equal(f.session.contextSnapshot.items[1].enabled,false);
  f.session.input.canPasteAndGo=()=>true;f.session.input.pasteAndGo=()=>{throw Error('paste failed');};
  const menu=f.session.contextSnapshot;
  assert.throws(()=>f.adapter.execute({...f.request,kind:'context-action',generation:menu.generation,id:menu.items[1].id}),/paste failed/);
  assert.equal(f.session.contextSnapshot,null);assert.equal(f.adapter.commands,null);
});
