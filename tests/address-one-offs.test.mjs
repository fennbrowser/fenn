import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const root=new URL('../upstream/firefox/browser/components/',import.meta.url);
const source=name=>readFileSync(new URL(name,root),'utf8').replace(/^import .*;$/gm,'').replaceAll('export class ','class ');
const modelScope=vm.createContext({UrlbarPrefs:{},UrlbarShared:{RESULT_SOURCE:{SEARCH:3}}});
vm.runInContext(source('urlbar/content/UrlbarSearchModeModel.mjs')+'\nthis.Model=UrlbarSearchModeModel;',modelScope);
const Model=modelScope.Model;

function retainedFixture({includeCurrent=true,isPrivate=false,local=false,disabled=false}={}){
  const normal={id:'normal',name:'Normal'},privateEngine={id:'private',name:'Private'},hidden={id:'hidden',name:'Hidden',hideOneOffButton:true};
  const engines=[normal,privateEngine,hidden],calls=[],prefs={local,disabled};
  const service={getEngineByName:name=>engines.find(engine=>engine.name===name),getDefault:async()=>{calls.push('normal');return normal;},
    getDefaultPrivate:async()=>{calls.push('private');return privateEngine;},
    getVisibleEngines:async()=>{calls.push('engines');return engines;}};
  const eventType=kind=>({isInstance:event=>event?.kind===kind});
  const baseScope=vm.createContext({Services:{prefs:{getBoolPref:name=>!!prefs[name]}},
    KeyboardEvent:eventType('keyboard'),MouseEvent:eventType('mouse'),ChromeUtils:{defineESModuleGetters(target){Object.assign(target,{
    SearchService:service,PrivateBrowsingUtils:{isWindowPrivate:window=>window.isPrivate},UrlbarSearchModeModel:Model});}}});
  vm.runInContext(source('search/SearchOneOffs.sys.mjs')+'\nthis.Base=SearchOneOffs;',baseScope);
  const childScope=vm.createContext({SearchOneOffs:baseScope.Base,XPCOMUtils:{declareLazy:()=>({
    SearchService:service,UrlbarSearchModeModel:Model,UrlbarPrefs:{get:()=>prefs.local,getScotchBonnetPref:()=>prefs.disabled},
    UrlbarShared:{LOCAL_SEARCH_MODES:[{pref:'local'}]}})}});
  vm.runInContext(source('urlbar/UrlbarSearchOneOffs.sys.mjs')+'\nthis.Child=UrlbarSearchOneOffs;',childScope);
  // Exercise the real retained policy methods without constructing UI in Node.
  const view=Object.create(childScope.Child.prototype);
  Object.defineProperty(view,'window',{value:{isPrivate,gBrowser:{selectedTab:{isEmpty:false}}},configurable:true});
  Object.defineProperty(view,'container',{value:{getAttribute:()=>includeCurrent?'true':null},configurable:true});
  return {view,Base:baseScope.Base,normal,privateEngine,hidden,engines,calls,prefs,service};
}

test('shared one-off filtering preserves current engine inclusion, names, ordering and hidden buttons',()=>{
  const a={name:'A'},b={name:'B'},hidden={name:'Hidden',hideOneOffButton:true},sameName={name:'A'};
  assert.deepEqual([...Model.oneOffEngines([a,hidden,b,sameName],a,true)],[a,b,sameName]);
  assert.deepEqual([...Model.oneOffEngines([a,hidden,b,sameName],a,false)],[b]);
  for(const [engines,expected] of [[[],true],[[a],true],[[sameName],true],[[b],false],[[a,b],false]]){
    assert.equal(Model.engineOneOffsWillHide(engines,a),expected);
  }
});

test('address one-off preference and local-mode policy has the same precedence as Firefox',()=>{
  for(const engineWillHide of [false,true])for(const disabled of [false,true])for(const localModes of [false,true]){
    assert.equal(Model.urlbarOneOffsWillHide(engineWillHide,{disabled,localModes}),disabled||(!localModes&&engineWillHide));
  }
});

