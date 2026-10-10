import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const base=new URL('../upstream/firefox/browser/components/urlbar/content/',import.meta.url);
function fixture() {
  const prefs={'history.enabled':true}, calls=[], observers=new Set();
  const local={source:1,restrict:'^',pref:'history.enabled',uiLabel:'history-label',icon:'history'};
  const engine={id:'engine',name:'Engine',isConfigEngine:true,getIconURL:async()=> 'engine.png'};
  const state={value:'query',pageProxyValid:false,searchMode:null};
  const services={sapName:'urlbar', readState:()=>state,
    engineStore:{init:async()=>{},getEngines:()=>[engine,{...engine,id:'hidden',hideOneOffButton:true}],
      getEngineByName:name=>name===engine.name?engine:null,default:engine,
      addObserver:fn=>observers.add(fn),removeObserver:fn=>observers.delete(fn)},
    l10n:{formatMessages:async specs=>specs.map(({id})=>({value:id,attributes:[{name:'title',value:id}]})),
      formatValue:async(id,args)=>id+':'+args.engineName},
    offeredEngines:()=>[1,2,3,4].map(n=>({uri:`offer:${n}`,title:`Offer ${n}`})),
    whereToOpenLink:event=>event.where || 'current',
    search:(value,options)=>calls.push({kind:'search',value,options}),
    openEngine:(value,options)=>calls.push({kind:'navigate',value,options}),
    exit:()=>{state.searchMode=null;calls.push({kind:'exit'});},
    openPreferences:()=>calls.push({kind:'settings'}),
    recordPicked:kind=>calls.push({kind:'telemetry',value:kind}),
    installEngine:async()=>false};
  // No browser window, input, DOM or switcher is supplied to the real module.
  const context=vm.createContext({UrlbarPrefs:{get:key=>prefs[key]},
    UrlbarShared:{LOCAL_SEARCH_MODES:[local],ICON:{SEARCH_GLASS:'glass'},getResultSourceName:()=> 'history'}});
  vm.runInContext(readFileSync(new URL('UrlbarSearchModeModel.mjs',base),'utf8')
    .replace(/^import .*;\r?$/gm,'').replace('export class UrlbarSearchModeModel','this.Model = class UrlbarSearchModeModel'),context);
  return {model:new context.Model(services),services,state,calls,prefs,engine,local,observers};
}

test('engine filtering, SAP-local eligibility and settings redesign share Firefox policy',async()=>{
  const {model,services,prefs,engine,local}=fixture();
  await model.populateEngines();assert.deepEqual([...model.engines],[engine,local]);
  prefs['history.enabled']=false;
  await model.populateEngines();assert.deepEqual([...model.engines],[engine]);
  prefs['browser.settings-redesign.enabled']=true;
  await model.populateEngines();assert.deepEqual([...model.engines],[engine,local]);
  services.sapName='searchbar';
  await model.populateEngines();assert.deepEqual([...model.engines],[engine]);
});

test('search-service failure retains local modes and a glass default',async()=>{
  const {model,services,local}=fixture();
  services.engineStore.init=async()=>{throw Error('unavailable');};
  await model.populateEngines();assert.deepEqual([...model.engines],[local]);
  assert.equal((await model.displayedEngineDetails()).icon,'glass');
  assert.equal((await model.displayedEngineDetails(local)).label,'history-label');
});

test('snapshot preserves offered-engine limit/order and local labels without a presenter',async()=>{
  const {model,state,local}=fixture();state.searchMode=local;
  const data=await model.snapshot();
  assert.deepEqual([...data.entries].map(e=>e.key),['engine:engine','local:^','install:offer:3','install:offer:2','install:offer:1','settings']);
  assert.equal(data.entries[1].label,'history-label');
  assert.equal(data.icon,null);assert.equal(data.symbol,'history');assert.equal(data.active,true);
  assert.equal(data.exitLabel,'urlbar-searchmode-exit-button2');
});

test('valid page display is not reused as a search string; typed text is retained',()=>{
  const {model,state,calls}=fixture();state.pageProxyValid=true;
  model.localSearch('^');assert.equal(calls[0].value,'^ ');
  state.pageProxyValid=false;model.localSearch('^');assert.equal(calls[2].value,'^ query');
});

