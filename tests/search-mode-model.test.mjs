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
    UrlbarShared:{LOCAL_SEARCH_MODES:[local],RESTRICT_TOKENS:{SEARCH:'?'},
      SEARCH_MODE_RESTRICT:new Set(['^','?']),RESULT_TYPE:{RESTRICT:9},
      ICON:{SEARCH_GLASS:'glass'},getResultSourceName:()=> 'history'}});
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
  assert.doesNotMatch(switcher,/nativeSnapshot|nativeActivate|nativeOpened|nativeVisibility/);
  for(const name of ['localSearch','remoteSearch','install','displayedEngineDetails','populateEngines'])assert.ok(switcher.includes(`searchModeModel.${name}(`));
  const session=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8');
  assert.match(session,/searchModeModel\.snapshot\(/);assert.match(session,/searchModeModel\.activate\(/);
});


test('visibility policy covers availability, feature gates, mode exits and alternate SAPs without DOM',()=>{
  const {model,prefs}=fixture();
  for(const available of [false,true]) for(const active of [false,true])
    for(const always of [false,true]) for(const gate of [false,true])
    for(const override of [false,true]) for(const isSearchbarSAP of [false,true])
    for(const smartbar of [false,true]) {
      prefs['unifiedSearchButton.always']=always;
      prefs['searchModeSwitcher.featureGate']=gate;
      prefs['scotchBonnet.enableOverride']=override;
      const state=model.constructor.visibility({available,searchMode:active?{source:1}:null,isSearchbarSAP,smartbar});
      const visible=available && !smartbar && (gate || override || isSearchbarSAP);
      assert.equal(state.modeButtonVisible,visible);
      assert.equal(state.modeExitVisible,visible && active && !always && !isSearchbarSAP);
    }
});

test('preference changes do not erase explicit keyboard reveal or per-tab availability',()=>{
  const {model,prefs}=fixture(),state={available:false,searchMode:null};
  prefs['scotchBonnet.enableOverride']=true;
  prefs['unifiedSearchButton.always']=true;
  assert.equal(model.constructor.visibility(state).modeButtonVisible,false);
  // Only Firefox's availability setter applies always-on and tab restoration.
  state.available=true;
  assert.equal(model.constructor.visibility(state).modeButtonVisible,true);
  prefs['scotchBonnet.enableOverride']=false;
  assert.equal(model.constructor.visibility(state).modeButtonVisible,false);
  prefs['searchModeSwitcher.featureGate']=true;
  assert.equal(model.constructor.visibility(state).modeButtonVisible,true);
});

test('token modes use the current default engine and copy addressbar-only local modes',()=>{
  const {model,services,local}=fixture(),Model=model.constructor;
  const options={isAddressbar:true,engineStore:services.engineStore};
  assert.equal(Model.modeForToken('?',options).engineName,'Engine');
  services.engineStore.default={name:'Replacement'};
  assert.equal(Model.modeForToken('?',options).engineName,'Replacement');
  const mode=Model.modeForToken('^',options);assert.notEqual(mode,local);
  mode.entry='changed';assert.equal(local.entry,undefined);
  assert.equal(Model.modeForToken('^',{...options,isAddressbar:false}),null);
  assert.equal(Model.modeForToken('@unknown',options),null);
  services.engineStore.default=null;
  assert.equal(Model.modeForToken('?',options).engineName,undefined);
});

test('result modes preserve original-engine, action-engine and restriction precedence',()=>{
  const {model,services}=fixture(),Model=model.constructor;
  const modeForToken=token=>Model.modeForToken(token,{isAddressbar:true,engineStore:services.engineStore});
  const mode=(payload,extra={})=>Model.modeForResult({payload,type:1},{modeForToken,...extra});
  assert.equal(mode({}),null);
  assert.equal(mode({engine:'Result'},{actionEngine:'Action'}).engineName,'Result');
  assert.equal(mode({engine:'Result',originalEngine:'Different'}),null);
  assert.equal(mode({engine:'Result',originalEngine:'Different'},{actionEngine:'Action'}).engineName,'Action');
  assert.equal(mode({keyword:'^',engine:'Result'}).source,1);
  assert.equal(mode({keyword:'^'},{actionEngine:'Action'}).engineName,'Action');
  assert.equal(mode({keyword:'^'}).restrictType,'symbol');
  const result={type:9,payload:{keyword:'^',engine:'Result'},providerName:'UrlbarProviderTopSites'};
  assert.equal(Model.modeForResult(result,{modeForToken}).restrictType,'keyword');
  for(const [providerName,dynamicType,entry] of [
    ['UrlbarProviderTopSites',false,'topsites_urlbar'],['UrlbarProviderTabToSearch',false,'tabtosearch'],
    ['UrlbarProviderTabToSearch',true,'tabtosearch_onboard'],['Other',false,'keywordoffer']]) {
    result.providerName=providerName;result.payload.dynamicType=dynamicType;
    assert.equal(Model.modeForResult(result,{modeForToken}).entry,entry);
    assert.equal(Model.modeForResult(result,{modeForToken,entry:'explicit'}).entry,'explicit');
  }
});

