import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console});
const pins = new Map();
context.Services={prefs:{getBoolPref:(key,fallback)=>pins.get(key) ?? fallback,
  setBoolPref:(key,value)=>pins.set(key,value)}};
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIOmnibarActions.sys.mjs',import.meta.url),'utf8')
  .replace('export class WinUIOmnibarActions','this.Actions = class WinUIOmnibarActions'),context);
function fixture() {
  const adapter=Object.create(context.Actions.prototype);
  const node={nodeType:1,isConnected:true,contains:n=>n===node};
  const tab={}; let document=1; let visible=true; const dispatched=[];
  node.dispatchEvent=e=>dispatched.push(e);
  const anchor={};
  const toolbar={active:true,navigation:true,supported:()=>true,menuAnchor:()=>anchor,menuAnchors:new Map([['omni:action-1',anchor]])};
  const window={gBrowser:{selectedTab:tab}, MouseEvent:class {constructor(type,data){this.type=type;Object.assign(this,data);}}};
  Object.assign(adapter,{toolbar,window,events:new WeakSet(),entries:new Map([[node,{node,id:'action-1',document:1,tab,pending:true}]]),context:()=>document,
    visible:()=>visible,snapshot:()=>({omnibarLeft:visible?[{id:'action-1',enabled:true}]:[],omnibarRight:[]})});
  return {adapter,node,anchor,toolbar,dispatched,window,changeDocument:()=>document++,hide:()=>visible=false};
}
test('native page actions preserve original target and modifiers',()=>{
  const f=fixture(); assert.equal(f.adapter.activate('omni:action-1',{}, {ctrlKey:true,shiftKey:true}),1);
  assert.equal(f.dispatched.length,1); assert.equal(f.dispatched[0].type,'click'); assert.equal(f.dispatched[0].ctrlKey,true);
  assert.equal(f.dispatched[0].shiftKey,true);
});
test('hidden or revoked action cannot execute',()=>{
  const f=fixture(); f.hide(); assert.equal(f.adapter.activate('omni:action-1',{},{}),2);
  assert.equal(f.dispatched.length,0);
});

test('an unpinned menu action still invokes the original Firefox target',()=>{
  const f=fixture();
  f.adapter.snapshot=()=>({omnibarLeft:[],omnibarRight:[],pageActions:{items:[{id:'action-1',enabled:true,pinned:false}]}});
  assert.equal(f.adapter.activate('omni:action-1',{}, {shiftKey:true}),1);
  assert.equal(f.dispatched.length,1); assert.equal(f.dispatched[0].shiftKey,true);
});
test('popup anchors are scoped to current tab and document',()=>{
  const f=fixture(); assert.equal(f.adapter.anchorFor(f.node),f.anchor);
  f.changeDocument(); assert.equal(f.adapter.anchorFor(f.node),f.node);
  const g=fixture(); g.window.gBrowser.selectedTab={}; assert.equal(g.adapter.anchorFor(g.node),g.node);
});
test('unrelated popup anchors and disabled native presentation stay unchanged',()=>{
  const f=fixture(); const other={nodeType:1}; assert.equal(f.adapter.anchorFor(other),other);
  assert.equal(f.adapter.anchorFor(undefined),undefined);
  f.toolbar.active=false; assert.equal(f.adapter.anchorFor(f.node),f.node);
});
test('rejected native geometry cannot invoke a browser action',()=>{
  const f=fixture(); f.toolbar.menuAnchor=()=>null;
  assert.equal(f.adapter.activate('omni:action-1',{},{}),2); assert.equal(f.dispatched.length,0);
});

test('native popup repositioning retains its anchor until original Firefox input',()=>{
  const f=fixture(); assert.equal(f.adapter.anchorFor(f.node),f.anchor);
  assert.equal(f.adapter.anchorFor(f.node),f.anchor);
  const nativeEvent={isTrusted:true}; f.adapter.events.add(nativeEvent);
  f.adapter.onOriginalInput(nativeEvent); assert.equal(f.adapter.anchorFor(f.node),f.anchor);
  f.adapter.onOriginalInput({isTrusted:true});
  assert.equal(f.adapter.anchorFor(f.node),f.node);
});

