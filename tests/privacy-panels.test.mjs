import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

function fixture(){
  const calls=[],prefs=new Map([['privacy.panicButton.enabled',true],['browser.privatebrowsing.resetPBM.enabled',true],['browser.privatebrowsing.resetPBM.showConfirmationDialog',true]]);
  const nodes=new Map(),items=[],entries=new Map(),changes=[];
  for(const [index,widget] of ['panic-button','reset-pbm-toolbar-button'].entries()){
    const node={id:widget},item={id:'widget-'+index,placed:true,stock:[{id:widget,enabled:true,shown:true,label:widget}]};
    nodes.set(widget,node);items.push(item);entries.set(widget,{id:item.id,node});
  }
  const window={private:true,PrivateBrowsingUtils:{isWindowPrivate:w=>w.private},Localization:class {formatMessagesSync([{id}]){return [{value:id,attributes:[{name:'label',value:id}]}];}},
    gBrowser:{selectedTab:{},selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,menuAnchor:()=>({}),owner:{addressSession:{sessionId:'address'},
    addressTab:window.gBrowser.selectedTab,addressDocument:7,invalidateAddress(){}},customization:{current:{items},entries,nativeOverflow:new Set(),ui:{getWidget:id=>({forWindow:()=>({node:nodes.get(id)})})}}};
  const controllers={forget:{id:'panic-button',forgetWindow:(w,span)=>{calls.push(['forget',w,span]);return Promise.resolve();}},reset:{restartFromToolbar:(w,options)=>{calls.push(['reset',w,options]);return Promise.resolve();}}};
  const context=vm.createContext({console,ChromeUtils:{importESModule:()=>({CustomizableWidgets:[controllers.forget],ResetPBMPanel:controllers.reset})},
    Glean:{privateBrowsingResetPbm:{confirmPanel:{record:v=>calls.push(['telemetry',v])}}},
    Services:{prefs:{getBoolPref:(k,f)=>prefs.get(k)??f,addObserver:(k,fn)=>changes.push(fn),removeObserver(){}},obs:{addObserver(){},removeObserver(){}}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIPrivacyPanels.sys.mjs',import.meta.url),'utf8').replace('export class WinUIPrivacyPanels','this.Panels=class WinUIPrivacyPanels'),context);
  const panels=new context.Panels(toolbar),navigation={'reset-pbm':{enabled:true,label:'Clear private session'}};
  toolbar.navigationState=()=>({...navigation,...panels.snapshot(navigation)});toolbar.navigationState();
  const open=(kind,session=1)=>panels.execute({command:`privacy-panel:open:${kind}:${session}`,x:10,y:10,width:32,height:32});
  const confirm=(kind,choice,remember=true,session=1)=>panels.execute({command:`privacy-panel:confirm:${kind}:${session}:1:${choice}:${remember?1:0}`});
  return {panels,toolbar,window,prefs,calls,items,nodes,changes,controllers,open,confirm};
}
test('Forget is inert on opening and cancellation and preserves the three Firefox choices',()=>{
  const f=fixture();assert.equal(f.open(1),1);const model=f.panels.panels[0].description;
  assert.deepEqual(Array.from(model.options,i=>i.id),[1,2,3]);assert.equal(model.warning,'panic-button-undo-warning');assert.deepEqual(f.calls,[]);
  assert.equal(f.panels.execute({command:'privacy-panel:cancel:1:1'}),1);assert.deepEqual(f.calls,[]);
});
test('Forget consumes confirmation exactly once and routes every range to Firefox',async()=>{
  for(const [choice,span] of [[1,5],[2,2],[3,6]]){
    const f=fixture();f.open(1);assert.equal(f.confirm(1,0),2);assert.equal(f.confirm(1,choice),1);assert.equal(f.confirm(1,choice),2);
    assert.equal(f.calls.length,1);assert.equal(f.calls[0][0],'forget');assert.equal(f.calls[0][1],f.window);assert.equal(f.calls[0][2],span);
    assert.equal(f.panels.snapshot({})['forgetPanel'].enabled,false);await new Promise(setImmediate);assert.equal(f.panels.busy,false);
  }
});
test('privacy publication never mutates the already revisioned customization description',()=>{
  const f=fixture();const before=JSON.stringify(f.items);f.panels.busy=true;
  assert.equal(f.panels.snapshot({'reset-pbm':{enabled:true}}).forgetPanel.enabled,false);
  assert.equal(JSON.stringify(f.items),before);
});
test('Clear Private Session is private-only, cancellation never persists the checkbox',()=>{
  const f=fixture();f.window.private=false;assert.equal(f.open(2),2);assert.deepEqual(f.calls,[]);
  f.window.private=true;assert.equal(f.open(2),1);assert.equal(f.panels.panels[1].description.remember,true);
  assert.equal(f.panels.execute({command:'privacy-panel:cancel:2:1'}),1);assert.equal(f.calls.filter(c=>c[0]==='reset').length,0);
  assert.equal(f.prefs.get('browser.privatebrowsing.resetPBM.showConfirmationDialog'),true);
});
test('private confirmation passes the checkbox only on explicit confirm; no-confirm honors the preference',()=>{
  const f=fixture();f.open(2);assert.equal(f.confirm(2,1),2);assert.equal(f.confirm(2,0,false),1);
  const call=f.calls.find(c=>c[0]==='reset');assert.deepEqual({...call[2]},{confirmed:true,alwaysAsk:false});
  const g=fixture();g.prefs.set('browser.privatebrowsing.resetPBM.showConfirmationDialog',false);
  assert.equal(g.open(2),1);assert.equal(g.panels.panels[1].description,null);assert.deepEqual({...g.calls.find(c=>c[0]==='reset')[2]},{confirmed:false,alwaysAsk:false});
  assert.equal(g.open(2,2),2);
});
test('context, placement, exact source and feature changes revoke confirmations',()=>{
  for(const mutate of [f=>f.toolbar.owner.addressDocument++,f=>f.items[0].placed=false,f=>f.items[0].stock[0].enabled=false,
    f=>f.nodes.set('panic-button',{}),f=>f.toolbar.customization.nativeOverflow.add('panic-button'),f=>f.toolbar.customization.editorOpen=true,
    f=>f.prefs.set('privacy.panicButton.enabled',false),f=>f.toolbar.active=false]){
    const f=fixture();f.open(1);mutate(f);assert.equal(f.confirm(1,1),2);assert.equal(f.panels.panels[0].session,0);assert.deepEqual(f.calls,[]);
  }
});
test('preference notifications, alternate panels and malformed/stale requests cannot resurrect a session',()=>{
  const f=fixture();f.open(1);f.open(2);assert.equal(f.confirm(1,1),2);f.changes[0]();assert.equal(f.confirm(2,0),2);
  for(const command of ['privacy-panel:open:3:2','privacy-panel:open:1:4294967296','privacy-panel:open:1:1:1:1:1','privacy-panel:confirm:1:1:1:4:1'])assert.equal(f.panels.execute({command}),2);
  f.panels.close();assert.equal(f.open(1,2),2);
});

test('Firefox Forget gateway retains exact clearing categories, range, window privacy and notifier',async()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/customizableui/CustomizableWidgets.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('if (Services.prefs.getBoolPref("privacy.panicButton.enabled"))');
  const code=source.slice(start,source.indexOf('if (PrivateBrowsingUtils.enabled)',start));
  const calls=[],window={private:true};
  const context=vm.createContext({CustomizableWidgets:[],Services:{prefs:{getBoolPref:()=>true},wm:{getMostRecentWindow:()=>({PanicButtonNotifier:{notify:()=>calls.push('notify')}})}},
    PrivateBrowsingUtils:{isWindowPrivate:w=>w.private},lazy:{Sanitizer:{getClearRange:span=>{calls.push(['range',span]);return [10,20];},sanitize:(items,options)=>{calls.push([Array.from(items),options]);return Promise.resolve();}}}});
  vm.runInContext(code,context);await context.CustomizableWidgets[0].forgetWindow(window,2);
  assert.deepEqual(calls[0],['range',2]);assert.deepEqual(calls[1][0],['cookies','history','openWindows','formdata','sessions','cache','downloads','offlineApps']);
  assert.equal(calls[1][1].privateStateForNewWindow,'private');assert.equal(calls[1][1].ignoreTimespan,false);assert.equal(calls[2],'notify');
});

test('Firefox reset gateway enforces private context and confirmation before preferences or clearing',async()=>{
  const prefs=new Map(),calls=[];
  const context=vm.createContext({console,Services:{prefs:{setBoolPref:(k,v)=>prefs.set(k,v)}},
    XPCOMUtils:{defineLazyPreferenceGetter:(object,key,_pref,fallback)=>object[key]=key==='_enabled'?true:fallback},
    ChromeUtils:{defineESModuleGetters:lazy=>Object.assign(lazy,{PrivateBrowsingUtils:{isWindowPrivate:w=>w?.private}})},
    Glean:{privateBrowsingResetPbm:{confirmPanel:{record:v=>calls.push(v)},resetAction:{record:v=>calls.push(v)}}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/privatebrowsing/ResetPBMPanel.sys.mjs',import.meta.url),'utf8').replace(/^import\b[^;]*;$/m,'').replace('export const ResetPBMPanel','this.ResetPBMPanel'),context);
  const reset=context.ResetPBMPanel;reset._restartPBM=async w=>calls.push(w);
  await assert.rejects(reset.restartFromToolbar({private:false},{confirmed:true,alwaysAsk:false}));
  await assert.rejects(reset.restartFromToolbar({private:true},{confirmed:false}));assert.equal(prefs.size,0);assert.equal(calls.length,0);
  await reset.restartFromToolbar({private:true},{confirmed:true,alwaysAsk:false});assert.equal(prefs.get('browser.privatebrowsing.resetPBM.showConfirmationDialog'),false);
  reset._shouldConfirmClear=false;await reset.restartFromToolbar({private:true},{confirmed:false});assert.equal(calls.at(-1).did_confirm,false);
});
