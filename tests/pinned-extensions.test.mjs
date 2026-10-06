import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import vm from "node:vm";

const context = vm.createContext({console});
vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIPinnedExtensions.sys.mjs", import.meta.url),"utf8")
  .replace("export class WinUIPinnedExtensions", "this.Pinned = class WinUIPinnedExtensions"), context);

function fixture() {
  const pinned = Object.create(context.Pinned.prototype);
  const widgets = new Map();
  const order = [];
  const actions = new Map();
  const toolbar = {menuAnchors:new Map(), menuAnchor:()=>({hasAttribute:()=>false})};
  const window = {gBrowser:{selectedTab:{}}, document:{getElementById:()=>null},
    WebExtensionPolicy:{getByID:id=>({extension:{canAccessWindow:()=>true},id})},
    gUnifiedExtensions:{browserActionFor:policy=>actions.get(policy.id)},
    MouseEvent:class {constructor(type, args) {this.type=type;Object.assign(this,args);}}};
  Object.assign(pinned,{toolbar,window,entries:new Map(),nextId:0,images:new Map(),loads:new Set(),
    events:new WeakMap(),theme:{matches:false},iconDetails:{getPreferredIcon:()=>({icon:"icon"})},
    ui:{AREA_NAVBAR:"navbar",getWidgetIdsInArea:()=>order,getWidget:id=>({forWindow:()=>({node:widgets.get(id)})})},
    loadImage:()=>{}});
  function add(id) {
    const attrs = new Map();
    const button = {dataset:{extensionid:id},isConnected:true,hidden:false,
      getAttribute:key=>attrs.get(key),hasAttribute:key=>attrs.has(key),dispatchEvent:()=>{}};
    const node = {querySelector:()=>button,hasAttribute:key=>attrs.has(key),closest:()=>null};
    const data = {badgeBackgroundColor:[1,2,3,255],badgeText:"3",title:id};
    const action = {action:{getContextData:()=>data,getTextColor:()=>[255,255,255,255]},extension:{name:id}};
    widgets.set(id,node); actions.set(id,action); order.push(id);
    return {attrs,button,data};
  }
  return {pinned,order,add,toolbar};
}

test("pinned order and stable identities follow Firefox, while native overflow revokes actions without losing identities", () => {
  const f=fixture(); const first=f.add("first"); f.add("second");
  const initial=f.pinned.snapshot();
  assert.deepEqual(Array.from(initial,p=>p.label),["first","second"]);
  f.order.reverse();
  assert.equal(f.pinned.snapshot()[1].id,initial[0].id);
  first.attrs.set("overflowedItem","true");
  assert.equal(f.pinned.snapshot().length,2);
  f.toolbar.customization={nativeOverflow:new Set(["first"])};
  assert.equal(f.pinned.activate(`extension:${initial[0].id}`,{},{}),2);
  first.attrs.delete("overflowedItem");
  f.toolbar.customization.nativeOverflow.clear();
  assert.equal(f.pinned.snapshot()[1].id,initial[0].id);
  assert.equal(f.pinned.activate(`extension:${initial[0].id}`,{},{}),1);
});

test("Firefox disabled state and title/badge updates are reflected without replacing identity", () => {
  const f=fixture();const item=f.add("extension");const id=f.pinned.snapshot()[0].id;
  item.attrs.set("disabled","true"); item.data.title="Updated"; item.data.badgeText="17";
  const state=f.pinned.snapshot()[0];
  assert.equal(state.id,id);assert.equal(state.enabled,false);assert.equal(state.label,"Updated");assert.equal(state.badge,"17");
  assert.equal(f.pinned.activate(`extension:${id}`,{},{}),2);
});

test("extension commands use the original widget with an event-scoped native popup anchor", () => {
  const f=fixture();const item=f.add("extension");const id=f.pinned.snapshot()[0].id;
  let dispatched;
  item.button.dispatchEvent=event=>{dispatched=event;assert.ok(f.pinned.events.get(event));};
  assert.equal(f.pinned.activate(`extension:${id}`,{}, {shiftKey:true}),1);
  assert.equal(dispatched.type,"command");assert.equal(dispatched.shiftKey,true);
  assert.equal(f.pinned.events.has(dispatched),false);
});

test("removal revokes anchors and bounds native publications", () => {
  const f=fixture();f.add("extension");const id=f.pinned.snapshot()[0].id;
  let removed=false;f.toolbar.menuAnchors.set(`extension:${id}`,{remove:()=>{removed=true;}});
  f.order.length=0;assert.equal(f.pinned.snapshot().length,0);assert.ok(removed);
  for(let i=0;i<40;i++)f.add(`extension-${i}`);
  assert.equal(f.pinned.snapshot().length,32);
});


test('Firefox generic extension artwork uses a themed native fallback only for its default icon',()=>{
 const f=fixture();f.add('extension');
 f.pinned.iconDetails.DEFAULT_ICON='default-icon';
 f.pinned.iconDetails.getPreferredIcon=()=>({icon:'default-icon'});
 f.pinned.loadImage=()=>{throw new Error('generic icon must not rasterize');};
 assert.equal(f.pinned.snapshot()[0].genericIcon,true);
 assert.equal(f.pinned.snapshot()[0].image,null);
 f.pinned.iconDetails.getPreferredIcon=()=>({icon:'custom-icon'});
 f.pinned.loadImage=()=>{};
 assert.equal(f.pinned.snapshot()[0].genericIcon,false);
});
