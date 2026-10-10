import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import {readFileSync} from "node:fs";
import {readActionStamp,actionStampMatches} from "../upstream/firefox/browser/base/content/WinUIActionStamp.sys.mjs";
const source=readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIHistoryMenu.sys.mjs",import.meta.url),"utf8").replace(/^import .*;$/m,"").replace("export class WinUIHistoryMenu","this.HistoryMenu=class WinUIHistoryMenu");
function fixture(){
 const calls=[];let enabled=true,options;
 const root={containerOpen:false,childCount:1,getChild:()=>node};const node={title:"Example",uri:"https://example.test",parent:root};
 const window={document:{l10n:{formatMessages:async()=>["Search","Clear","Manage"].map(value=>({value}))}},MouseEvent:class {constructor(type,args){this.type=type;Object.assign(this,args);}},
 PlacesUIUtils:{openNodeIn:(...args)=>calls.push(["visit",...args])},PlacesCommandHook:{searchHistory:()=>calls.push(["search"]),showPlacesOrganizer:value=>calls.push(["manage",value])},Sanitizer:{showUI:()=>calls.push(["clear"])}};
 const toolbar={window,active:true,owner:{invalidateAddress(){},localization:()=>({formatValueSync:id=>id})},menuAnchor:()=>({}),openMenu:()=>calls.push(["original"])};
 const context=vm.createContext({console:{error(){}},Services:{prefs:{getBoolPref:()=>enabled,addObserver(){},removeObserver(){}}},Ci:{nsINavHistoryQueryOptions:{QUERY_TYPE_HISTORY:0,SORT_BY_DATE_DESCENDING:4}},ChromeUtils:{importESModule:()=>({BrowserUtils:{whereToOpenLink:event=>{calls.push(["modifiers",event]);return "tab";}},PlacesUtils:{history:{getNewQuery:()=>({}),getNewQueryOptions:()=>options={},executeQuery:()=>({root})}}})}});
 Object.assign(context,{readActionStamp,actionStampMatches});
 vm.runInContext(source,context);const menu=new context.HistoryMenu(toolbar);
 return {menu,window,toolbar,root,node,calls,disable:()=>enabled=false,options:()=>options};
}
const settle=()=>new Promise(resolve=>setImmediate(resolve));
test("History root retains the Firefox query and executes opaque commands once",async()=>{
 const f=fixture();assert.equal(f.menu.execute({command:"history-menu:open:1"}),1);await settle();
 assert.equal(f.options().maxResults,42);assert.equal(f.options().excludeQueries,true);
 assert.deepEqual(Array.from(f.menu.description.items.filter(i=>i.label),i=>i.label),["Search","Clear","Example","Manage","winui-history-open-firefox"]);
 const action={command:"history-menu:invoke:1:1:3",modifiers:5};assert.equal(f.menu.execute(action),1);
 assert.equal(f.calls[0][0],"modifiers");assert.equal(f.calls[0][1].ctrlKey,true);assert.equal(f.calls[0][1].shiftKey,true);
 assert.equal(f.calls[1][0],"visit");assert.equal(f.calls[1][2],"tab");assert.equal(f.calls[1][3].ownerWindow,f.window);
 assert.equal(f.root.containerOpen,false);assert.equal(f.menu.execute(action),2);
});
test("delayed descriptions cannot revive a dismissed or superseded opening",async()=>{
 const f=fixture();let finish;f.window.document.l10n.formatMessages=()=>new Promise(r=>finish=r);
 f.menu.execute({command:"history-menu:open:1"});f.menu.execute({command:"history-menu:dismiss:1"});finish([]);await settle();
 assert.equal(f.menu.description,null);assert.equal(f.menu.snapshot().closed,1);assert.equal(f.menu.execute({command:"history-menu:open:1"}),2);
});
test("deleted entries, unknown IDs/revisions and disabled preferences cannot execute",async()=>{
 const f=fixture();f.menu.execute({command:"history-menu:open:2"});await settle();
 for(const command of ["history-menu:invoke:2:2:3","history-menu:invoke:2:1:999","history-menu:invoke:1:1:3"])assert.equal(f.menu.execute({command}),2);
 f.node.parent=null;assert.equal(f.menu.execute({command:"history-menu:invoke:2:1:3"}),2);assert.equal(f.calls.length,0);
 f.disable();assert.equal(f.menu.execute({command:"history-menu:open:3"}),2);
});
test("query/localization failure offers an explicit original menu without executing it",async()=>{
 const f=fixture();f.window.document.l10n.formatMessages=async()=>{throw Error("unavailable");};
 f.menu.execute({command:"history-menu:open:1"});await settle();assert.equal(f.calls.length,0);
 assert.equal(f.menu.description.items.length,1);assert.equal(f.menu.execute({command:"history-menu:invoke:1:1:1",modifiers:0}),1);assert.equal(f.calls[0][0],"original");
});

test("middle click uses Firefox's destination policy and cannot invoke fixed commands",async()=>{
 const f=fixture();f.menu.execute({command:"history-menu:open:1"});await settle();
 assert.equal(f.menu.execute({command:"history-menu:middle:1:1:1",modifiers:0}),2);
 assert.equal(f.menu.execute({command:"history-menu:middle:1:1:3",modifiers:0}),1);
 assert.equal(f.calls[0][1].button,1);
});

test("customization and navigation context changes close retained query results",async()=>{
 for(const change of [f=>f.toolbar.customization={editorOpen:true},f=>f.toolbar.owner.addressTab={},f=>f.toolbar.owner.addressDocument=123]){
  const f=fixture();f.menu.execute({command:"history-menu:open:1"});await settle();assert.equal(f.root.containerOpen,true);
  change(f);assert.equal(f.menu.snapshot().session,undefined);assert.equal(f.root.containerOpen,false);
 }
});

test("shared stamps retain History grammar, opening lifetime and bounded revisions",async()=>{
 const f=fixture();
 for(const command of ["history-menu:open:01","history-menu:open:4294967296","history-menu:open:9007199254740992"])assert.equal(f.menu.execute({command}),2);
 assert.equal(f.menu.execute({command:"history-menu:open:1"}),1);await settle();
 for(const command of ["history-menu:invoke:2:1:1","history-menu:invoke:1:2:1","history-menu:invoke:1:4294967296:1","history-menu:invoke:1:01:1"])assert.equal(f.menu.execute({command}),2);
 assert.equal(f.calls.length,0);assert.equal(f.root.containerOpen,true);
 assert.equal(f.menu.execute({command:"history-menu:invoke:1:1:1"}),1);
 assert.deepEqual(f.calls,[["search"]]);
});
