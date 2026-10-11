import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const read=name=>readFileSync(new URL('../upstream/firefox/browser/components/urlbar/'+name,import.meta.url),'utf8');
const uri=spec=>({spec,equals:other=>other?.spec===spec});
function fixture(){
  class URIURL extends URL {static fromURI(value){return new URL(value.spec);}}
  const c=vm.createContext({URL:URIURL,ChromeUtils:{defineESModuleGetters(){},defineLazyGetter(){}}});
  vm.runInContext(read('UrlbarSearchTermsPersistence.sys.mjs').replace('export var ','var '),c);
  const service=c.UrlbarSearchTermsPersistence,resets=[];
  service.setPersistenceState=(state,originalURI)=>{resets.push(originalURI);state.persist={originalURI,
    searchTerms:'chosen words',origin:'https://search.test',pathname:'/search',originalEngineName:'engine',isDefaultEngine:true,shouldPersist:null};};
  const facts={enabled:true,originalURI:uri('https://search.test/search?q=chosen'),uri:uri('https://search.test/search?q=chosen'),
    userTypedValue:null,hideSearchTerms:false,dueToTabSwitch:true,isSameDocument:false};
  return {service,resets,facts};
}

test('disabled persistence removes cached state without typed or telemetry effects',()=>{
  const {service}=fixture();
  for(const existing of [false,true]) {
    const state=existing?{persist:{shouldPersist:true}}:{};
    const effect=service.updateState(state,{enabled:false,userTypedValue:'edit'});
    assert.equal(state.persist,undefined);assert.equal(effect.updateAttribute,existing);
    assert.equal(effect.shouldPersist,false);assert.equal(effect.writeUserTypedValue,false);
    assert.equal(effect.userTypedValue,'edit');assert.equal(effect.recordView,false);
  }
});

test('first view, same-document and tab-switch persistence preserve reset and telemetry policy',()=>{
  for(const dueToTabSwitch of [false,true])for(const isSameDocument of [false,true])for(const cached of [false,true]) {
    const {service,resets,facts}=fixture(),state={};
    if(cached)service.setPersistenceState(state,facts.originalURI);resets.length=0;
    const e=service.updateState(state,{...facts,dueToTabSwitch,isSameDocument});
    assert.equal(resets.length,(!isSameDocument&&!dueToTabSwitch)||!cached?1:0);
    assert.equal(e.shouldPersist,true);assert.equal(e.userTypedValue,'chosen words');
    assert.equal(e.writeUserTypedValue,true);assert.equal(e.recordView,!isSameDocument);
    assert.equal(state.persist.shouldPersist,true);
  }
});

test('changed or missing original URI resets cached state even on a tab switch',()=>{
  for(const originalURI of [null,uri('https://search.test/search?q=new')]) {
    const {service,resets,facts}=fixture(),state={};service.setPersistenceState(state,facts.originalURI);resets.length=0;
    service.updateState(state,{...facts,originalURI});assert.deepEqual(resets,[originalURI]);
  }
});

test('revert hides persisted terms without running eligibility and clears only previously persisted text',()=>{
  for(const wasPersisting of [false,true]) {
    const {service,facts}=fixture(),state={};service.setPersistenceState(state,facts.originalURI);
    state.persist.shouldPersist=wasPersisting;service.shouldPersist=()=>{throw Error('hidden eligibility');};
    const e=service.updateState(state,{...facts,hideSearchTerms:true,userTypedValue:'edit'});
    assert.equal(e.shouldPersist,false);assert.equal(e.recordView,false);
    assert.equal(e.writeUserTypedValue,wasPersisting);assert.equal(e.userTypedValue,wasPersisting?null:'edit');
    assert.equal(state.persist.shouldPersist,false);
  }
});

test('real eligibility rejects edited text, different page and changed confirmed mode',()=>{
  for(const reason of ['edit','origin','path','mode','provider']) {
    const {service,facts}=fixture(),state={};service.setPersistenceState(state,facts.originalURI);state.persist.shouldPersist=true;
    if(reason==='edit')facts.userTypedValue='new words';
    if(reason==='origin')facts.uri=uri('https://other.test/search');
    if(reason==='path')facts.uri=uri('https://search.test/images');
    if(reason==='mode')state.searchModes={confirmed:{engineName:'other'}};
    if(reason==='provider'){state.persist.provider={};facts.isSameDocument=true;service.isDefaultPage=()=>false;}
    const e=service.updateState(state,facts);assert.equal(e.shouldPersist,false,reason);
    assert.equal(e.userTypedValue,null);assert.equal(e.writeUserTypedValue,true);
  }
});

test('explicit mode-state update needs no window and retained wrapper applies only attribute effect',()=>{
  const {service}=fixture();let removed=0;
  for(const matching of [true,false]) {
    const state={persist:{shouldPersist:true,originalEngineName:'engine'},searchModes:{confirmed:{engineName:matching?'engine':'other'}}};
    assert.equal(service.updateSearchModeState(state),!matching);
    assert.equal(state.persist.shouldPersist,matching);assert.equal(service.updateSearchModeState(state),false);
  }
  const state={persist:{shouldPersist:true,originalEngineName:'engine'},searchModes:{confirmed:{engineName:'other'}}};
  service.onSearchModeChanged({gBrowser:{selectedBrowser:{}},gURLBar:{getBrowserState:()=>state,removeAttribute:name=>{assert.equal(name,'persistsearchterms');removed++;}}});
  assert.equal(removed,1);service.onSearchModeChanged({gBrowser:{},gURLBar:{getBrowserState:()=>({})}});
  assert.equal(service.updateSearchModeState(null),false);
});

test('retained persistence binding applies service effects and avoids disabled browser reads',()=>{
  const source=read('content/UrlbarInputBase.mjs'),start=source.indexOf('  #handlePersistedSearchTerms({');
  const method=source.slice(start,source.indexOf('\n  /**',start)).replace('#handlePersistedSearchTerms','update');
  const {service,facts}=fixture(),events=[];let enabled=true;
  const Input=new Function('lazy','Glean',`return class {${method}}`)({UrlbarUtils:{isPersistedSearchTermsEnabled:()=>enabled},UrlbarSearchTermsPersistence:service},
    {urlbarPersistedsearchterms:{viewCount:{add:n=>events.push(n)}}});
  const input=new Input();input.window={gBrowser:{selectedBrowser:{originalURI:facts.originalURI},currentURI:facts.uri}};
  input.userTypedValue=null;input.toggleAttribute=(...args)=>events.push(args);
  assert.equal(input.update({state:{},dueToTabSwitch:true,isSameDocument:false,hideSearchTerms:false}),true);
  assert.equal(input.userTypedValue,'chosen words');assert.deepEqual(events,[['persistsearchterms',true],1]);
  enabled=false;Object.defineProperty(input,'window',{get(){throw Error('disabled browser read');}});events.length=0;
  assert.equal(input.update({state:{persist:{}}}),false);assert.deepEqual(events,[['persistsearchterms',false]]);
});
