import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
function fixture(){
  const calls=[],publications=[],tabs=[{},{}],group={isConnected:true,label:'Example',color:'blue',tabs:[tabs[1]]};
  const colors=['blue','purple','cyan','orange','yellow','pink','green','gray','red'];
  const controller={close(){},nativeEditState:()=>({colors,commands:[{key:'new-tab',l10n:'new',enabled:true},{key:'save',l10n:'save',enabled:true}],advanced:false}),
    openEditModal:(...args)=>calls.push(['fallback',...args]),openCreateModal:(...args)=>calls.push(['create-fallback',...args]),nativeCreationFinished:(g,keep)=>{calls.push(['created',keep]);if(!keep)g.isConnected=false;},nativeEditValue:(g,name,color)=>{g.label=name;g.color=color;calls.push(['edit',name,color]);return true;},nativeEditFinished:(g,name)=>calls.push(['finish',g.label,name]),nativeEditCommand:(g,key)=>{calls.push(['command',g,key]);return true;}};
  const w={devicePixelRatio:1,document:{documentElement:{hasAttribute:()=>false}},gDialogBox:{isOpen:false},gBrowser:{selectedTab:tabs[0],selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:10}}},tabGroups:[group],tabGroupMenu:controller,tabContainer:{addEventListener(){},removeEventListener(){}}},
    addEventListener(){},removeEventListener(){},windowUtils:{syncWinUITabMenu:s=>publications.push(JSON.parse(s).groupHeader ?? JSON.parse(s).groupCreation)},
    MutationObserver:class{observe(){}disconnect(){}},Localization:class{formatMessagesSync([m]){return [{value:m.id}];}}};
  const owner={attached:true,schedule(){},stripSession:{ids:new Map([[group,1]]),windowDragSession:2}},context=vm.createContext({ChromeUtils:{importESModule:()=>({TabStateFlusher:{flush:()=>Promise.resolve()}})},Services:{obs:{addObserver(){},removeObserver(){}},prefs:{getBoolPref:(_k,f)=>f,addObserver(){},removeObserver(){}}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIGroupHeader.sys.mjs',import.meta.url),'utf8').replace('export class WinUIGroupHeader','this.Menu=class WinUIGroupHeader'),context);
  const menu=new context.Menu(owner,w),open=()=>menu.open(group,{x:4,y:5,width:60,height:32},1),act=(command,rest={})=>menu.execute({kind:'group-header',session:1,revision:menu.revision,command,...rest});
  return {menu,w,owner,group,tabs,calls,publications,open,act,controller};
}
test('header is independent of optional toolbar and exposes native name/color editing',()=>{
  const f=fixture();assert.equal(f.open(),true);assert.equal(f.menu.description.name,'Example');assert.equal(f.menu.description.colors.length,9);
  const revision=f.menu.revision;assert.equal(f.act(1,{value:'Renamed'}),1);assert.equal(f.group.label,'Renamed');assert.ok(f.menu.revision>revision);
  assert.equal(f.act(2,{value:'green'}),1);assert.equal(f.group.color,'green');assert.equal(f.act(3),1);assert.equal(f.menu.session,0);assert.equal(f.calls.at(-1)[0],'finish');
});
test('commands use original controller only once and stale replies cannot replay edits',()=>{
  const f=fixture();f.open();const revision=f.menu.revision;f.act(1,{value:'New'});
  assert.equal(f.act(2,{value:'red',revision}),2);assert.equal(f.act(4),1);assert.equal(f.calls.at(-1)[2],'new-tab');assert.equal(f.act(4),2);
});
test('Escape/light dismissal preserves latest native draft including an unacknowledged edit',()=>{
  const f=fixture();f.open();f.act(1,{value:'First'});assert.equal(f.act(0,{name:'Last text',color:'red'}),1);assert.equal(f.group.label,'Last text');assert.equal(f.group.color,'red');
});
test('external identity, membership, metadata, selected document and modal changes revoke',()=>{
  for(const mutate of [f=>f.group.label='External',f=>f.group.color='red',f=>f.group.tabs=[{}],f=>f.w.gBrowser.tabGroups=[{...f.group}],f=>f.w.gBrowser.selectedTab={},f=>f.w.gDialogBox.isOpen=true,f=>f.owner.attached=false,f=>f.owner.addressToolbar={customization:{editorOpen:true}}]){
    const f=fixture();f.open();mutate(f);assert.equal(f.act(4),2);assert.equal(f.menu.session,0);assert.ok(!f.calls.some(c=>c[0]==='command'));
  }
});
test('availability changes are revised in place; inventory changes dismiss',()=>{
  const f=fixture();f.open();const previous=f.menu.revision;f.controller.nativeEditState=()=>({colors:f.menu.description.colors.map(c=>c.key),commands:[{key:'new-tab',l10n:'new',enabled:false},{key:'save',l10n:'save',enabled:true}],advanced:false});
  f.menu.publish();assert.ok(f.menu.revision>previous);assert.equal(f.menu.description.commands[0].enabled,false);assert.equal(f.act(4),2);
  f.controller.nativeEditState=()=>({colors:[],commands:[],advanced:false});f.menu.publish();assert.equal(f.menu.session,0);
});
test('malformed edits and delayed flush after teardown cannot mutate or resurrect',async()=>{
  const f=fixture();f.open();for(const action of [{command:1,value:'x'.repeat(4097)},{command:2,value:'invalid'},{command:99},{command:1,value:'late',session:2}])assert.equal(f.act(action.command,action),2);
  f.menu.close();const count=f.publications.length;await Promise.resolve();await Promise.resolve();assert.equal(f.publications.length,count);assert.equal(f.open(),false);
});
test('oversized existing names and escaped descriptions preserve the original editor',()=>{
  for(const value of ['x'.repeat(4097),'\u0001'.repeat(4096)]){
    const f=fixture();f.group.label=value;assert.equal(f.open(),true);assert.equal(f.menu.session,0);
    assert.equal(f.calls.at(-1)[0],'fallback');assert.equal(f.group.label,value);assert.equal(f.publications.at(-1).fallback,true);
  }
});

function create(f){
  assert.equal(f.menu.requestCreation(f.group),true);f.menu.publishCreation();
  const request=f.publications.at(-1);assert.equal(request.group,1);assert.equal(request.window,2);
  assert.equal(f.menu.open(f.group,{x:4,y:5,width:60,height:32},1,request.request),true);
  return request;
}
test('creation publishes after strip identity and exposes only name, color, Done and Cancel',()=>{
  const f=fixture();create(f);
  assert.equal(f.menu.description.creation,true);assert.equal(f.menu.description.commands.length,0);
  assert.equal(f.menu.description.title,'tab-group-editor-title-create');assert.equal(f.menu.description.cancel,'tab-group-editor-cancel');
  f.act(1,{value:'Created'});f.act(2,{value:'green'});
  assert.equal(f.act(3,{name:'Final',color:'green'}),1);assert.equal(f.group.label,'Final');assert.equal(f.group.isConnected,true);
  assert.deepEqual(f.calls.filter(c=>c[0]==='created'),[['created',true]]);assert.equal(f.act(3),2);
});
test('creation uses Firefox selection settled before publication and rejects later selection changes',()=>{
  for(const changedAfterPublication of [false,true]) {
    const f=fixture();assert.equal(f.menu.requestCreation(f.group),true);
    f.w.gBrowser.selectedTab=f.tabs[1];
    f.menu.publishCreation();const request=f.publications.at(-1).request;
    if(changedAfterPublication)f.w.gBrowser.selectedTab=f.tabs[0];
    const selected=f.w.gBrowser.selectedTab;
    assert.equal(f.menu.open(f.group,{x:4,y:5,width:60,height:32},1,request),!changedAfterPublication);
    assert.equal(f.w.gBrowser.selectedTab,selected);
    assert.equal(f.group.isConnected,true);
    if(!changedAfterPublication)assert.equal(f.menu.description.creation,true);
  }
});
test('creation Cancel and Escape ungroup once; light dismissal keeps tabs grouped',()=>{
  for(const action of [{command:21},{command:0,cancel:true},{command:0}]){
    const f=fixture();create(f);assert.equal(f.act(action.command,{...action,name:'Draft',color:'red'}),1);
    const keep=action.command===0 && !action.cancel;
    assert.equal(f.group.isConnected,keep);assert.deepEqual(f.calls.filter(c=>c[0]==='created'),[['created',keep]]);
    assert.equal(f.tabs.length,2);assert.equal(f.act(action.command,action),2);
  }
});
test('creation rejects stale identity, membership and future cancellation revisions',()=>{
  const f=fixture();f.menu.requestCreation(f.group);const request=f.menu.pendingCreation.request;
  assert.equal(f.menu.open(f.group,{},1,request+1),false);
  f.group.tabs.push({});assert.equal(f.menu.open(f.group,{},1,request),false);
  const g=fixture();create(g);assert.equal(g.act(21,{revision:g.menu.revision+1}),2);assert.equal(g.group.isConnected,true);
  assert.equal(g.act(4),2);g.w.gBrowser.selectedTab={};assert.equal(g.act(21),2);
  assert.deepEqual(g.calls.filter(c=>c[0]==='created'),[['created',true]]);
});
test('creation More transfers the original draft and original name without committing',()=>{
  const f=fixture();const state=f.controller.nativeEditState();f.controller.nativeEditState=()=>({...state,advanced:true});create(f);
  f.act(1,{value:'Draft'});assert.equal(f.act(20,{x:1,y:2,width:40,height:30}),1);
  assert.equal(f.calls.some(c=>c[0]==='created'),false);
  const call=f.calls.at(-1);assert.equal(call[0],'create-fallback');assert.equal(call[2].nativeFallback,true);assert.equal(call[2].initialName,'Example');
});
test('creation survives its own loading document and falls back when publication fails',()=>{
  const f=fixture();create(f);f.w.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId++;
  assert.equal(f.act(3),1);assert.deepEqual(f.calls.filter(c=>c[0]==='created'),[['created',true]]);
  const g=fixture();g.menu.requestCreation(g.group);g.w.windowUtils.syncWinUITabMenu=()=>{throw new Error('unavailable');};g.menu.publishCreation();
  assert.equal(g.calls.at(-1)[0],'create-fallback');assert.equal(g.calls.at(-1)[2].nativeFallback,true);
});

test('a rejected queued creation opens the original create panel or settles a superseded tab',()=>{
  for(const changed of [false,true]){
    const f=fixture();f.menu.requestCreation(f.group);const creation=f.menu.pendingCreation.request;
    if(changed)f.w.gBrowser.selectedTab={};
    f.menu.rejectCreation({creation,menuSession:1});assert.equal(f.menu.pendingCreation,null);
    assert.equal(f.calls.at(-1)[0],changed?'created':'create-fallback');
    assert.equal(f.group.isConnected,true);
  }
});