test('original inputs adapt token and selected-action facts to shared result-mode policy',()=>{
  const {model,services}=fixture();
  for(const file of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) {
    const source=readFileSync(new URL(file,base),'utf8');
    const methods=['searchModeForToken(token)', '_searchModeForResult(result,'].map(signature=>{
      const start=source.indexOf('  '+signature);return source.slice(start,source.indexOf('\n  /**',start));
    });
    const Binding=new Function('UrlbarSearchModeModel',`return class {#isAddressbar=true; ${methods.join('\n')}}`)(model.constructor);
    const binding=new Binding();binding.controller={engineStore:services.engineStore};binding.view={selectedElement:{dataset:{engine:'Selected'}}};
    const result={payload:{keyword:'^'}};
    assert.equal(binding.searchModeForToken('?').engineName,'Engine');
    const described=binding._searchModeForResult(result,null,{engine:'Native'});
    assert.equal(described.restrictType,'symbol');
    if(file==='UrlbarInputBase.mjs') {
      assert.equal(described.engineName,'Native');
      assert.equal(binding._searchModeForResult(result).engineName,'Selected');
    } else assert.equal(described.source,1,'Smartbar must not acquire action-element overrides');
  }
});


test('result transition checks trimmed keywords before resolving a mode and preserves query trailing space',()=>{
  const Model=fixture().model.constructor,calls=[];
  const result={payload:{keyword:' @engine ',autofillKeyword:'@auto',query:'  words  '}};
  const resolveMode=()=>{calls.push('resolve');return {engineName:'Engine'};};
  assert.equal(Model.resultTransition(result,{value:'unrelated',resolveMode}),null);assert.deepEqual(calls,[]);
  for(const value of ['@engine',' @auto ']) {
    const plan=Model.resultTransition(result,{value,resolveMode});
    assert.equal(plan.value,'words  ');assert.equal(plan.searchMode.isPreview,false);
    assert.equal(plan.storeTypedValue,true);assert.equal(plan.startQuery,true);
    assert.equal(Object.isFrozen(plan.searchMode),true);
  }
  assert.equal(Model.resultTransition(result,{value:'@engine',resolveMode:()=>null}),null);
});

test('result transition preserves preview inference and confirms native actions without forcing a query',()=>{
  const Model=fixture().model.constructor,mode={engineName:'Engine'},result={payload:{}};
  const plan=options=>Model.resultTransition(result,{value:'',checkValue:false,resolveMode:()=>mode,...options});
  const preview=plan({startQuery:false});assert.equal(preview.searchMode.isPreview,undefined);
  assert.equal(preview.value,'');assert.equal(preview.storeTypedValue,false);assert.equal(preview.startQuery,false);
  const action=plan({startQuery:false,nativeAction:true});
  assert.equal(action.searchMode.isPreview,false);assert.equal(action.storeTypedValue,true);assert.equal(action.startQuery,false);
  assert.equal(mode.isPreview,undefined);
});

test('original result-mode binding applies mutable state and text before awaiting the mode query',async()=>{
  const Model=fixture().model.constructor;
  const source=readFileSync(new URL('UrlbarInputBase.mjs',base),'utf8');
  const start=source.indexOf('  maybeConfirmSearchModeFromResult({');
  const end=source.indexOf('\n  /**',start);
  const method=source.slice(start,end).replaceAll('#searchModeApplied','_searchModeApplied');
  const context=vm.createContext({UrlbarSearchModeModel:Model});
  vm.runInContext('this.Binding=class {'+method+'}',context);
  const input=new context.Binding(),calls=[],gate=Promise.withResolvers();
  input.value='@engine';input._searchModeForResult=()=>({engineName:'Engine'});
  Object.defineProperty(input,'searchMode',{set(mode){mode.source=3;calls.push(['mode',mode]);input._searchModeApplied=gate.promise;}});
  input.setValue=value=>{calls.push(['value',value]);input.untrimmedValue=value;};
  input.startQuery=options=>calls.push(['query',options.allowAutofill]);
  assert.equal(input.maybeConfirmSearchModeFromResult({result:{payload:{keyword:'@engine',query:' words'}}}),true);
  assert.deepEqual(calls.map(c=>c[0]),['mode','value']);assert.equal(input.userTypedValue,'words');
  assert.equal(calls[0][1].isPreview,false);gate.resolve();await gate.promise;await Promise.resolve();
  assert.deepEqual(calls.map(c=>c[0]),['mode','value','query']);assert.equal(calls[2][1],false);
});