function menuFixture() {
  pins.clear();
  const ids=['trust-icon-container','identity-icon-box','identity-box','tracking-protection-icon-container',
    'identity-permission-box','reader-mode-button','picture-in-picture-button','taskbar-tabs-button',
    'translations-button','star-button-box','pageActionButton','share-button','extension-action'];
  const nodes=new Map(ids.map(id=>[id,{id,isConnected:true,hidden:false,attrs:{},textContent:id,
    classes:new Set(),classList:{contains(c){return nodes.get(id).classes.has(c);}},
    getAttribute(name){return this.attrs[name] || '';},hasAttribute(name){return name in this.attrs;},querySelectorAll(){return [];},querySelector(){return null;},matches(){return false;}}]));
  nodes.get('trust-icon-container').hidden=true;
  const document={getElementById:id=>nodes.get(id),querySelectorAll:selector=>selector.startsWith('#page-action-buttons') ?
    [...nodes.values()].filter(n=>!['trust-icon-container','identity-icon-box','identity-box','tracking-protection-icon-container','identity-permission-box','star-button'].includes(n.id)) : []};
  const adapter=Object.create(context.Actions.prototype);
  Object.assign(adapter,{entries:new Map(),nextId:0,images:new Map(),events:new WeakSet(),label:key=>key,context:()=>17,
    toolbar:{menuAnchor:()=>({}),menuAnchors:new Map()},window:{document,gURLBar:{parentNode:{}},
      gBrowser:{selectedTab:{}},getComputedStyle:()=>({display:'block',visibility:'visible',fill:'currentColor',color:'rgb(0,0,0)',listStyleImage:'none'})}});
  return {adapter,nodes};
}

test('page menu has exactly five rows, four default pins and disabled explanations',()=>{
  const {adapter,nodes}=menuFixture();
  nodes.get('picture-in-picture-button').hidden=true;
  const menu=adapter.snapshot().pageActions;
  assert.deepEqual(Array.from(menu.items,i=>i.key),['connection','reader','pip','taskbar','translation']);
  assert.equal(menu.items[0].pinnable,false); assert.equal(menu.items[0].pinned,false);
  assert.ok(menu.items.slice(1).every(i=>i.pinned && i.pinnable));
  assert.equal(menu.items[2].enabled,false); assert.equal(menu.items[2].reason,'pip-unavailable');
});

test('unpinning keeps the action in the menu, persists across adapters and rejects repeated intents',()=>{
  const {adapter,nodes}=menuFixture();
  const reader=adapter.snapshot().pageActions.items[1];
  assert.equal(adapter.activate('omni-unpin:'+reader.id,{},{}),1);
  const next=adapter.snapshot();
  assert.ok(!next.omnibarRight.some(i=>i.id===reader.id));
  assert.equal(next.pageActions.items[1].enabled,true);
  assert.equal(next.pageActions.items[1].pinned,false);
  assert.equal(adapter.activate('omni-unpin:'+reader.id,{},{}),2);
  const another=Object.create(context.Actions.prototype);
  Object.assign(another,adapter,{entries:new Map(),nextId:0});
  assert.equal(another.pageActions().items[1].pinned,false);
  assert.equal(adapter.activate('omni-pin:'+reader.id,{},{}),1);
  assert.ok(adapter.snapshot().omnibarRight.some(i=>i.id===reader.id));
  nodes.get('reader-mode-button').hidden=true;
  assert.equal(adapter.activate('omni:'+reader.id,{},{}),2);
});

