import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
const base='../upstream/firefox/browser/base/content/';
function fixture({count=20,index=10,fallback=false,filter=false}={}){
  const calls=[],entries=Array.from({length:count},(_,i)=>({ID:i+1,URI:{spec:'https://example.com/'+i},url:'https://example.com/'+i,title:'Page '+i,hasUserInteraction:true}));
  const history={count,index,getEntryAtIndex:i=>entries[i]},cached={index,entries};let callback;
  class Node{
    constructor(){this.attributes=new Map();this.children=[];this.listeners=new Map();this.style={setProperty(){}};}
    getAttribute(k){return this.attributes.get(k) || '';}
    hasAttribute(k){return this.attributes.has(k);}
    setAttribute(k,v){this.attributes.set(k,String(v));}
    addEventListener(k,v){this.listeners.set(k,v);}
    appendChild(n){this.children.push(n);n.parentNode=this;}
    removeChild(n){this.children.splice(this.children.indexOf(n),1);n.parentNode=null;}
    get lastElementChild(){return this.children.at(-1);}
    dispatchEvent(e){e.target=this;this.parentNode.listeners.get('command')(e);}
    hidePopup(){calls.push('hide');}
  }
  const browser={browsingContext:{sessionHistory:fallback?null:history,currentWindowGlobal:{innerWindowId:7}},contentPrincipal:{equals:p=>p===browser.contentPrincipal}};
  const tab={linkedBrowser:browser},window={gBrowser:{selectedTab:tab,selectedBrowser:browser},document:{createXULElement:()=>new Node()},
    XULBrowserWindow:{setOverLink:v=>calls.push(['status',v])},MutationObserver:class{observe(){}disconnect(){}},
    MouseEvent:class{constructor(type,options){Object.assign(this,{type},options);}},
    windowUtils:{setHandlingUserInput:()=>({destruct:()=>calls.push('input-end')})},BrowserCommands:{gotoHistoryIndex:event=>calls.push(['navigate',event.target.getAttribute('index'),event.target.getAttribute('historyindex'),event.button,event.ctrlKey,event.shiftKey])},
    Image:class{decode(){return Promise.reject();}},setTimeout,clearTimeout};
  window.SessionStore={getSessionHistory(_tab,cb){if(cb)callback=cb;return cached;}};
  const state={back:{enabled:index>0,label:'Back'},forward:{enabled:index<count-1,label:'Forward'}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,customization:{editorOpen:false},menuAnchor:()=>({}),navigationModel:{snapshot:()=>state},owner:{addressTab:tab,addressDocument:7,addressSession:{sessionId:'address'},invalidateAddress(){}}};
  const favicons={getFaviconForPage:async()=>({dataURI:{spec:'data:image/png;base64,AA=='}})};
  const context=vm.createContext({console,ChromeUtils:{importESModule:()=>({PlacesUtils:{favicons}})},Services:{io:{newURI:spec=>({spec})},obs:{addObserver(){},removeObserver(){}}},document:window.document,gBrowser:window.gBrowser,
    gNavigatorBundle:{getString:k=>k},XULBrowserWindow:window.XULBrowserWindow,CSS:{escape:x=>x},BrowserUtils:{navigationRequireUserInteraction:filter},SessionStore:window.SessionStore});
  const source=readFileSync(new URL(base+'browser.js',import.meta.url),'utf8');
  vm.runInContext(source.slice(source.indexOf('function FillHistoryMenu('),source.indexOf('function toOpenWindowByType(')),context);window.FillHistoryMenu=context.FillHistoryMenu;
  vm.runInContext(readFileSync(new URL(base+'WinUIBackForwardMenu.sys.mjs',import.meta.url),'utf8').replace('export class WinUIBackForwardMenu','this.Menu=class WinUIBackForwardMenu'),context);
  const menu=new context.Menu(toolbar);
  return {menu,toolbar,window,state,history,cached,entries,calls,favicons,callback:()=>callback,open:async()=>{await menu.open(1,'back');return menu.snapshot();},
    invoke:(id,kind='invoke',revision=menu.description?.revision)=>menu.execute({command:`back-forward-menu:${kind}:1:${revision}:${id}`,modifiers:5})};
}