test('retained policy uses normal and private defaults and preserves engine cache initialization',async()=>{
  for(const isPrivate of [false,true])for(const includeCurrent of [false,true]){
    const f=retainedFixture({isPrivate,includeCurrent}),info=await f.view.getEngineInfo();
    assert.equal(info.default,isPrivate?f.privateEngine:f.normal);
    assert.deepEqual([...info.engines],includeCurrent?[f.normal,f.privateEngine]:[isPrivate?f.normal:f.privateEngine]);
    const expected=Model.engineOneOffsWillHide(info.engines,info.default);
    assert.equal(await f.Base.prototype.willHide.call(f.view),expected);
    assert.equal(await f.Base.prototype.willHide.call(f.view),expected);
    assert.deepEqual(f.calls,[isPrivate?'private':'normal','engines']);
  }
});

test('retained address policy initializes engines even when disabled or local modes decide visibility',async()=>{
  for(const disabled of [false,true])for(const local of [false,true]){
    const f=retainedFixture({disabled,local});f.engines.splice(1);
    assert.equal(await f.view.willHide(),disabled||!local);
    assert.deepEqual(f.calls,['normal','engines']);assert.ok(f.view._engineInfo);
    f.prefs.disabled=!disabled;
    assert.equal(await f.view.willHide(),!disabled||!local);
    assert.equal(f.calls.length,2,'preferences must not bypass or repeat engine initialization');
  }
});

test('retained one-off search-service failures still reject without a fabricated visibility result',async()=>{
  const f=retainedFixture({disabled:true,local:true});f.service.getVisibleEngines=async()=>{throw Error('service failed');};
  await assert.rejects(f.view.willHide(),/service failed/);assert.equal(f.view._engineInfo,undefined);
});


test('retained filtering captures current-engine inclusion and default name before awaiting engines',async()=>{
  for(const includeCurrent of [false,true]){
    const f=retainedFixture({includeCurrent});
    f.service.getVisibleEngines=async()=>{
      f.normal.name='Renamed';f.view.container.getAttribute=()=>includeCurrent?null:'true';
      return f.engines;
    };
    const info=await f.view.getEngineInfo();
    assert.deepEqual([...info.engines],[f.normal,f.privateEngine]);
  }
});


test('one-off destinations preserve forced, Alt/preference, empty-tab and mouse-background policy',()=>{
  for(const forceNewTab of [false,true])for(const loadInBackground of [false,true])
  for(const wantsNewTab of [false,true])for(const tabIsEmpty of [false,true])for(const mouseBackground of [false,true]){
    const p=Model.oneOffOpenPolicy({forceNewTab,loadInBackground,wantsNewTab,tabIsEmpty,mouseBackground});
    assert.equal(p.where,forceNewTab||mouseBackground||(wantsNewTab&&!tabIsEmpty)?'tab':'current');
    assert.equal(p.params?.inBackground, (forceNewTab?loadInBackground:mouseBackground)?true:undefined);
    assert.ok(Object.isFrozen(p));if(p.params)assert.ok(Object.isFrozen(p.params));
  }
});

test('retained event adapter preserves modifier precedence, lazy reads and mutable searchbar parameters',()=>{
  for(const newTabPref of [false,true])for(const empty of [false,true])for(const altKey of [false,true]){
    const f=retainedFixture();f.prefs['browser.search.openintab']=newTabPref;
    f.view.window.gBrowser.selectedTab.isEmpty=empty;
    assert.equal(f.view._whereToOpen({kind:'keyboard',altKey}).where,(altKey!==newTabPref)&&!empty?'tab':'current');
  }
  const f=retainedFixture();
  Object.defineProperty(f.view.window.gBrowser,'selectedTab',{get(){throw Error('unnecessary tab read');}});
  assert.equal(f.view._whereToOpen({kind:'keyboard',altKey:false}).where,'current');
  for(const event of [{kind:'mouse',button:1,getModifierState(){throw Error('unnecessary Accel read');}},
    {kind:'mouse',button:0,getModifierState:name=>name==='Accel'}]){
    const p=f.view._whereToOpen(event);assert.equal(p.where,'tab');assert.equal(p.params.inBackground,true);
    p.params.avoidBrowserFocus=true;assert.equal(p.params.avoidBrowserFocus,true);
  }
  f.prefs['browser.tabs.loadInBackground']=true;
  const p=f.view._whereToOpen(new Proxy({},{get(){throw Error('forced path read event');}}),true);
  p.params.avoidBrowserFocus=true;assert.equal(p.where,'tab');
});

