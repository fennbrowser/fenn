import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

// Run the real TabsPanel row builder and command handlers, including its
// collapsed-group rules. The fixture only substitutes DOM and browser services.
function fixture(){
  const calls=[],publications=[],prefs=new Map([['privacy.userContext.enabled',true]]);
  let document,window,adapter;
  class Node {
    constructor(name='toolbaritem'){this.name=name;this.children=[];this.attrs=new Map();this.ownerDocument=document;this.parentNode=null;this.dataset={};this.style={setProperty(){}};const values=new Set();this.classList={add:(...v)=>v.forEach(x=>values.add(x)),contains:v=>values.has(v),forEach:f=>values.forEach(f),remove:v=>values.delete(v)};}
    setAttribute(k,v){this.attrs.set(k,String(v));if(k==='class')this.classList.add(...String(v).split(' '));}
    hasAttribute(k){return this.attrs.has(k);}
    getAttribute(k){return this.attrs.get(k) || '';}
    removeAttribute(k){this.attrs.delete(k);}
    appendChild(n){if(n.name==='fragment'){for(const child of [...n.children])this.appendChild(child);return n;}n.remove();this.children.push(n);n.parentNode=this;return n;}
    append(...nodes){for(const node of nodes)this.appendChild(node);}
    remove(){if(this.parentNode){const p=this.parentNode;p.children.splice(p.children.indexOf(this),1);this.parentNode=null;}}
    get firstElementChild(){return this.children[0];}
    get documentGlobal(){return window;}
    closest(selector){return selector==='toolbaritem'?(this.name==='toolbaritem'?this:this.parentNode?.closest(selector)):null;}
    querySelector(selector){return this.querySelectorAll(selector)[0] || null;}
    querySelectorAll(selector){const all=this.children.flatMap(c=>[c,...c.querySelectorAll('*')]);return all.filter(n=>selector==='*' || selector.includes('toolbaritem') && n.name==='toolbaritem' || selector.startsWith('.') && n.classList.contains(selector.slice(1)));}
    addEventListener(){}removeEventListener(){}
    doCommand(){calls.push(['container',this.getAttribute('command'),this.getAttribute('data-usercontextid'),this.getAttribute('data-container-entrypoint')]);}
  }
  const metrics={METRIC_SOURCE:{TAB_OVERFLOW_MENU:'overflow'},userTriggeredContext:v=>({source:v})};
  document={addEventListener(){},removeEventListener(){},documentElement:new Node('root'),fullscreenElement:null,createXULElement:n=>new Node(n),createDocumentFragment:()=>new Node('fragment'),l10n:{setAttributes(){},formatValues:async()=>['Default group']}};
  const browser={tabs:[],tabGroups:[],tabContainer:{verticalMode:false,addEventListener(){},removeEventListener(){},_handleTabSelect(){calls.push(['active']);}},TabMetrics:metrics,
    getIcon:()=>"",getTabTooltip:t=>t.label,getAllDuplicateTabsToClose:()=>[],getAllTabGroups:()=>browser.tabGroups,
    setSelectedTab(t,m){calls.push(['select',t,m]);browser.selectedTab=t;},
    removeTab(t,m){calls.push(['close',t,m]);t.isConnected=false;browser.tabs.splice(browser.tabs.indexOf(t),1);},
    removeAllDuplicateTabs(){calls.push(['duplicates']);},isTabPendingPermitUnload:t=>!!t.unload,
    moveTabBefore:(t,to,m)=>calls.push(['before',t,to,m]),moveTabAfter:(t,to,m)=>calls.push(['after',t,to,m]),
    adoptTab:(t,index)=>{calls.push(['adopt',t,index]);return t;},adoptTabGroup:g=>g};
  Object.defineProperty(browser,'openTabs',{get:()=>browser.tabs});
  document.defaultView=window={document,gBrowser:browser,gDialogBox:{isOpen:false},PrivateBrowsingUtils:{isWindowPrivate:w=>!!w.private},
    ContextualIdentityService:{getPublicIdentityFromId:id=>id?{name:'Work'}:null,getUserContextLabel:id=>'Container '+id,getPublicIdentities:()=>[{userContextId:1}]},
    Localization:class{formatMessagesSync([{id,args}]){return [{value:args?.tabGroupName || id,attributes:['label','title','tooltiptext'].map(name=>({name,value:args?.tabGroupName || id}))}];}},
    SessionStore:{savedGroups:[],openSavedTabGroup:(...a)=>calls.push(['restore',...a])},
    windowUtils:{syncWinUITabMenu:s=>publications.push(JSON.parse(s)),setHandlingUserInput:()=>({destruct(){}})},
    openNewUserContextTab:({target})=>calls.push(['container','Browser:NewUserContextTab',target.getAttribute('data-usercontextid'),target.getAttribute('data-container-entrypoint')]),FirefoxViewHandler:{tab:{} ,openTab:v=>calls.push(['view',v])}};
  const context=vm.createContext({console,Glean:{browserUiInteraction:{allTabsPanelEntrypoint:{'alltabs-button':{add(){}}},listAllTabsAction:{search_tabs:{add(){}},close_all_duplicates:{add(){}}}}},Services:{prefs:{getBoolPref:(k,d)=>prefs.get(k)??d,addObserver(){},removeObserver(){}},obs:{addObserver(){},removeObserver(){}}},
    XPCOMUtils:{declareLazy:()=>({TabMetrics:metrics,PanelMultiView:{hidePopup(){}}})},ChromeUtils:{importESModule:()=>({ContentSharingUtils:{isEnabled:false}})}});
  let source=readFileSync(new URL('../upstream/firefox/browser/components/tabbrowser/TabsList.sys.mjs',import.meta.url),'utf8');
  vm.runInContext(source.replace(/^import .*;\r?\n/m,'').replace('export class TabsPanel','this.TabsPanel=class TabsPanel'),context);
  const controller=(hidden=false,audio=false)=>new context.TabsPanel({view:new Node('panelview'),containerNode:new Node('vbox'),dropIndicator:hidden?null:new Node('indicator'),onlyHiddenTabs:hidden,filterFn:t=>audio?t.soundPlaying || t.muted:hidden?t!==window.FirefoxViewHandler.tab:!t.hidden});
  window.gTabsPanel={allTabsPanel:controller(),hiddenTabsPopup:controller(true),hiddenAudioTabsPopup:controller(true,true),init(){},hideAllTabsPanel(){},hasHiddenTabsExcludingFxView:()=>browser.tabs.some(t=>t.hidden),containerTabsView:new Node('view'),searchTabs:()=>calls.push(['search'])};
  window.gTabsPanel.hiddenAudioTabs=window.gTabsPanel.hiddenAudioTabsPopup.containerNode;
  const owner={faviconCache:new Map(),loadTabFavicon(){},attached:true,tabIds:new Map(),nextTabId:1,schedule(){},stripSession:{ids:new Map(),targets:new Map(),windowDragSession:9},
    resolveStripTab:id=>browser.tabs.find(t=>owner.tabIds.get(t)===id && t.isConnected && !t.hidden && (!t.group?.collapsed || t===browser.selectedTab)),stripSourceWindow:()=>window};
  window.gWinUIPane=owner;
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUITabGroupsMenu.sys.mjs',import.meta.url),'utf8').replace('export function','function').replace('export class WinUITabGroupsMenu','class WinUITabGroupsMenu'),context);
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAllTabs.sys.mjs',import.meta.url),'utf8').replace(/^import .*;\r?\n/m,'').replace('export class WinUIAllTabs','this.Adapter=class WinUIAllTabs'),context);
  adapter=new context.Adapter(owner,window);
  const add=(label='Tab '+browser.tabs.length,options={})=>{const tab=new Node('tab');Object.assign(tab,{label,isConnected:true,closing:false,hidden:false,pinned:false,userContextId:0,...options});
    tab.toggleMuteAudio=()=>{tab.muted=!tab.muted;calls.push(['mute',tab]);};Object.defineProperty(tab,'selected',{get:()=>browser.selectedTab===tab});browser.tabs.push(tab);if(!browser.selectedTab)browser.selectedTab=tab;return tab;};
  const group=(tabs,name='Group')=>{const g={id:'group-'+browser.tabGroups.length,label:name,name,color:'blue',isConnected:true,tabs,documentGlobal:window,ownerDocument:document,select(){calls.push(['group',g]);}};for(const t of tabs)t.group=g;browser.tabGroups.push(g);owner.stripSession.ids.set(g,10+browser.tabGroups.length);return g;};
  const open=(session=1)=>adapter.open(session,{x:10,y:40,width:32,height:32});
  const invoke=(operation,row,extra={})=>adapter.execute({menuSession:adapter.session,revision:adapter.revision,epoch:adapter.epoch,operation,row,...extra});
  return {adapter,owner,window,browser,calls,publications,prefs,add,group,open,invoke,Node};
}

