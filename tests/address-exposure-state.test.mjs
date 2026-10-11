import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const root=new URL('../upstream/firefox/',import.meta.url);
const read=name=>readFileSync(new URL(name,root),'utf8');
const context=vm.createContext({console});
vm.runInContext(read('browser/base/content/WinUIUrlbarSession.sys.mjs')
  .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);

function fixture() {
  const s=Object.create(context.Session.prototype), exposures=[];
  const a={providerName:'a',exposureTelemetry:true},b={providerName:'b'};
  const browser={currentURI:{spec:'page'}};
  const controller={visibleResults:[],engagementEvent:{addExposure:r=>exposures.push(r)}};
  const view={nativePresentation:s,get visibleResults(){throw Error('View exposure read');},
    set visibleResults(_){throw Error('View exposure write');}};
  Object.assign(s,{input:{view,controller},window:{gBrowser:{selectedBrowser:browser}},
    sessionId:'s',sequence:3,nativeOpen:true,
    active:{browser,page:'page',revision:2,rows:[{id:'a'},{id:'b'}],results:[a,b],
      resultIndexes:[0,1],context:{results:[a,b],deferUserSelectionProviders:new Set(['a','b'])},exposed:new Set()}});
  const request={session:'s',revision:2,sequence:3,ids:['a','b']};
  return {s,controller,exposures,a,b,request};
}

test('validated viewport reports publish controller exposure without reading or writing the view',()=>{
  const f=fixture();
  assert.equal(f.s.reportNativeVisibility(f.request),'visible');
  assert.deepEqual([...f.controller.visibleResults],[f.a,f.b]);
  assert.deepEqual(f.exposures,[f.a]);
  assert.equal(f.s.active.context.deferUserSelectionProviders.size,0);
  f.s.reportNativeVisibility(f.request);assert.equal(f.exposures.length,1);
  f.s.reportNativeVisibility({...f.request,ids:['b']});
  assert.deepEqual([...f.controller.visibleResults],[f.b]);
});

test('stale, duplicate, unknown and hidden IDs cannot replace controller exposure',()=>{
  const f=fixture();f.s.reportNativeVisibility({...f.request,ids:['a']});
  const snapshot=f.controller.visibleResults;
  for(const request of [{...f.request,session:'old'},{...f.request,sequence:2},
    {...f.request,revision:1},{...f.request,ids:['a','a']},{...f.request,ids:['missing']}]) {
    assert.equal(f.s.reportNativeVisibility(request),'stale');
    assert.equal(f.controller.visibleResults,snapshot);
  }
  f.b.isHiddenExposure=true;
  assert.equal(f.s.reportNativeVisibility(f.request),'stale');
  f.b.isHiddenExposure=false;f.s.active.context.results[1]={};
  assert.equal(f.s.reportNativeVisibility(f.request),'stale');
  f.s.nativeOpen=false;
  assert.equal(f.s.reportNativeVisibility(f.request),'stale');
  assert.equal(f.controller.visibleResults,snapshot);
});

test('dismissal revokes current exposure without mutating a captured engagement snapshot',()=>{
  const f=fixture();f.s.reportNativeVisibility(f.request);
  const snapshot=f.controller.visibleResults;
  f.s.suspendNativePresentation();
  assert.equal(f.controller.visibleResults.length,0);
  assert.equal(snapshot.length,2);assert.equal(f.s.nativeOpen,false);
  f.controller.visibleResults=snapshot;
  f.s.cancel=()=>{f.controller.visibleResults=[];};
  f.s.onNativeViewClosed(true);
  assert.equal(f.controller.visibleResults,snapshot);
});

test('engagement snapshot reads controller identities and open state without reading the retained view',()=>{
  const source=read('browser/components/urlbar/content/UrlbarTelemetryUtils.mjs');
  const start=source.indexOf('  static engagementData('),end=source.indexOf('\n  /**',start);
  const c=vm.createContext({});vm.runInContext('this.Helpers=class {'+source.slice(start,end)+'}',c);
  const result={},input={controller:{visibleResults:[result],resultsOpen:true},searchMode:null,getSearchSource:()=> 'urlbar',get view(){throw Error('View read');}};
  const snapshot=c.Helpers.engagementData(input);
  input.controller.visibleResults=[];
  assert.equal(snapshot.visibleResults[0],result);assert.equal(snapshot.viewIsOpen,true);
});

test('provider impressions consume direct or explicit message snapshots without a view',()=>{
  const source=read('browser/components/urlbar/UrlbarProvidersManager.sys.mjs');
  const start=source.indexOf('  notifyEngagementChange('),end=source.indexOf('\n}\n',start);
  const c=vm.createContext({lazy:{logger:{error(){throw Error('Unexpected state');}},
    UrlbarShared:{NOTIFICATIONS:{PROVIDER_ENGAGEMENT:'engagement'}}}});
  vm.runInContext('this.Manager=class {'+source.slice(start,end)+'}',c);
  const manager=new c.Manager(),calls=[];
  const provider={name:'p',tryMethod:(...args)=>calls.push(args)};
  manager.providersByNotificationType={onImpression:[provider],onEngagement:[provider],onAbandonment:[provider],onSearchSessionEnd:[]};
  const result={providerName:'p'},controller={visibleResults:[result],notify(){},get view(){throw Error('View read');}};
  manager.notifyEngagementChange('abandonment',{}, {},controller);
  assert.equal(calls[0][0],'onImpression');assert.equal(calls[0][4][0].result,result);
  assert.equal(calls[1][0],'onAbandonment');calls.length=0;
  manager.notifyEngagementChange('abandonment',{}, {},controller,[]);
  assert.equal(calls.length,0,'explicit empty message snapshot must not fall back');
  const wireResult={providerName:'p'};
  manager.notifyEngagementChange('engagement',{}, {result:wireResult},controller,[wireResult]);
  assert.equal(calls[0][4][0].result,wireResult);assert.equal(calls[1][0],'onEngagement');
});