test('one-off command decisions distinguish typed engine navigation from local or page-value modes',()=>{
  for(const userTypedSearchString of [false,true])for(const hasEngine of [false,true])
  for(const shiftKey of [false,true])for(const where of ['current','tab','window']){
    const expected=userTypedSearchString&&hasEngine&&(shiftKey||where!=='current')?'navigation':where==='tab'?'new-tab':'query';
    assert.equal(Model.oneOffCommandKind({userTypedSearchString,hasEngine,shiftKey,where}),expected);
  }
  assert.equal(Model.oneOffAllowsAutofill({engineName:'Normal'}),false);
  assert.equal(Model.oneOffAllowsAutofill({source:3}),false);
  assert.equal(Model.oneOffAllowsAutofill({source:2}),true);
});

function commandFixture({where='current',background=false,pageValid=false,value='typed words',engine=true}={}){
  const f=retainedFixture(),calls=[],event={shiftKey:false};
  const newTab={linkedBrowser:{},documentGlobal:{gURLBar:{startQuery:p=>calls.push(['new-query',p])}}};
  const browser={addTrustedTab:url=>{calls.push(['tab',url]);return newTab;}};
  const input={value,getAttribute:()=>pageValid?'valid':'invalid',window:{gBrowser:browser},
    controller:{engagementEvent:{discard:()=>calls.push(['discard'])}},
    startQuery:p=>calls.push(['query',p]),handleNavigation:p=>calls.push(['navigation',p]),
    setSearchMode:(mode,target)=>calls.push(['mode',mode,target]),select:()=>calls.push(['select'])};
  const button={engine:engine?f.normal:null,classList:{contains:()=>false},doCommand:()=>calls.push(['command'])};
  for(const [name,value] of Object.entries({input,view:{oneOffSearchButtons:{settingsButton:{}}},selectedButton:button}))
    Object.defineProperty(f.view,name,{value,writable:true,configurable:true});
  f.view._whereToOpen=()=>({where,params:background?{inBackground:true}:undefined});
  const mode=engine?{engineName:'Normal',entry:'oneoff'}:{source:2,entry:'oneoff'};
  return {...f,calls,event,newTab,browser,input,button,mode};
}

test('retained one-off dispatch preserves navigation, mode queries and new-tab effects',()=>{
  for(const where of ['current','tab','window'])for(const engine of [false,true])
  for(const pageValid of [false,true])for(const shiftKey of [false,true])for(const background of [false,true]){
    const f=commandFixture({where,engine,pageValid,background});f.event.shiftKey=shiftKey;
    f.view.handleSearchCommand(f.event,f.mode);
    const navigate=engine&&!pageValid&&(shiftKey||where!=='current');
    if(navigate){
      assert.equal(f.calls.length,1);const [kind,args]=f.calls[0];assert.equal(kind,'navigation');
      assert.equal(args.event,f.event);assert.equal(args.oneOffParams.engine,f.normal);assert.equal(args.oneOffParams.openWhere,where);
    } else if(where==='tab'){
      assert.deepEqual(f.calls.map(c=>c[0]),background?['tab','mode']:['tab','mode','new-query']);
      assert.equal(f.mode.isPreview,false);assert.equal(f.calls[1][2],f.newTab.linkedBrowser);
      assert.equal(f.newTab.linkedBrowser.userTypedValue,pageValid?undefined:'typed words');
      assert.equal(f.browser.selectedTab,background?undefined:f.newTab);
    } else {
      assert.equal(f.input.searchMode,f.mode);assert.deepEqual(f.calls.map(c=>c[0]),where==='current'?['query']:['query','select']);
    }
    for(const [kind,params] of f.calls)if(kind==='query'||kind==='new-query'){
      assert.equal(params.allowAutofill,!engine);assert.equal(params.event,f.event);
    }
    assert.equal(f.view.selectedButton,null);
  }
});

test('settings and add-engine one-offs retain their special command/discard path',()=>{
  for(const settings of [false,true]){
    const f=commandFixture();
    if(settings)f.view.view.oneOffSearchButtons.settingsButton=f.button;else f.button.classList.contains=()=>true;
    f.view._whereToOpen=()=>{throw Error('special command inspected opening policy');};
    f.view.handleSearchCommand(f.event,f.mode);
    assert.deepEqual(f.calls.map(c=>c[0]),['discard','command']);assert.equal(f.view.selectedButton,null);
  }
});
