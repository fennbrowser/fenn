import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

function fixture(){
  const calls=[],prefs=new Map(),nodes=new Map();
  const make=(id,attrs={})=>{const attributes=new Map(Object.entries(attrs));const node={id,localName:'menuitem',
    getAttribute:k=>attributes.get(k),hasAttribute:k=>attributes.has(k),setAttribute:(k,v)=>attributes.set(k,v),
    doCommand:()=>calls.push(id)};nodes.set(id,node);return node;};
  const source=make('developer-button'),popup={children:[]};nodes.set('menuWebDeveloperPopup',popup);
  const window={document:{getElementById:id=>nodes.get(id),l10n:{translateElements:async()=>{}}},getComputedStyle:()=>({display:'-moz-box'}),
    MutationObserver:class {observe(){}disconnect(){}},gBrowser:{selectedTab:{},selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}}};
  const item={id:'widget-1',placed:true,stock:[{id:'developer-button',label:'Developer Tools',enabled:true,shown:true}]};
  const toolbar={window,active:true,navigation:true,supported:()=>true,menuAnchor:()=>({}),owner:{addressSession:{sessionId:'address'},
    addressTab:window.gBrowser.selectedTab,addressDocument:7,invalidateAddress(){}},customization:{current:{items:[item]},nativeOverflow:new Set(),
      entries:new Map([['developer-button',{id:item.id,node:source}]]),ui:{getWidget:()=>({forWindow:()=>({node:source})})}}};
  const startup={initDevTools:()=>calls.push('init')};
  const context=vm.createContext({console,Ci:{nsICommandLineHandler:1},Cc:{'@mozilla.org/devtools/startup-clh;1':{getService:()=>({wrappedJSObject:startup})}},
    Services:{prefs:{addObserver(){},removeObserver(){},getBoolPref:(k,f)=>prefs.get(k)??f},obs:{addObserver(){},removeObserver(){}}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIDeveloperMenu.sys.mjs',import.meta.url),'utf8').replace('export class WinUIDeveloperMenu','this.Menu=class WinUIDeveloperMenu'),context);
  const menu=new context.Menu(toolbar);
  toolbar.navigationState=()=>({developerMenu:menu.snapshot()});
  const add=(id,attrs={})=>{const node=make(id,{label:id,...attrs});popup.children.push(node);return node;};
  const open=async()=>{await menu.open(1);return menu.snapshot();};
  const invoke=(id=1)=>menu.execute({command:`developer-menu:invoke:1:1:${id}`});
  return {menu,toolbar,window,calls,prefs,nodes,popup,make,add,open,invoke,item};
}
test('opening lazily projects Firefox order, labels, disabled and checked state',async()=>{
  const f=fixture();f.add('toolbox',{type:'checkbox',checked:'true'});f.add('blocked',{disabled:'true'});f.add('hidden',{hidden:'true'});
  assert.deepEqual(f.calls,[]);const model=await f.open();assert.deepEqual(f.calls,['init']);
  assert.deepEqual(Array.from(model.items,i=>[i.label,i.enabled,i.checkable,i.checked]),[['toolbox',true,true,true],['blocked',false,false,false]]);
  assert.equal(f.invoke(2),2);assert.equal(f.invoke(),1);assert.equal(f.invoke(),2);assert.deepEqual(f.calls,['init','toolbox']);
});
test('Firefox command elements and local display rules determine availability',async()=>{
  const f=fixture();f.add('source',{command:'cmd-source'});f.make('cmd-source',{disabled:'true'});f.add('css-hidden');
  f.window.getComputedStyle=n=>({display:n.id==='css-hidden'?'none':'-moz-box'});
  assert.equal((await f.open()).items.length,1);assert.equal(f.invoke(),2);
});
test('state, command identity and exact source-node mutations revoke an opening',async()=>{
  for(const change of [f=>f.popup.children[0].setAttribute('disabled','true'),f=>f.popup.children[0].setAttribute('command','new-command'),
    f=>f.popup.children[0].setAttribute('label','changed'),f=>f.popup.children[0]=f.make('same',{label:'toolbox'}),
    f=>f.prefs.set('devtools.policy.disabled',true),f=>f.item.placed=false,f=>f.toolbar.customization.nativeOverflow.add('developer-button'),
    f=>f.toolbar.owner.addressDocument++,f=>f.toolbar.customization.editorOpen=true,f=>f.toolbar.active=false]){
    const f=fixture();f.add('toolbox');await f.open();change(f);assert.equal(f.invoke(),2);assert.equal(f.menu.session,0);
  }
});
test('malformed, stale and unplaced openings are refused without initializing DevTools',()=>{
  const f=fixture();f.item.placed=false;
  for(const command of ['developer-menu:open:1','developer-menu:open:4294967296','developer-menu:open:1:1:1','developer-menu:invoke:1:1:1'])assert.equal(f.menu.execute({command}),2);
  assert.deepEqual(f.calls,[]);
});
test('oversized inventory is refused and a pending localization cannot resurrect dismissed state',async()=>{
  const f=fixture();for(let i=0;i<65;i++)f.add('tool-'+i);await f.open();assert.equal(f.menu.session,0);
  const g=fixture();g.add('toolbox');let finish;g.window.document.l10n.translateElements=()=>new Promise(resolve=>finish=resolve);
  const opening=g.menu.open(1);g.menu.dismiss();finish();await opening;assert.equal(g.menu.session,0);assert.equal(g.menu.description,null);
});
