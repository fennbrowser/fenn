import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console});
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
