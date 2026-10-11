import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const read=name=>readFileSync(new URL('../upstream/firefox/browser/components/urlbar/'+name,import.meta.url),'utf8');

function fixture() {
  const records=[],browser={},builds=[];let reads=0,allowInput=true,viewTime=1200;
  const data={searchMode:null,visibleResults:[{id:1}],viewIsOpen:true},smart={model:'old'};
  const utils={engagementData:input=>input.data,smartbarData:input=>input.smart,
    collectBounceSnapshot:(_event,details,start,visible)=>start?{...details,visibleResults:visible}:null,
    buildRecordedBounce:(snapshot,state,smartbar)=>{
      if(!snapshot)return null;
      const built={metric:'bounce',eventInfo:{source:snapshot.searchSource,open:state.viewIsOpen,model:smartbar.model,
        results:state.visibleResults.map(r=>r.id).join(',')}};builds.push(built);return built;
    }};
  const c=vm.createContext({console,Date,ChromeUtils:{now:()=>Date.now()},
    Glean:{urlbar:new Proxy({}, {get:()=>({set(){},record:event=>records.push({...event})})})},
    lazy:{UrlbarPrefs:{addObserver(){},get:key=>key==='events.bounce.maxSecondsFromLastSearch'?10:true},
      UrlbarTelemetryUtils:utils,SearchService:{defaultEngine:{telemetryId:'engine'}},logger:{info(){}},
      ExtensionUtils:{isExtensionUrl:()=>false},Interactions:{getRecentInteractionsForBrowser:async()=>[
        {created_at:Date.now()+1000,totalViewTime:viewTime}]}}});
  const source=read('UrlbarParentController.sys.mjs');
  vm.runInContext(source.slice(source.indexOf('const gTrackedBounces =')).replaceAll('export ', '')+
    ';this.Recorder=TelemetryEvent;this.trigger=handleBounceEventTrigger;',c);
  const controller={sapName:'urlbar',resolveTargetBrowser:()=>browser,
    browserWindow:{closed:false,isBlankPageURL:()=>false,gBrowser:{currentURI:{spec:'https://example.test/'}}},
    get input(){reads++;if(!allowInput)throw Error('Late input read');return {data,smart};}};
  const recorder=new c.Recorder(controller);
  recorder._startEventInfo={interactionType:'typed',timeStamp:1};
  return {recorder,c,controller,records,data,smart,builds,browser,get reads(){return reads;},
    prohibit(){allowInput=false;},setTime(value){viewTime=value;}};
}

test('direct bounce captures presentation before navigation and never reads a later input',async()=>{
  const f=fixture();await f.recorder.startTrackingBounceEvent(1,{type:'keydown'},{searchSource:'urlbar'});
  assert.equal(f.reads,2);f.data.visibleResults=[];f.data.viewIsOpen=false;f.smart.model='new';f.prohibit();
  await f.c.trigger(f.browser);await f.c.trigger(f.browser);
  assert.equal(f.records.length,1);assert.equal(f.records[0].results,'1');assert.equal(f.records[0].open,true);
  assert.equal(f.records[0].model,'old');assert.equal(f.records[0].view_time,'1.2');assert.equal(f.reads,2);
});

test('explicit bounce needs no input and preserves replacement and view-time filtering',async()=>{
  const f=fixture();f.prohibit();const state={engagementData:f.data,smartbarData:f.smart};
  await f.recorder.startTrackingBounceEvent(1,{}, {searchSource:'urlbar_handoff'},state);
  f.recorder.discard();await f.recorder.startTrackingBounceEvent(1,{}, {searchSource:'urlbar'},state);
  assert.equal(f.records.length,1);assert.equal(f.records[0].sap,'handoff');
  await f.c.trigger(f.browser);assert.equal(f.records.length,1);assert.equal(f.reads,0);
  for(const time of [0,10000,15000]) {
    f.recorder._startEventInfo={};await f.recorder.startTrackingBounceEvent(1,{}, {searchSource:'urlbar'},state);
    f.setTime(time);await f.c.trigger(f.browser);
  }
  assert.equal(f.records.length,1);
});

test('shared bounce builder freezes engagement mode and exposure into event fields',()=>{
  const c=vm.createContext({});
  vm.runInContext(read('content/UrlbarTelemetryUtils.mjs').replace(/^import .*;\r?$/gm,'')
    .replace('export class UrlbarTelemetryUtils','this.Utils=class UrlbarTelemetryUtils'),c);
  const u=c.Utils;u.getInteractionType=(method,start,source,words,mode,previous)=>{
    assert.equal(method,'bounce');assert.equal(previous,'previous');return {interaction:'captured'};};
  u.buildEventInfo=fields=>fields;
  const snapshot={searchSource:'urlbar_persisted',startEventInfo:{},searchWords:['term'],visibleResults:[{id:1}],
    searchMode:null,selIndex:0,selType:'history',windowMode:'private'};
  const mode={source:4},data={searchMode:mode,viewIsOpen:true};
  const built=u.buildRecordedBounce(snapshot,data,{model:'m'},'previous');
  assert.equal(built.searchMode,mode);assert.equal(built.visibleResults,snapshot.visibleResults);
  assert.equal(built.windowMode,'private');assert.equal(built.interaction,'captured');assert.equal(built.model,'m');
  assert.equal(u.buildRecordedBounce(null,data,{},null),null);
});

test('direct smartbar bounce retains its required-location validation at capture time',async()=>{
  const f=fixture();f.controller.sapName='smartbar';f.prohibit();
  const state={engagementData:f.data,smartbarData:f.smart};
  await assert.rejects(f.recorder.startTrackingBounceEvent(1,{}, {searchSource:'smartbar'},state),/location/);
  assert.equal(f.builds.length,0);assert.equal(f.reads,0);
  await f.recorder.startTrackingBounceEvent(1,{}, {searchSource:'smartbar',location:'urlbar'},state);
  await f.c.trigger(f.browser);assert.equal(f.records.length,1);
});