test('secure identity moves into menu; insecure and trust warning indicators stay inline',()=>{
  const {adapter,nodes}=menuFixture();
  let state=adapter.snapshot(); let connection=state.pageActions.items[0];
  assert.equal(connection.enabled,true);
  assert.ok(!state.omnibarLeft.some(i=>i.id===connection.id));
  assert.equal(adapter.activate('omni-pin:'+connection.id,{},{}),2);
  nodes.get('identity-box').classes.add('notSecure');
  assert.ok(adapter.snapshot().omnibarLeft.some(i=>i.id===connection.id));
  nodes.get('identity-icon-box').hidden=true; nodes.get('trust-icon-container').hidden=false;
  nodes.get('trust-icon-container').classes.add('warning');
  state=adapter.snapshot(); connection=state.pageActions.items[0];
  assert.ok(state.omnibarLeft.some(i=>i.id===connection.id));
  assert.ok(state.omnibarLeft.some(i=>i.label==='tracking-protection-icon-container'));
});

test('bookmark star is the final published action before native ellipsis; other actions stay inline',()=>{
  const {adapter}=menuFixture(); const state=adapter.snapshot();
  assert.equal(state.omnibarRight.at(-1).label,'star-button-box');
  assert.ok(state.omnibarRight.some(i=>i.label==='extension-action'));
  assert.ok(state.omnibarRight.some(i=>i.label==='share-button'));
  assert.ok(!state.omnibarRight.some(i=>i.label==='pageActionButton'));
});

test('Firefox indicators remain persistent; ordinary actions only reveal on hover',()=>{
  const {adapter,nodes}=menuFixture();
  nodes.get('identity-box').classes.add('notSecure');
  let state=adapter.snapshot();
  assert.ok(state.omnibarLeft.length>0 && state.omnibarLeft.every(i=>i.alwaysVisible));
  assert.ok(state.omnibarRight.every(i=>!i.alwaysVisible));
  for (const [id,attr] of [['reader-mode-button','readeractive'],['picture-in-picture-button','pipactive'],['translations-button','translationsactive']]) {
    const node=nodes.get(id);
    const menu=adapter.snapshot().pageActions.items.find(i=>i.id===adapter.entries.get(node).id);
    adapter.activate('omni-unpin:'+menu.id,{},{});
    node.attrs[attr]=attr==='readeractive'?'true':'';
    assert.equal(adapter.snapshot().omnibarRight.find(i=>i.id===menu.id)?.alwaysVisible,true);
    delete node.attrs[attr];
    assert.ok(!adapter.snapshot().omnibarRight.some(i=>i.id===menu.id));
  }
  nodes.get('identity-icon-box').hidden=true;
  assert.ok(!adapter.snapshot().omnibarLeft.some(i=>i.label==='identity-icon-box'));
  nodes.set('urlbar-zoom-button',{...nodes.get('share-button'),id:'urlbar-zoom-button',attrs:{label:'120%'}});
  assert.equal(adapter.snapshot().omnibarRight.find(i=>i.text==='120%')?.alwaysVisible,true);
});

function indicator(id, classes = [], attrs = {}) {
  return {id, isConnected:true, attrs, classList:{contains:name=>classes.includes(name)},
    matches:selector=>selector.split(',').some(s=>classes.includes(s.trim().slice(1))),
    getAttribute:name=>attrs[name] || '', hasAttribute:name=>name in attrs};
}

test('native artwork covers the pinned markup inventory, including every permission and notification',()=>{
  const {adapter,nodes}=menuFixture();
  const markup=readFileSync(new URL('../upstream/firefox/browser/base/content/navigator-toolbox.inc.xhtml',import.meta.url),'utf8');
  const native=readFileSync(new URL('../native/WinUIPane/OmnibarIcons.h',import.meta.url),'utf8');
  let count=0;
  for (const match of markup.matchAll(/<image\b([^>]+)>/g)) {
    const attributes=Object.fromEntries(Array.from(match[1].matchAll(/([\w-]+)="([^"]*)"/g),m=>[m[1],m[2]]));
    const classes=(attributes.class || '').split(' ');
    if (!classes.some(c=>['blocked-permission-icon','notification-anchor-icon','sharing-icon'].includes(c))) continue;
    const node=indicator(attributes.id || '',classes,attributes);
    if (node.id==='webrtc-sharing-icon') node.attrs.sharing='camera';
    const result=adapter.nativePresentation(node,node);
    assert.ok(result.icon && native.includes(`{L"${result.icon}"`),JSON.stringify(attributes));
    if (classes.includes('blocked-permission-icon')) assert.equal(result.iconState,'blocked');
    count++;
  }
  assert.ok(count>=40,count);
  const page=markup.slice(markup.indexOf('<hbox id="page-action-buttons"'),markup.indexOf('<html:div class="search-one-offs"'));
  for (const match of page.matchAll(/<(?:hbox|toolbarbutton) id="([^"]+)"/g)) {
    if (['page-action-buttons','pageActionButton'].includes(match[1])) continue;
    const node=nodes.get(match[1]) || indicator(match[1]);
    assert.ok(adapter.nativePresentation(node,node).icon,match[1]);
  }
});

