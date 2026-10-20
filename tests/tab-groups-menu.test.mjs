import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
function fixture(){
  const calls=[],groups=[],saved=[],prefs=new Map([['browser.tabs.groups.alternateMenu',true]]),source={};
  const metrics={METRIC_SOURCE:{TAB_OVERFLOW_MENU:'overflow',TAB_GROUP_MENU:'group'},userTriggeredContext:v=>v};
  const sharing={isEnabled:false,handleShareTabGroup:g=>calls.push(['share',g])};
  const window={document:{},private:false,PrivateBrowsingUtils:{isWindowPrivate:w=>w.private},BROWSER_NEW_TAB_URL:'about:newtab',
    MutationObserver:class {observe(){}disconnect(){}},Localization:class {formatMessagesSync([{id,args}]){const value=args?args.tabGroupName+' — Closed':id;return [{value,attributes:[{name:'label',value},{name:'title',value}]}];}},
    gBrowser:{tabContainer:{addEventListener(){},removeEventListener(){}},selectedTab:{},selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}},openTabs:[{},{},{}],tabs:[{},{},{}],getAllTabGroups:()=>groups,
      addTrustedTab:url=>{calls.push(['tab',url]);return {new:true};},addTabGroup:(...args)=>calls.push(['create',...args]),
      adoptTabGroup:(g,options)=>{calls.push(['adopt',g,options]);return g;},replaceGroupWithWindow:(...args)=>calls.push(['detach',...args]),removeTabGroup:(...args)=>calls.push(['delete',...args])},
    SessionStore:{savedGroups:saved,openSavedTabGroup:(...args)=>{calls.push(['restore',...args]);return {};},forgetSavedTabGroup:id=>calls.push(['forget',id])},
    MouseEvent:class {constructor(type,args){Object.assign(this,args);}},focus:()=>calls.push(['focus'])};
  const item={id:'widget-1',placed:true,stock:[{id:'tab-groups-button',label:'Groups',enabled:true,shown:true}]};
  const toolbar={window,active:true,navigation:true,supported:()=>true,menuAnchor:()=>({}),owner:{addressSession:{sessionId:'address'},addressTab:window.gBrowser.selectedTab,addressDocument:7,invalidateAddress(){}},
    customization:{current:{items:[item]},entries:new Map([['tab-groups-button',{id:item.id,node:source}]]),nativeOverflow:new Set(),ui:{getWidget:()=>({forWindow:()=>({node:source})})}}};
  toolbar.customization.stock={dispatch:(...args)=>{calls.push(['more',...args]);return 1;}};
  const context=vm.createContext({console,ChromeUtils:{importESModule:()=>({TabMetrics:metrics,ContentSharingUtils:sharing})},Services:{wm:{getEnumerator:()=>[window]},prefs:{getBoolPref:(k,f)=>prefs.get(k)??f,addObserver(){},removeObserver(){}},obs:{addObserver(){},removeObserver(){}}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUITabGroupsMenu.sys.mjs',import.meta.url),'utf8').replace('export class WinUITabGroupsMenu','this.Menu=class WinUITabGroupsMenu'),context);
  const menu=new context.Menu(toolbar);toolbar.navigationState=()=>({tabGroupsMenu:menu.snapshot()});
  const add=(name='Group',owner=window)=>{const group={id:'group-'+groups.length,name,color:'blue',collapsed:true,lastSeenActive:1,tabs:[{_tPos:0}],documentGlobal:owner,ownerDocument:owner.document,select(){calls.push(['select',this]);}};groups.push(group);return group;};
  const open=()=>menu.execute({command:'tab-groups-menu:open:1',x:10,y:10,width:32,height:32});
  const invoke=(id=menu.description.items[0]?.id)=>menu.execute({command:`tab-groups-menu:invoke:1:1:${id}`});
  return {menu,toolbar,window,calls,groups,saved,sharing,item,prefs,add,open,invoke};
}
test('empty state creates through Firefox with the original metrics context',()=>{
  const f=fixture();assert.equal(f.open(),1);assert.equal(f.menu.description.emptyTitle,'tab-groups-list-empty-header');assert.deepEqual(f.calls,[]);
  assert.equal(f.invoke(f.menu.description.create.id),1);assert.equal(f.calls[0][0],'tab');assert.equal(f.calls[1][0],'create');assert.equal(f.calls[1][2].metricsContext,'overflow');
});
test('current and other-window groups retain ordering, selection, focus and context eligibility',()=>{
  const f=fixture();const group=f.add();f.open();const row=f.menu.description.items[0];assert.equal(row.context[0].enabled,false);assert.equal(row.context[1].enabled,true);
  assert.equal(f.invoke(row.context[0].id),2);assert.equal(f.invoke(),1);assert.equal(f.invoke(row.id),2);assert.equal(f.calls[0][1],group);assert.equal(f.calls[1][0],'focus');
  const g=fixture();const owner={document:{},gBrowser:{openTabs:[{},{}],tabs:[]},focus:()=>g.calls.push(['foreign-focus'])};g.add('Other',owner);g.open();assert.equal(g.menu.description.items[0].context[0].enabled,true);g.invoke();assert.equal(g.calls.at(-1)[0],'foreign-focus');
});
test('saved groups follow closedAt order, restore with metrics and are never read in private windows',()=>{
  const f=fixture();f.saved.push({id:'old',name:'Old',closedAt:1},{id:'new',name:'New',closedAt:2});f.open();assert.equal(f.menu.description.items[0].label,'New — Closed');f.invoke();assert.equal(f.calls[0][1],'new');assert.equal(f.calls[0][3].source,'overflow');
  const g=fixture();g.window.private=true;Object.defineProperty(g.window.SessionStore,'savedGroups',{get(){throw Error('private saved-group exposure');}});g.add();assert.equal(g.open(),1);assert.equal(g.menu.description.items.length,1);
});
test('context commands route to Firefox owners and respect sole-group and sharing gates',()=>{
  for(const op of ['moveHere','moveNew','share','delete']){
    const f=fixture();f.sharing.isEnabled=true;const owner=op==='moveHere'?{document:{},gBrowser:{openTabs:[{},{}],tabs:[]},focus(){}}:f.window;f.add('Group',owner);f.open();
    const id=[...f.menu.actions].find(([,action])=>action.operation===op)?.[0];assert.ok(id);assert.equal(f.invoke(id),1);
    assert.equal(f.calls[0][0],{moveHere:'adopt',moveNew:'detach',share:'share',delete:'delete'}[op]);
  }
  const f=fixture();f.add();f.window.gBrowser.openTabs=[{}];f.open();assert.equal(f.menu.description.items[0].context[1].enabled,false);assert.ok(![...f.menu.actions.values()].some(a=>a.operation==='share'));
});
test('rename, same-position tab replacement, source/context/policy changes revoke actions',()=>{
  for(const change of [f=>f.groups[0].name='Changed',f=>f.groups[0].tabs=[{_tPos:0}],f=>f.groups[0]={...f.groups[0]},f=>f.item.placed=false,
    f=>f.toolbar.owner.addressDocument++,f=>f.toolbar.customization.nativeOverflow.add('tab-groups-button'),f=>f.toolbar.customization.editorOpen=true,
    f=>f.prefs.set('browser.tabs.groups.alternateMenu',false),f=>f.sharing.isEnabled=true]){
    const f=fixture();f.add();f.open();const id=f.menu.description.items[0].id;change(f);assert.equal(f.invoke(id),2);assert.deepEqual(f.calls,[]);
  }
});
test('bounded inventory keeps all excess groups reachable through Firefox at the invocation anchor',()=>{
  const f=fixture();for(let i=0;i<45;i++)f.add('Long '+i);f.open();assert.equal(f.menu.description.items.length,40);assert.ok(JSON.stringify(f.menu.description).length<65536);
  assert.equal(f.invoke(f.menu.description.more.id),1);assert.equal(f.calls[0][0],'more');
});

test('active timestamps advance without revoking stable inventory; reordered groups revoke it',()=>{
  const f=fixture();const group=f.add();f.add('Second');let tick=0;
  Object.defineProperty(group,'lastSeenActive',{get:()=>++tick});f.open();
  assert.equal(f.menu.snapshot().session,1);assert.equal(f.menu.snapshot().session,1);
  f.groups.reverse();assert.equal(f.invoke(1),2);assert.equal(f.menu.session,0);
});
test('malformed/stale requests and teardown cannot initialize or resurrect a menu',()=>{
  const f=fixture();for(const command of ['tab-groups-menu:open:0','tab-groups-menu:open:4294967296','tab-groups-menu:open:1:1:1','tab-groups-menu:invoke:1:1:1'])assert.equal(f.menu.execute({command}),2);
  f.add();f.open();f.menu.close();assert.equal(f.invoke(1),2);assert.equal(f.open(),2);
});