test('actual Firefox controller supplies 15 ordered entries, current check and tooltips',async()=>{
 const f=fixture();const model=await f.open();assert.equal(model.items.length,15);
 assert.deepEqual(Array.from(model.items,i=>i.label),Array.from({length:15},(_,i)=>'Page '+(17-i)));
 assert.equal(model.items.filter(i=>i.checked).length,1);assert.equal(model.items.find(i=>i.checked).label,'Page 10');
 assert.equal(model.items[0].tooltip,'tabHistory.goForward');assert.equal(model.items.at(-1).tooltip,'tabHistory.goBack');
 assert.equal(f.invoke(15),1);assert.deepEqual(f.calls.find(c=>Array.isArray(c)&&c[0]==='navigate'),['navigate','3','-7',0,true,true]);assert.equal(f.invoke(15),2);
});
test('Firefox interaction filter and boundary window remain unchanged',async()=>{
 const f=fixture({index:1,filter:true});f.entries[3].hasUserInteraction=false;
 const m=await f.open();assert.equal(m.items.length,14);assert.equal(m.items[0].label,'Page 14');assert(!m.items.some(i=>i.label==='Page 3'));
 assert.equal(m.items.find(i=>i.checked).label,'Page 1');
});
test('middle-click uses the original relative index including current-page duplication',async()=>{
 const f=fixture();const m=await f.open();const current=m.items.find(i=>i.checked);
 assert.equal(f.invoke(current.id,'middle'),1);assert.deepEqual(f.calls.find(c=>Array.isArray(c)&&c[0]==='navigate'),['navigate','10','0',1,true,true]);
});
test('same-document history replacement, count/index changes, owner and source changes revoke actions',async()=>{
 for(const change of [f=>f.entries[10].ID++,f=>f.entries[10].URI.spec+='?replacement',f=>f.history.index--,f=>f.history.count--,
   f=>f.toolbar.owner.addressDocument++,f=>f.toolbar.owner.addressSession.sessionId='new',f=>f.window.gBrowser.selectedTab={},
   f=>f.toolbar.customization.editorOpen=true,f=>f.toolbar.active=false,f=>f.state.back.enabled=false]){
  const f=fixture();await f.open();change(f);assert.equal(f.invoke(1),2);assert.equal(f.menu.session,0);
 }
});
test('SessionStore fallback uses the same controller and delayed callbacks cannot revive dismissal',async()=>{
 const f=fixture({fallback:true});assert.equal((await f.open()).items.length,15);const popup=f.menu.popup;
 f.menu.dismiss();f.cached.entries.push({url:'https://example.com/new',title:'New'});f.callback()(f.cached,false,false);
 f.menu.publish();assert.equal(f.menu.session,0);assert.equal(f.menu.description,null);assert(popup.children.length);
});
test('stale revisions, IDs, malformed and unavailable openings are refused',async()=>{
 const f=fixture();await f.open();assert.equal(f.invoke(1,'invoke',3),2);assert.equal(f.invoke(999),2);
 for(const command of ['back-forward-menu:open:1:back','back-forward-menu:open:4294967296:back','back-forward-menu:open:2','back-forward-menu:open:2:sideways','back-forward-menu:open:2:1:1','back-forward-menu:dismiss:1:back'])assert.equal(f.menu.execute({command}),2);
 f.state.forward.enabled=false;assert.equal(f.menu.execute({command:'back-forward-menu:open:2:forward'}),2);
 f.toolbar.menuAnchor=()=>null;assert.equal(f.menu.execute({command:'back-forward-menu:open:2:back'}),2);
});
test('one-entry parent history cancels native opening',async()=>{
 const f=fixture({count:1,index:0});await f.open();assert.equal(f.menu.session,0);
});

test('hover status and icon-only revision handoff do not navigate or retarget entries',async()=>{
 const f=fixture();await f.open();assert.equal(f.invoke(1,'hover',1),1);assert.deepEqual(f.calls.at(-1),['status','https://example.com/17']);const count=f.calls.length;assert.equal(f.invoke(1,'hover',1),1);assert.equal(f.calls.length,count,'unchanged status must not dispatch another browser OverLink event');
 assert.equal(f.invoke(1,'leave'),1);assert.deepEqual(f.calls.at(-1),['status','']);assert.equal(f.menu.session,1);assert.equal(f.invoke(1,'invoke',1),1);
});
test('favicon completion publishes a new immutable description and cannot resurrect dismissal',async()=>{
 for(const dismiss of [false,true]){
  const f=fixture({count:2,index:1});let finish;
  f.window.Image=class{naturalWidth=16;naturalHeight=16;decode(){return new Promise(resolve=>finish=resolve);}};
  f.window.document.createElementNS=()=>({getContext:()=>({drawImage(){},getImageData(){return {};}})});
  f.toolbar.owner.encodePixels=()=>({width:16,height:16,pixels:'00'.repeat(1024)});
  const pending=f.menu.open(1,'back'),first=f.menu.snapshot();assert.equal(first.revision,1);assert(first.items.every(i=>i.image===null));
  await new Promise(resolve=>setImmediate(resolve));if(dismiss)f.menu.dismiss();finish();await pending;
  assert(first.items.every(i=>i.image===null),'a published revision is immutable');
  if(dismiss){assert.equal(f.menu.description,null);assert.equal(f.menu.session,0);}
  else{const next=f.menu.snapshot();assert.equal(next.revision,2);assert(next.items.some(i=>i.image));assert.notEqual(next,first);}
 }
});

test('missing stored favicons publish the native default without decoding black fallback artwork',async()=>{
 const f=fixture();let decodes=0;f.favicons.getFaviconForPage=async()=>null;f.window.Image=class{decode(){decodes++;return Promise.reject();}};
 const model=await f.open();assert.equal(model.revision,2);assert(model.items.every(i=>i.image===null));assert.equal(decodes,0);
});