test('original row builder preserves ordering, current tab, pinned and container state',()=>{
  const f=fixture();const a=f.add('One',{pinned:true}),b=f.add('Two',{userContextId:1});f.open();
  const rows=f.adapter.description.rows;assert.deepEqual(Array.from(rows,r=>r.label),['One','Two']);assert.equal(rows[0].selected,true);assert.equal(rows[0].pinned,true);assert.equal(rows[1].container,'Container 1');
  assert.equal(f.invoke(1,rows[1].id),1);assert.equal(f.browser.selectedTab,b);assert.equal(f.adapter.session,0);assert.equal(f.calls.at(-1)[2].source,'overflow');
});
test('close and mute use Firefox handlers and keep the opening live',()=>{
  const f=fixture();f.add();const b=f.add('Sound',{muted:true});f.open();const session=f.adapter.session,row=f.adapter.description.rows[1];
  assert.equal(f.invoke(3,row.id),1);assert.equal(b.muted,false);assert.equal(f.adapter.session,session);
  assert.equal(f.invoke(2,row.id),1);assert.equal(b.isConnected,false);assert.equal(f.adapter.description.rows.length,1);assert.equal(f.adapter.session,session);
});
test('refresh without a presentation change still retains current command nodes',()=>{
  const f=fixture();f.add();const b=f.add();f.open();f.adapter.publish();f.adapter.publish();
  assert.equal(f.invoke(2,f.adapter.description.rows[1].id),1);assert.equal(b.isConnected,false);
});
test('collapsed groups and hidden audio follow the original list rules',()=>{
  const f=fixture();f.add('Active');const a=f.add('Grouped A'),b=f.add('Grouped B');f.group([a,b]).collapsed=true;f.add('Hidden',{hidden:true});f.add('Hidden audio',{hidden:true,muted:true});f.open();
  assert.deepEqual(Array.from(f.adapter.description.rows,r=>r.label),['Hidden audio','Active','Group']);
  f.invoke(8);assert.equal(f.adapter.description.hidden,true);assert.deepEqual(Array.from(f.adapter.description.rows,r=>r.label),['Hidden','Hidden audio']);
});
test('private windows omit containers and saved groups',()=>{
  const f=fixture();f.add();f.window.private=true;f.window.SessionStore.savedGroups.push({id:'saved',name:'Private must not see'});f.open();assert.equal(f.adapter.description.containers.length,0);assert.equal(f.adapter.description.groups.length,0);
});
test('presentation changes keep tab commands eligible while replayed openings are rejected',()=>{
  const f=fixture();f.add();f.add();f.open();const old={menuSession:1,revision:1,epoch:1,operation:2,row:f.adapter.description.rows[1].id};f.browser.tabs[1].label='Renamed';assert.equal(f.adapter.execute(old),1);assert.equal(f.adapter.execute(old),2);
  f.adapter.dismiss();assert.equal(f.adapter.execute(old),2);assert.equal(f.open(1),false);assert.equal(f.open(2),true);
});
test('reorder validates source session, privacy and unload before invoking Firefox moves',()=>{
  const f=fixture();const a=f.add(),b=f.add();f.open();const row=f.adapter.description.rows[1].id,extra={sourceHwnd:123,sourceSession:9,sourceTab:f.owner.tabIds.get(a),sourceGroup:0,before:true};
  assert.equal(f.invoke(4,row,{...extra,sourceSession:8}),2);assert.equal(f.calls.length,0);
  assert.equal(f.invoke(4,row,extra),1);assert.equal(f.calls.at(-1)[0],'before');assert.equal(f.calls.at(-1)[1],a);assert.equal(f.calls.at(-1)[2],b);
  a.unload=true;assert.equal(f.invoke(4,row,extra),2);
});
test('container commands retain the original command and entrypoint',()=>{
  const f=fixture();f.add();f.open();assert.equal(f.invoke(10,0,{container:99}),2);f.open(2);assert.equal(f.invoke(10,0,{container:1}),1);
  assert.deepEqual(f.calls.at(-1),['container','Browser:NewUserContextTab','1','all_tabs_menu']);
});
test('saved group activation uses Firefox SessionStore',()=>{
  const f=fixture();f.add();f.window.SessionStore.savedGroups.push({id:'saved',name:'Saved',closedAt:1});f.open();assert.equal(f.invoke(13,f.adapter.description.groups[0].id),1);assert.equal(f.calls.at(-1)[0],'restore');assert.equal(f.calls.at(-1)[1],'saved');
});
test('unsupported native layout revokes the opening without closing any tabs',()=>{
  const f=fixture();f.add();f.open();f.window.gBrowser.tabContainer.verticalMode=true;f.adapter.publish();assert.equal(f.adapter.session,0);assert.equal(f.browser.tabs.length,1);assert.equal(f.publications.at(-1).allTabs.closed,1);
});

