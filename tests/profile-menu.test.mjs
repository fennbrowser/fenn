import {readFileSync} from "node:fs";
import vm from "node:vm";
import test from "node:test";
import assert from "node:assert/strict";
function fixture() {
  const calls=[],observers=new Map(),prefs=new Map(),current={id:10,name:"Current",copyProfile:()=>calls.push("copy")};
  const profiles={isEnabled:true,initialized:true,currentProfile:current,list:[current,{id:20,name:"Other"}],
    async getAllProfiles(){calls.push("read");return this.list;},async getProfile(id){return this.list.find(p=>p.id===id);},
    launchInstance:p=>calls.push(["launch",p.id]),maybeSetupDataStore:async()=>{calls.push("setup");}};
  const UIState={STATUS_SIGNED_IN:"signed_in",STATUS_NOT_VERIFIED:"not_verified",STATUS_NOT_CONFIGURED:"not_configured",
    state:{status:"not_configured",syncEnabled:false},get(){return this.state;}};
  const source={id:"fxa-toolbar-menu-button"};
  const window={document:{getElementById:id=>id==="fxa-toolbar-menu-button"?source:null},gDialogBox:{isOpen:false},
    gProfiles:{createNewProfile:s=>calls.push(["create",s]),manageProfiles:()=>calls.push("manage")},
    gSync:{clickFxAMenuHeaderButton:s=>calls.push(["account",s]),doSyncFromFxaMenu:s=>calls.push(["sync",s]),openPrefsFromFxaMenu:(k,s)=>calls.push(["settings",k,s]),disconnect:()=>calls.push("signout")},
    openTrustedLinkIn:(...args)=>calls.push(args),MouseEvent:class {},
    Localization:class {formatMessagesSync(messages){return messages.map(m=>({value:m.id,attributes:["label"].map(name=>({name,value:m.id}))}));}}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,customization:{nativeOverflow:new Set(),editorOpen:false},
    owner:{addressSession:{sessionId:"address"},addressTab:{},addressDocument:1,invalidateAddress(){}},navigationState:()=>({profile:{enabled:true}}),menuAnchor:()=>source,
    openMenu:(id,bounds)=>{calls.push(["original",id,bounds]);return 1;}};
  window.gBrowser={selectedTab:toolbar.owner.addressTab,selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:1}}}};
  let allowed=true;
  const Services={policies:{isAllowed:()=>allowed},prefs:{getBoolPref:(k,d)=>prefs.get(k)??d,addObserver(){},removeObserver(){}},
    obs:{addObserver:(fn,k)=>observers.set(k,fn),removeObserver:(_fn,k)=>observers.delete(k)}};
  const context=vm.createContext({console,Services,ChromeUtils:{importESModule:path=>path.includes("UIState")?{UIState}:{SelectableProfileService:profiles}}});
  vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIProfileMenu.sys.mjs",import.meta.url),"utf8").replace("export class WinUIProfileMenu","this.Menu=class WinUIProfileMenu"),context);
  const menu=new context.Menu(toolbar);
  const invoke=key=>{const row=menu.snapshot().items.find(i=>i.key===key);return menu.execute({command:`profile-menu:invoke:${menu.session}:1:${row.id}`,modifiers:0});};
  return {menu,toolbar,window,profiles,UIState,calls,observers,prefs,invoke,source,setAllowed:v=>allowed=v};
}
const settle=()=>new Promise(resolve=>setTimeout(resolve,0));
test("closed menu is lazy, bounds profiles, keeps current checked and publishes no service IDs",async()=>{
  const f=fixture();assert.equal(f.menu.snapshot().enabled,true);assert.deepEqual(f.calls,[]);
  f.profiles.list=Array.from({length:20},(_,i)=>({id:i+10,name:"Profile "+i}));await f.menu.open(1,{});
  const state=f.menu.snapshot(),rows=state.items.filter(i=>i.key?.startsWith("profile-"));assert.equal(rows.length,8);
  assert.equal(rows[0].checked,true);assert.equal(rows[0].enabled,false);assert.ok(rows.every(i=>!("profileId" in i)));
  assert.ok(state.items.some(i=>i.key==="manage"));assert.ok(!f.calls.includes("setup"));
  f.menu.close();f.menu.close();assert.equal(f.observers.size,0);assert.equal(f.menu.actions.size,0);assert.equal(f.menu.snapshot().closed,1);
});
test("profile policy and absent store preserve Firefox feature gates",async()=>{
  const f=fixture();f.profiles.initialized=false;await f.menu.open(1,{});assert.ok(!f.calls.includes("read"));assert.ok(f.menu.snapshot().items.some(i=>i.key==="create"));
  f.setAllowed(false);await f.menu.open(2,{});assert.ok(!f.menu.snapshot().items.some(i=>["create","manage","edit"].includes(i.key)));
  f.prefs.set("identity.fxaccounts.enabled",false);await f.menu.open(3,{});assert.deepEqual(Array.from(f.menu.snapshot().items,i=>i.key),["more"]);
});
test("account commands reflect Firefox state and dispatch original controllers once",async()=>{
  const f=fixture();await f.menu.open(1,{});assert.equal(f.invoke("account"),1);assert.equal(f.calls.at(-1)[0],"account");assert.equal(f.calls.at(-1)[1],f.source);
  const old="profile-menu:invoke:1:1:1";assert.equal(f.menu.execute({command:old}),2);
  f.UIState.state={status:"signed_in",syncEnabled:true,email:"user@example.test"};
  for(const key of ["account","sync","settings","signout"]){await f.menu.open(f.menu.closedSession+1,{});assert.equal(f.invoke(key),1);}
  assert.ok(f.calls.includes("signout"));assert.ok(f.calls.some(c=>Array.isArray(c)&&c[0]==="settings"&&c[1]==="sync_settings"));
});
test("live account changes cannot execute a previously described command",async()=>{
  const f=fixture();await f.menu.open(1,{});const id=f.menu.snapshot().items.find(i=>i.key==="account").id;f.UIState.state={status:"signed_in",syncEnabled:true};assert.equal(f.menu.execute({command:`profile-menu:invoke:1:1:${id}`}),2);
});
test("profile launch resolves the exact live profile and consumes the opening",async()=>{
  const f=fixture();await f.menu.open(1,{});assert.equal(f.invoke("profile-1"),1);await settle();assert.deepEqual(f.calls.at(-1),["launch",20]);assert.equal(f.menu.session,0);
});
test("removed profiles and context changes during launch lookup cannot launch",async()=>{
  const f=fixture();await f.menu.open(1,{});f.profiles.list.pop();assert.equal(f.invoke("profile-1"),1);await settle();assert.ok(!f.calls.some(c=>Array.isArray(c)&&c[0]==="launch"));
  f.profiles.list.push({id:20,name:"Other"});let resolve;f.profiles.getProfile=()=>new Promise(r=>resolve=r);await f.menu.open(2,{});f.invoke("profile-1");f.toolbar.owner.addressDocument++;resolve({id:20});await settle();assert.ok(!f.calls.some(c=>Array.isArray(c)&&c[0]==="launch"));
});
test("copy and edit use the described current profile and original APIs",async()=>{
  const f=fixture();await f.menu.open(1,{});assert.equal(f.invoke("copy"),1);await settle();assert.ok(f.calls.includes("copy"));
  await f.menu.open(2,{});assert.equal(f.invoke("edit"),1);assert.deepEqual(f.calls.at(-1),["about:editprofile","tab"]);
  await f.menu.open(3,{});f.profiles.currentProfile={id:30};assert.equal(f.invoke("copy"),2);
});
test("observers, document changes and toolbar fallback revoke pending profile descriptions",async()=>{
  const f=fixture();await f.menu.open(1,{});f.observers.get("sps-profiles-updated")();assert.equal(f.menu.session,0);
  let resolve;f.profiles.getAllProfiles=()=>new Promise(r=>resolve=r);const pending=f.menu.open(2,{});f.menu.dismiss();resolve(f.profiles.list);await pending;assert.equal(f.menu.snapshot().session,undefined);
  f.profiles.getAllProfiles=async()=>f.profiles.list;await f.menu.open(3,{});f.toolbar.active=false;assert.equal(f.menu.snapshot().session,undefined);
});
test("grammar, stale openings, modal state and fallback keep native bounds scoped",async()=>{
  const f=fixture();const bounds={command:"profile-menu:open:1",x:100,y:30,width:32,height:32};assert.equal(f.menu.execute(bounds),1);await settle();
  assert.equal(f.menu.execute(bounds),2);assert.equal(f.menu.execute({command:"profile-menu:invoke:1:2:1"}),2);
  assert.equal(f.invoke("more"),1);assert.equal(f.calls.at(-1)[2],bounds);
  f.window.gDialogBox.isOpen=true;assert.equal(f.menu.execute({command:"profile-menu:open:2"}),1);await settle();assert.equal(f.menu.snapshot().session,undefined);
  for(const command of ["profile-menu:open:0","profile-menu:invoke:1:1:0","profile-menu:open:4294967296","profile-menu:dismiss:1:1:1"])assert.equal(f.menu.execute({command}),2);
});

test("sync activity updates retain the opening while semantic account transitions revoke it",async()=>{
 const f=fixture();await f.menu.open(1,{});f.UIState.state.syncing=true;
 f.observers.get("sync-ui-state:update")(null,"sync-ui-state:update");assert.equal(f.menu.session,1);
 f.UIState.state.status="signed_in";f.observers.get("sync-ui-state:update")(null,"sync-ui-state:update");assert.equal(f.menu.session,0);
});

test("a selected-tab change before owner publication cancels delayed profile launch",async()=>{
 const f=fixture();let resolve;f.profiles.getProfile=()=>new Promise(r=>resolve=r);await f.menu.open(1,{});
 assert.equal(f.invoke("profile-1"),1);f.window.gBrowser.selectedTab={};resolve({id:20});await settle();
 assert.ok(!f.calls.some(c=>Array.isArray(c)&&c[0]==="launch"));
});
