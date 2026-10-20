import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
function fixture(){
  const calls=[],publications=[],tabs=[{},{}],group={isConnected:true,label:'Example',color:'blue',tabs:[tabs[1]]};
  const colors=['blue','purple','cyan','orange','yellow','pink','green','gray','red'];
  const controller={close(){},nativeEditState:()=>({colors,commands:[{key:'new-tab',l10n:'new',enabled:true},{key:'save',l10n:'save',enabled:true}],advanced:false}),
    openEditModal:(...args)=>calls.push(['fallback',...args]),nativeEditValue:(g,name,color)=>{g.label=name;g.color=color;calls.push(['edit',name,color]);return true;},nativeEditFinished:(g,name)=>calls.push(['finish',g.label,name]),nativeEditCommand:(g,key)=>{calls.push(['command',g,key]);return true;}};
  const w={devicePixelRatio:1,document:{documentElement:{hasAttribute:()=>false}},gDialogBox:{isOpen:false},gBrowser:{selectedTab:tabs[0],selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:10}}},tabGroups:[group],tabGroupMenu:controller,tabContainer:{addEventListener(){},removeEventListener(){}}},
    addEventListener(){},removeEventListener(){},windowUtils:{syncWinUITabMenu:s=>publications.push(JSON.parse(s).groupHeader)},
    MutationObserver:class{observe(){}disconnect(){}},Localization:class{formatMessagesSync([m]){return [{value:m.id}];}}};
  const owner={attached:true},context=vm.createContext({ChromeUtils:{importESModule:()=>({TabStateFlusher:{flush:()=>Promise.resolve()}})},Services:{obs:{addObserver(){},removeObserver(){}},prefs:{getBoolPref:(_k,f)=>f,addObserver(){},removeObserver(){}}}});
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