test('closed and unchanged panels do not rebuild Firefox row models',()=>{
  const f=fixture();f.add();let builds=0;const read=f.adapter.read.bind(f.adapter);f.adapter.read=()=>{builds++;return read();};
  f.adapter.publish();assert.equal(builds,0);f.open();assert.equal(builds,1);f.adapter.publish();assert.equal(builds,1);
  f.adapter.changed();f.adapter.publish();assert.equal(builds,2);f.adapter.dismiss();f.adapter.publish();assert.equal(builds,2);
});

test('presentation-only changes do not revoke a valid reorder, but structural changes do',()=>{
 const f=fixture();const a=f.add(),b=f.add();f.open();
 const action={menuSession:1,revision:1,epoch:1,operation:4,row:f.adapter.description.rows[1].id,sourceHwnd:123,sourceSession:9,sourceTab:f.owner.tabIds.get(a),sourceGroup:0,before:true};
 b.label='Asynchronous title';assert.equal(f.adapter.execute(action),1);assert.equal(f.adapter.execute(action),2);assert.equal(f.calls.filter(c=>c[0]==='before').length,1);
 const current={...action,revision:f.adapter.revision,epoch:f.adapter.epoch};f.group([b]);assert.equal(f.adapter.execute(current),2);
});

test('retained presenter finishes cleanup before native command rows are borrowed',()=>{
 const f=fixture();f.add();let hidden,showing,removed=false;
 const popup={state:'hiding',addEventListener(type,callback){hidden=callback;},removeEventListener(){removed=true;}};
 f.window.gTabsPanel.allTabsView={closest:()=>popup};
 f.window.document.addEventListener=(type,callback)=>{showing=callback;};
 f.open();assert.equal(f.adapter.description,undefined);assert.equal(f.publications.length,0);
 popup.state='closed';hidden();assert.equal(f.adapter.description.rows.length,1);
 showing({target:f.window.gTabsPanel.allTabsView});assert.equal(f.adapter.session,0);
 f.open(2);assert.equal(f.adapter.session,2);f.adapter.dismiss();
 popup.state='hiding';f.open(3);f.adapter.dismiss();hidden();assert.equal(f.adapter.session,0);assert.equal(removed,true);
});

