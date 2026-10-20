import {readFileSync} from "node:fs";
import vm from "node:vm";
import test from "node:test";
import assert from "node:assert/strict";
function fixture(){
  const calls=[],lists=new Map(),entries=new Map(),items=[],stockParts=new Map();
  const node=id=>({id,hidden:false,collapsed:false,localName:"toolbarbutton",querySelector(){return this.button;}});
  lists.set("widget-overflow-list",{children:[]});lists.set("widget-overflow-fixed-list",{children:[]});
  const window={document:{getElementById:id=>lists.get(id)},gBrowser:{selectedTab:{},selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}},
    BrowserCommands:{home:e=>calls.push(["home",e]),openTab:e=>calls.push(["newtab",e])},MouseEvent:class {constructor(type,args){Object.assign(this,args);}},
    WebExtensionPolicy:{getByID:id=>({extension:{canAccessWindow:()=>true,name:id}})},gUnifiedExtensions:{browserActionFor:()=>({})}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,owner:{addressSession:{sessionId:"address"},addressTab:window.gBrowser.selectedTab,addressDocument:7,
    invalidateAddress(){},localization:()=>({formatValueSync:()=>"More tools…"})},
    menuAnchor:()=>({}),openMenu:(...args)=>{calls.push(["original",...args]);return 1;},navigationCommands:[{id:"home",method:"home"},{id:"history",menu:true}],
    pinned:{paletteEntries:new Map(),activateAddon:(...args)=>{calls.push(["extension",...args]);return 1;}},
    customization:{current:{items},entries,nativeOverflow:new Set(),stock:{eligible:()=>true,snapshot:id=>stockParts.get(id),dispatch:(...args)=>{calls.push(["stock",...args]);return 1;}}}};
  const navigation={overflow:{enabled:true,label:"More tools"},home:{enabled:true,label:"Home"},history:{enabled:true,label:"History"}};
  const context=vm.createContext({console});
  vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIOverflowMenu.sys.mjs",import.meta.url),"utf8").replace("export class WinUIOverflowMenu","this.Menu=class WinUIOverflowMenu"),context);
  const menu=new context.Menu(toolbar);toolbar.navigationState=()=>{navigation.overflowMenu=menu.snapshot(navigation);return navigation;};toolbar.navigationState();
  let next=0;
  const add=(id,kind="stock",fixed=false)=>{
    const n=node(id),item={id:"widget-"+(++next),kind:"button",control:kind,extension:kind==="extension",...(kind==="stock"?{stock:[]}: {})};
    entries.set(id,{id:item.id,node:n,widget:id});items.push(item);
    lists.get(fixed?"widget-overflow-fixed-list":"widget-overflow-list").children.push(n);
    if(!fixed)toolbar.customization.nativeOverflow.add(id);
    stockParts.set(id,[{id,label:id,shown:true,enabled:true,checked:false,checkable:false}]);
    return n;
  };
  const open=async(session=1)=>{toolbar.navigationState();await menu.open(session,{x:100,y:40,width:32,height:32,keyboard:false});return menu.snapshot(navigation);};
  const invoke=(key,extra={})=>menu.execute({command:`overflow-menu:invoke:${menu.session}:1:${menu.description.items.find(i=>i.key===key).id}`,modifiers:0,...extra});
  return {menu,toolbar,window,navigation,items,lists,entries,stockParts,calls,add,open,invoke};
}
test("uses Firefox list order, stock group parts, fixed placement and disabled/checkable states",async()=>{
  const f=fixture();f.add("zoom-controls");f.stockParts.set("zoom-controls",["out","reset","in"].map(id=>({id,label:id,shown:true,enabled:id!=="out",checked:id==="reset",checkable:id==="reset"})));
  f.add("home-button","home",true);const state=await f.open();
  assert.deepEqual(Array.from(state.items.filter(r=>!r.separator),r=>r.label),["out","reset","in","Home","More tools…"]);
  assert.equal(state.items[0].enabled,false);assert.equal(state.items[1].checked,true);assert.equal(state.items[1].checkable,true);
  assert.equal(f.invoke("item-1"),2);assert.equal(f.invoke("item-2"),1);assert.equal(f.calls.at(-1)[0],"stock");
});
test("ordinary automatic overflow excludes represented items not chosen by the native planner",async()=>{
  const f=fixture();f.add("home-button","home");f.toolbar.customization.nativeOverflow.clear();
  assert.equal((await f.open()).items.length,1);assert.equal(f.invoke("more"),1);assert.equal(f.calls.at(-1)[1],"overflow");
});
test("destination handoff uses the current native activation rectangle after a window move",async()=>{
  const f=fixture();await f.open();assert.equal(f.invoke("more",{x:900,y:200,width:32,height:32,keyboard:true}),1);
  assert.equal(f.calls.at(-1)[2].x,900);assert.equal(f.calls.at(-1)[2].keyboard,true);
});
test("navigation entries retain Firefox's current Stop versus Reload command",async()=>{
  const f=fixture();f.add("stop-reload-button","reload",true);f.navigation.reload={enabled:true,label:"Stop",stopping:true};
  f.toolbar.navigationCommands.push({id:"reload",method:"reloadOrDuplicate"},{id:"stop",method:"stop"});
  f.window.BrowserCommands.stop=()=>f.calls.push("stop");f.window.BrowserCommands.reloadOrDuplicate=()=>f.calls.push("reload");
  await f.open();assert.equal(f.invoke("item-1"),1);assert.deepEqual(f.calls,["stop"]);
});
test("visibility, feature gating, unsupported widgets and bounded lists keep original access",async()=>{
  const f=fixture();f.add("hidden").hidden=true;f.add("gated");f.toolbar.customization.stock.eligible=()=>false;
  assert.equal((await f.open()).items.length,1);f.menu.dismiss();f.toolbar.customization.stock.eligible=()=>true;
  for(let i=0;i<80;i++)f.add("stock-"+i,"stock",true);
  const state=await f.open(2);assert.equal(state.items.filter(i=>i.id && i.key!=="more").length,60);assert.equal(state.items.at(-1).key,"more");
});
test("extension commands use original live browser-action gateway and event modifiers",async()=>{
  const f=fixture(),node=f.add("addon","extension",true);
  node.button={dataset:{extensionid:"test@addon"},getAttribute:()=>"An add-on",hasAttribute:()=>false};f.toolbar.pinned.paletteEntries.set("addon",{});
  await f.open();assert.equal(f.invoke("item-1",{modifiers:5,keyboard:true}),1);
  const call=f.calls.at(-1);assert.equal(call[1],"test@addon");assert.equal(call[2].keyboard,true);assert.equal(call[3].ctrlKey,true);assert.equal(call[3].shiftKey,true);
  assert.equal(f.menu.execute({command:"overflow-menu:invoke:1:1:1"}),2);
});
test("membership, command and document changes revoke the opening before activation",async()=>{
  for(const change of [f=>f.lists.get("widget-overflow-list").children.splice(0),f=>f.stockParts.get("find")[0].enabled=false,
    f=>f.window.gBrowser.selectedTab={},f=>f.toolbar.customization.editorOpen=true,f=>f.toolbar.active=false,f=>f.navigation.overflow.enabled=false]){
    const f=fixture();f.add("find");await f.open();change(f);
    assert.equal(f.menu.execute({command:"overflow-menu:invoke:1:1:1",modifiers:0}),2);assert.equal(f.calls.length,0);assert.equal(f.menu.session,0);
  }
});
test("replacement nodes with the same public description cannot inherit an old command",async()=>{
  const f=fixture();f.add("find");await f.open();const list=f.lists.get("widget-overflow-list");list.children[0]={...list.children[0]};
  assert.equal(f.menu.execute({command:"overflow-menu:invoke:1:1:1",modifiers:0}),2);assert.equal(f.calls.length,0);
});
test("address edit parts reuse the native clipboard contract with the exact selection",async()=>{
  const f=fixture();f.add("edit-controls");f.stockParts.set("edit-controls",[{id:"copy-button",label:"Copy",shown:true,enabled:true,address:true}]);
  f.toolbar.owner.addressSession.clipboard=request=>{f.calls.push(["clipboard",request]);return 1;};
  assert.equal(f.menu.snapshot(f.navigation).preserveAddress,true);
  await f.open();const editor={version:1,session:"address",editId:7,text:"alpha beta",selectionStart:0,selectionEnd:5,composing:false};
  assert.equal(f.invoke("item-1",{editor}),1);assert.equal(f.calls[0][0],"clipboard");assert.deepEqual(JSON.parse(JSON.stringify(f.calls[0][1])),{...editor,clipboard:"cmd_copy"});
});
test("opening synchronizes native selection before Firefox edit availability, but stale requests cannot mutate it",()=>{
  const f=fixture();f.add("find");let count=0;f.toolbar.owner.addressSession.syncNativeSelection=()=>{count++;return 1;};
  assert.equal(f.menu.execute({command:"overflow-menu:open:1",editor:{selectionStart:0,selectionEnd:5}}),1);assert.equal(count,1);
  assert.equal(f.menu.execute({command:"overflow-menu:open:1",editor:{}}),2);assert.equal(count,1);
});
test("opening and activation refresh Firefox page edit controllers even while its retained panel is closed",()=>{
  const f=fixture();f.add("find");let forced=0;
  f.window.goUpdateGlobalEditMenuItems=force=>{assert.equal(force,true);if(++forced===2)f.stockParts.get("find")[0].enabled=false;};
  assert.equal(f.menu.execute({command:"overflow-menu:open:1"}),1);assert.equal(f.menu.description.items[0].enabled,true);
  assert.equal(f.invoke("item-1"),2);assert.equal(forced,2);assert.deepEqual(f.calls,[]);
});
test("late retained-panel close, stale openings and malformed grammar cannot resurrect a menu",async()=>{
  const f=fixture();let hidden;f.lists.set("widget-overflow",{state:"hiding",addEventListener:(_name,fn)=>hidden=fn});
  const pending=f.open();f.menu.dismiss();hidden();await pending;assert.equal(f.menu.snapshot(f.navigation).session,undefined);
  for(const command of ["overflow-menu:open:0","overflow-menu:open:4294967296","overflow-menu:dismiss:1:1:1","overflow-menu:invoke:1:1:0"])assert.equal(f.menu.execute({command}),2);
  f.menu.close();f.menu.close();assert.equal(f.menu.execute({command:"overflow-menu:open:2"}),2);
});

test("publications during retained-panel dismissal preserve a pending opening until its description exists",async()=>{
  const f=fixture();f.add("find");let hidden;f.lists.set("widget-overflow",{state:"hiding",addEventListener:(_name,fn)=>hidden=fn});
  const pending=f.open();f.toolbar.navigationState();assert.equal(f.menu.session,1);assert.ok(!f.menu.description);
  hidden();await pending;assert.equal(f.menu.session,1);assert.equal(f.menu.description.items[0].label,"find");
});
