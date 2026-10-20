import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
function fixture(){
  const calls=[],prefs=new Map([['devtools.performance.popup.feature-flag',true]]),observers=new Map();let active=false,paused=false,selected='web-developer';
  const node={},tab={},window={gBrowser:{selectedTab:tab,selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}},
    Localization:class{formatMessagesSync([{id}]){return [{value:id,attributes:[{name:'label',value:id}]}];}},
    openTrustedLinkIn:(...a)=>calls.push(['settings',...a]),openWebLinkIn:(...a)=>calls.push(['help',...a])};
  const item={id:'widget-1',placed:true,stock:[{id:'profiler-button',label:'Profiler',enabled:true,shown:true}]};
  const toolbar={window,active:true,navigation:true,supported:()=>true,menuAnchor:()=>({}),owner:{addressSession:{sessionId:'address'},addressTab:tab,addressDocument:7,invalidateAddress(){}},
    customization:{current:{items:[item]},nativeOverflow:new Set(),entries:new Map([['profiler-button',{id:item.id,node}]]),ui:{getWidget:()=>({forWindow:()=>({node})})}}};
  const presets={presets:{'web-developer':{l10nIds:{popup:{label:'web-label',description:'web-description'}}},graphics:{l10nIds:{popup:{label:'graphics-label',description:'graphics-description'}}}},
    getRecordingSettings:()=>({presetName:selected}),changePreset:(page,key,features)=>{selected=key;calls.push(['preset',page,key,features]);}};
  const background={startProfiler:page=>{active=true;calls.push(['start',page]);},stopProfiler:()=>{active=paused=false;calls.push(['discard']);},captureProfile:async page=>{paused=true;calls.push(['capture',page]);}};
  const Services={profiler:{IsActive:()=>active,IsPaused:()=>paused,GetFeatures:()=>['js']},
    prefs:{getBoolPref:(k,f)=>prefs.get(k)??f,addObserver(){},removeObserver(){}},obs:{addObserver:(fn,k)=>observers.set(k,fn),removeObserver:(_fn,k)=>observers.delete(k)}};
  const context=vm.createContext({console,Services,ChromeUtils:{importESModule:path=>path.includes('prefs-presets')?presets:background}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIProfilerPanel.sys.mjs',import.meta.url),'utf8').replace('export class WinUIProfilerPanel','this.Panel=class WinUIProfilerPanel'),context);
  const panel=new context.Panel(toolbar);toolbar.navigationState=()=>({profilerPanel:panel.snapshot()});
  const act=(verb='open',id,revision=panel.revision,session=1)=>panel.execute({command:`profiler-panel:${verb}:${session}`+(id===undefined?'':`:${revision}:${id}`)});
  return {panel,toolbar,window,item,node,calls,prefs,observers,presets,act,setState:(a,p=false)=>{active=a;paused=p;},setPreset:key=>selected=key};
}
test('idle panel uses Firefox presets and localization, without starting a recorder',()=>{
  const f=fixture();assert.equal(f.act(),1);const m=f.panel.description;assert.equal(m.active,false);assert.equal(m.selected,'web-developer');assert.equal(m.options.length,3);assert.deepEqual(f.calls,[]);
  assert.equal(m.actions.find(a=>a.key==='start').shown,true);assert.equal(m.actions.find(a=>a.key==='capture').enabled,false);
});
test('start and discard retain the opening and acknowledge with newer revisions',()=>{
  const f=fixture();f.act();const initial=f.panel.revision;assert.equal(f.act('invoke',1),1);assert.equal(f.panel.description.active,true);assert.ok(f.panel.revision>initial);
  assert.equal(f.act('invoke',1,initial),2);assert.equal(f.act('invoke',2),1);assert.equal(f.panel.description.active,false);
  assert.deepEqual(f.calls,[['start','aboutprofiling'],['discard']]);
});
test('preset selection uses shared settings and custom reveals original settings destination',()=>{
  const f=fixture();f.act();const graphics=f.panel.description.options.find(o=>o.key==='graphics');f.act('invoke',graphics.id);assert.equal(f.panel.description.description,'graphics-description');
  const custom=f.panel.description.options.find(o=>o.key==='custom');f.act('invoke',custom.id);assert.equal(f.panel.description.actions.find(a=>a.id===4).shown,true);
  assert.equal(f.act('invoke',4),1);assert.equal(f.panel.session,0);assert.deepEqual(f.calls.at(-1),['settings','about:profiling','tab']);
});
test('global state changes revoke stale operations and paused capture is unavailable',()=>{
  const f=fixture();f.act();const previous=f.panel.revision;f.setState(true);f.observers.get('profiler-started')();
  assert.equal(f.act('invoke',1,previous),2);assert.equal(f.panel.description.active,true);
  f.setState(true,true);f.observers.get('profiler-paused')();assert.equal(f.panel.description.paused,true);assert.equal(f.panel.description.actions.find(a=>a.id===2).shortcutEnabled,false);assert.equal(f.act('invoke',3),2);
  assert.equal(f.act('invoke',100),2);assert.equal(f.act('invoke',2),1);
});
test('capture consumes the opening once and uses the original service',()=>{
  const f=fixture();f.setState(true);f.act();assert.equal(f.act('invoke',3),1);assert.equal(f.act('invoke',3),2);assert.deepEqual(f.calls,[['capture','aboutprofiling']]);
});
test('source, browser identity, customization and policy changes revoke without commands',()=>{
  for(const change of [f=>f.item.placed=false,f=>f.toolbar.customization.nativeOverflow.add('profiler-button'),f=>f.toolbar.customization.editorOpen=true,
    f=>f.toolbar.owner.addressDocument++,f=>f.window.gBrowser.selectedTab={},f=>f.toolbar.active=false,f=>f.window.gDialogBox={isOpen:true},
    f=>f.prefs.set('devtools.policy.disabled',true),f=>f.prefs.set('devtools.performance.popup.feature-flag',false),f=>f.toolbar.customization.entries.get('profiler-button').node={}]){
    const f=fixture();f.act();change(f);assert.equal(f.act('invoke',1),2);assert.equal(f.panel.session,0);assert.deepEqual(f.calls,[]);
  }
});
test('malformed and replayed openings cannot resurrect dismissed or torn-down panels',()=>{
  const f=fixture();for(const command of ['profiler-panel:open:4294967296','profiler-panel:open:1:1:1','profiler-panel:invoke:1:1:1'])assert.equal(f.panel.execute({command}),2);
  f.act();f.act('dismiss');assert.equal(f.act(),2);assert.equal(f.act('open',undefined,0,2),1);f.panel.close();assert.equal(f.act('invoke',1,1,2),2);assert.equal(f.observers.size,0);
});
test('same preset selection still acknowledges retained input, help does not mutate profiling',()=>{
  const f=fixture();f.act();const revision=f.panel.revision;assert.equal(f.act('invoke',100),1);assert.ok(f.panel.revision>revision);
  assert.equal(f.act('invoke',5),1);assert.deepEqual(f.calls.at(-1),['help','https://profiler.firefox.com/docs/','tab']);assert.equal(f.panel.session,0);
});