test('remote activation preserves shift, background tab and engine telemetry semantics',async()=>{
  for (const [event,expected,where] of [[{},'query','current'],[{shiftKey:true,where:'window'},'navigation','current'],
    [{where:'tab'},'navigation','tab'],[{where:'tabshifted'},'navigation','tabshifted']]) {
    const {model,calls}=fixture();
    assert.equal(await model.activate('engine:engine',event,()=>true),expected);
    assert.equal(calls[0].kind,expected==='query'?'search':'navigate');
    if(expected==='query') {
      assert.equal(calls[0].options.focus,false);assert.equal(calls[0].options.startQuery,false);
    } else {assert.equal(calls[0].options.where,where);assert.equal(calls[0].options.inBackground,true);}
    assert.equal(calls[1].value,'builtin_search');
  }
});

test('stale activation after engine initialization and unknown keys cannot issue commands',async()=>{
  const {model,services,calls}=fixture();let current=true;
  services.engineStore.init=async()=>{current=false;};
  assert.equal(await model.activate('engine:engine',{},()=>current),false);
  assert.equal(await model.activate('engine:missing',{},()=>true),false);
  assert.equal(await model.activate('install:offer:4',{},()=>true),false);
  assert.equal(calls.length,0);
});

test('exit clears cycling cache and settings uses the shared command',async()=>{
  const {model,calls}=fixture();model.selectedIndex=3;
  assert.equal(await model.activate('exit',{},()=>true),'query');
  assert.equal(model.engines.length,0);assert.equal(model.selectedIndex,0);
  assert.equal(await model.activate('settings',{},()=>true),'settings');
  assert.deepEqual(calls.map(c=>c.kind),['exit','settings','telemetry']);
});

test('OpenSearch selection runs once and removes its observer after notification',async()=>{
  const {model,services,calls,observers,engine}=fixture();
  services.installEngine=async()=>{for(const fn of [...observers])fn('added',engine);return true;};
  assert.equal(await model.activate('install:offer:1',{},()=>true),'query');
  assert.equal(observers.size,0);
  assert.equal(calls.filter(c=>c.kind==='search').length,1);
  assert.equal(calls.find(c=>c.kind==='search').options.startQuery,false);
});

test('OpenSearch cancellation, rejection and stale completion release observers without searching',async()=>{
  for(const outcome of ['cancel','reject','stale','dispose']) {
    const {model,services,calls,observers,engine}=fixture();let current=true;
    services.installEngine=async()=>{
      if(outcome==='reject')throw Error('install failed');
      if(outcome==='stale') {current=false;for(const fn of [...observers])fn('added',engine);}
      if(outcome==='dispose')model.dispose();
      return outcome!=='cancel';
    };
    const action=model.activate('install:offer:1',{},()=>true,()=>current);
    if(outcome==='reject')await assert.rejects(action,/install failed/);
    else assert.equal(await action,false);
    assert.equal(observers.size,0,outcome);assert.equal(calls.some(c=>c.kind==='search'),false,outcome);
  }
});

test('disposing removes a pending observer and prevents subsequent activation',async()=>{
  const {model,services,calls,observers,engine}=fixture();let observer;
  services.installEngine=async()=>{observer=[...observers][0];return true;};
  await model.install({uri:'offer:1'});assert.equal(observers.size,1);
  model.dispose();observer('added',engine);assert.equal(observers.size,0);
  assert.equal(calls.some(c=>c.kind==='search'),false);
  assert.equal(await model.activate('engine:engine',{},()=>true),false);
});

test('both input implementations own and dispose the binding, and the native adapter calls the model',()=>{
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) {
    const source=readFileSync(new URL(name,base),'utf8');
    assert.match(source,/bindSearchModeModel\(this\)/);assert.match(source,/#searchModeModel\?\.dispose\(\)/);
  }
  const switcher=readFileSync(new URL('SearchModeSwitcher.mjs',base),'utf8');
  assert.doesNotMatch(switcher,/nativeSnapshot|nativeActivate|nativeOpened/);
  for(const name of ['localSearch','remoteSearch','install','displayedEngineDetails','populateEngines'])assert.ok(switcher.includes(`searchModeModel.${name}(`));
  const session=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8');
  assert.match(session,/searchModeModel\.snapshot\(/);assert.match(session,/searchModeModel\.activate\(/);
});