test('built-ins never request Gecko artwork; extensions retain the exact pixel payload',()=>{
  const {adapter,nodes}=menuFixture(); const loads=[];
  adapter.loadImage=(...args)=>loads.push(args);
  adapter.window.getComputedStyle=()=>({display:'block',visibility:'visible',fill:'currentColor',color:'rgb(0,0,0)',listStyleImage:'url("test.svg")'});
  let state=adapter.snapshot();
  assert.equal(loads.length,1); // The only extension in this fixture.
  const pixels={width:1,height:1,pixels:'000000ff'};
  adapter.images.set(loads[0][0],pixels);
  state=adapter.snapshot();
  const builtin=state.omnibarRight.find(i=>i.label==='reader-mode-button');
  assert.equal(builtin.icon,'reader'); assert.equal(builtin.image,null);
  const extension=state.omnibarRight.find(i=>i.label==='extension-action');
  assert.equal(extension.icon,''); assert.equal(extension.image,pixels);
  const star=indicator('star-button',[],{starred:'true'}); nodes.set(star.id,star);
  assert.equal(adapter.snapshot().omnibarRight.at(-1).icon,'bookmark-filled');
  delete star.attrs.starred;
  assert.equal(adapter.snapshot().omnibarRight.at(-1).icon,'bookmark');
});

test('permission wrapper carries the actual first child state; sharing and protection states stay distinct',()=>{
  const {adapter,nodes}=menuFixture();
  const camera=indicator('', ['blocked-permission-icon'], {'data-permission-id':'camera'});
  assert.deepEqual({...adapter.nativePresentation(nodes.get('identity-permission-box'),camera)}, {icon:'camera',iconState:'blocked'});
  const sharing=indicator('webrtc-sharing-icon',['sharing-icon'],{sharing:'microphone'});
  assert.deepEqual({...adapter.nativePresentation(sharing,sharing)}, {icon:'microphone',iconState:'recording'});
  for (const type of ['camera','microphone','screen']) {
    sharing.attrs.sharing=type;
    assert.equal(adapter.nativePresentation(sharing,sharing).iconState,'recording');
  }
  sharing.attrs.sharing='microphone';
  const allowed=indicator('webRTC-shareMicrophone-notification-icon',['notification-anchor-icon']);
  assert.equal(adapter.nativePresentation(allowed,allowed).iconState,'normal');
  sharing.attrs.paused='';
  assert.deepEqual({...adapter.nativePresentation(sharing,sharing)}, {icon:'microphone',iconState:'paused'});
  sharing.attrs.sharing='screen';
  assert.equal(adapter.nativePresentation(sharing,sharing).icon,'screen');
  const protection=indicator('tracking-protection-icon-box',[],{hasException:'true'});
  nodes.set(protection.id,protection);
  assert.equal(adapter.nativePresentation(nodes.get('tracking-protection-icon-container'),protection).iconState,'blocked');
  delete protection.attrs.hasException; protection.attrs.active='true';
  assert.equal(adapter.nativePresentation(nodes.get('tracking-protection-icon-container'),protection).iconState,'active');
  nodes.get('trust-icon-container').classes.add('breached');
  assert.equal(adapter.nativePresentation(nodes.get('trust-icon-container'),null).icon,'danger');
});
