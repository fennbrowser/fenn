import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const root=new URL('../upstream/firefox/browser/components/',import.meta.url);
const source=name=>readFileSync(new URL(name,root),'utf8').replace(/^import .*;$/gm,'').replaceAll('export class ','class ');
const modelScope=vm.createContext({UrlbarPrefs:{},UrlbarShared:{}});
vm.runInContext(source('urlbar/content/UrlbarSearchModeModel.mjs')+'\nthis.Model=UrlbarSearchModeModel;',modelScope);
const Model=modelScope.Model;

function retainedFixture({includeCurrent=true,isPrivate=false,local=false,disabled=false}={}){
  const normal={id:'normal',name:'Normal'},privateEngine={id:'private',name:'Private'},hidden={id:'hidden',name:'Hidden',hideOneOffButton:true};
  const engines=[normal,privateEngine,hidden],calls=[],prefs={local,disabled};
  const service={getDefault:async()=>{calls.push('normal');return normal;},
    getDefaultPrivate:async()=>{calls.push('private');return privateEngine;},
    getVisibleEngines:async()=>{calls.push('engines');return engines;}};
  const baseScope=vm.createContext({ChromeUtils:{defineESModuleGetters(target){Object.assign(target,{
    SearchService:service,PrivateBrowsingUtils:{isWindowPrivate:window=>window.isPrivate},UrlbarSearchModeModel:Model});}}});
  vm.runInContext(source('search/SearchOneOffs.sys.mjs')+'\nthis.Base=SearchOneOffs;',baseScope);
  const childScope=vm.createContext({SearchOneOffs:baseScope.Base,XPCOMUtils:{declareLazy:()=>({
    UrlbarSearchModeModel:Model,UrlbarPrefs:{get:()=>prefs.local,getScotchBonnetPref:()=>prefs.disabled},
    UrlbarShared:{LOCAL_SEARCH_MODES:[{pref:'local'}]}})}});
  vm.runInContext(source('urlbar/UrlbarSearchOneOffs.sys.mjs')+'\nthis.Child=UrlbarSearchOneOffs;',childScope);
  // Exercise the real retained policy methods without constructing UI in Node.
  const view=Object.create(childScope.Child.prototype);
  Object.defineProperty(view,'window',{value:{isPrivate},configurable:true});
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