test('cross-window drops preserve private isolation and live source ownership',()=>{
 const f=fixture(),source=fixture();f.add('Destination');const tab=source.add('Source');f.open();source.open();
 f.owner.stripSourceWindow=()=>source.window;
 const action={sourceHwnd:123,sourceSession:9,sourceTab:source.owner.tabIds.get(tab),sourceGroup:0,before:true},row=f.adapter.description.rows[0].id;
 source.window.private=true;assert.equal(f.invoke(4,row,action),2);assert.equal(f.calls.length,0);
 source.window.private=false;source.owner.attached=false;assert.equal(f.invoke(4,row,action),2);assert.equal(f.calls.length,0);
 source.owner.attached=true;assert.equal(f.invoke(4,row,action),1);assert.equal(f.calls[0][0],'adopt');assert.equal(f.calls.at(-1)[0],'before');
});

test('unexposed container and hidden-list destinations are refused by Firefox',()=>{
 const f=fixture();const source=f.add('Visible');f.add('Hidden',{hidden:true});f.open();
 assert.equal(f.invoke(9),2);f.invoke(8);
 assert.equal(f.invoke(4,f.adapter.description.rows[0].id,{sourceHwnd:123,sourceSession:9,sourceTab:f.owner.tabIds.get(source),sourceGroup:0,before:true}),2);
 assert.equal(f.calls.length,0);f.adapter.dismiss();f.window.private=true;
 for(const operation of [11,15]){f.open(operation);assert.equal(f.invoke(operation),2);assert.equal(f.calls.length,0);}
});
