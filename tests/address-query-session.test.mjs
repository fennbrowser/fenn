import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {UrlbarQueryLifecycle} from '../upstream/firefox/browser/components/urlbar/content/UrlbarQueryLifecycle.mjs';

function fixture({hold=false}={}) {
  const parents=[];
  class Parent {
    constructor(identity){this.identity=identity;this.started=[];parents.push(this);}
    setChild(child){this.child=child;}
    initEngineStore(){if(!hold)this.child.updateEngineStore('init',[],null);}
    startQuery(query){this.started.push(query);return Promise.resolve(query);}
    cancelQuery(){this.cancelled=(this.cancelled??0)+1;}
    destroy(){this.destroyed=(this.destroyed??0)+1;}
  }
  let id=0;
  const sandbox=vm.createContext({console,UrlbarQueryLifecycle,
    UrlbarShared:{getLogger:()=>({debug(){}}),RESULT_SOURCE:{ACTIONS:7},NOTIFICATIONS:{
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:()=>10},UrlbarContentUtils:{usesMessagePath:()=>false},
    UrlbarQueryContext:class {constructor(options){Object.assign(this,options,{id:++id,results:[]});}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;}}});
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarQuerySession']) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${file}.mjs`,import.meta.url),'utf8');
    vm.runInContext(source.replace(/^import .*;$/gm,'').replaceAll('export class ','class ')+`\nthis.${file}=${file};`,sandbox);
  }
  let current=true;
  const context={sapName:'urlbar',isPrivate:true,actor:{browsingContext:{isContent:false}}};
  const session=new sandbox.UrlbarQuerySession({context,isCurrent:()=>current});
  return {session,parent:parents[0],parents,sandbox,context,invalidate(){current=false;}};
}

test('fresh controllers and engine stores use explicit identity with no input or view',async()=>{
  const f=fixture(),child=f.parent.child;
  assert.throws(()=>child.input,/no input/);assert.throws(()=>child.view,/no view/);
  assert.throws(()=>child.setView({}),/cannot attach/);
  assert.equal(child.engineStore.isPrivate,true);
  const q=await f.session.query({isPrivate:false,sapName:'searchbar'},{searchString:'hello'});
  assert.equal(q.isPrivate,true);assert.equal(q.sapName,'urlbar');
  assert.equal(f.parent.started[0],q);
  assert.throws(()=>new f.sandbox.UrlbarChildController({context:{...f.context,actor:{browsingContext:{isContent:true}}}}),/chrome context/);
  f.session.close();f.session.close();assert.equal(f.parent.destroyed,1);
});

test('exposure is query-scoped, copied, identity-validated and revoked by cancellation',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'hello'}),row={};q.results.push(row);
  const visible=[row];f.session.publishPresentation(q.id,visible,true);visible.length=0;
  assert.equal(f.parent.child.visibleResults.length,1);
  for(const [id,rows] of [[q.id-1,[row]],[q.id,[{}]],[q.id,[row,row]]]) {
    assert.throws(()=>f.session.publishPresentation(id,rows,true),/Invalid query presentation/);
  }
  f.session.cancel();assert.equal(f.parent.child.visibleResults.length,0);
  assert.equal(f.parent.child.resultsOpen,false);
  assert.throws(()=>f.session.publishPresentation(q.id,[row],true),/Invalid/);
  f.session.close();
});

test('held queries settle on close without dispatch or late engine revival',async()=>{
  const f=fixture({hold:true}),pending=f.session.query({}, {searchString:'held'});
  f.session.close();await assert.rejects(pending,/closed/);
  assert.equal(f.parent.started.length,0);
  f.parent.child.updateEngineStore('init',[],null);
  assert.equal(f.parent.child.engineStore.initialized,false);
  await assert.rejects(f.session.query({},{}),/closed/);
});

test('supersession and cancellation reject promises even when engine readiness resolves',async()=>{
  const f=fixture({hold:true}),a=f.session.query({}, {searchString:'old'}),b=f.session.query({}, {searchString:'new'});
  f.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(a,/superseded/);assert.equal((await b).searchString,'new');
  assert.equal(f.parent.started.length,1);
  f.session.close();
  const g=fixture({hold:true}),pending=g.session.query({}, {searchString:'cancel'});
  g.session.cancel();g.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(pending,/cancelled/);assert.equal(g.parent.started.length,0);g.session.close();
});

test('scope loss rejects completion and presentation and releases the controller',async()=>{
  const f=fixture(),pending=f.session.query({}, {searchString:'hello'});f.invalidate();
  await assert.rejects(pending,/scope changed/);assert.equal(f.parent.destroyed,1);
  const g=fixture(),q=await g.session.query({}, {searchString:'hello'});g.invalidate();
  assert.throws(()=>g.session.publishPresentation(q.id,[],false),/scope changed/);
  assert.equal(g.parent.destroyed,1);
  assert.throws(()=>new g.sandbox.UrlbarQuerySession({context:g.context,isCurrent:()=>false}),/scope changed/);
  assert.equal(g.parents.length,1);
});

test('scope loss during engine wait suppresses provider dispatch and result-side effects',async()=>{
  const f=fixture({hold:true}),pending=f.session.query({}, {searchString:'held'});
  f.invalidate();
  f.parent.child.notify('onQueryResults',{id:100,firstResultChanged:true,results:[{}]});
  f.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(pending,/scope changed/);assert.equal(f.parent.started.length,0);
  f.parent.child.notify('onQueryResults',{id:100,firstResultChanged:true,results:[{}]});
  assert.equal(f.parent.destroyed,1);
});

test('failed dispatch revokes the current query without cancelling a newer query',async()=>{
  const f=fixture();let failed;
  f.parent.startQuery=query=>{failed=query;return Promise.reject(Error('provider failure'));};
  await assert.rejects(f.session.query({}, {searchString:'failed'}),/provider failure/);
  assert.throws(()=>f.session.publishPresentation(failed.id,[],true),/Invalid/);
  assert.equal(f.parent.child.resultsOpen,false);f.session.close();
  const g=fixture(),gate=Promise.withResolvers();
  await g.parent.child.engineStore.init();
  g.parent.startQuery=()=>gate.promise;
  const old=g.session.query({}, {searchString:'old'}),rejection=assert.rejects(old,/old failure/);
  g.parent.startQuery=query=>Promise.resolve(query);
  const current=await g.session.query({}, {searchString:'new'});
  gate.reject(Error('old failure'));await rejection;
  g.session.publishPresentation(current.id,[],true);assert.equal(g.parent.child.resultsOpen,true);g.session.close();
});

test('retained message-path construction still uses its proxy without resolving a chrome actor',()=>{
  const f=fixture();f.sandbox.UrlbarContentUtils.usesMessagePath=()=>true;
  class Proxy {setChild(child){this.child=child;}}
  f.sandbox.UrlbarParentControllerProxy=Proxy;
  const child=new f.sandbox.UrlbarChildController({input:{sapName:'searchbar',isPrivate:true,
    get window(){throw Error('chrome actor lookup on the message path');}}});
  assert.ok(child.parentController instanceof Proxy);assert.equal(child.engineStore.isPrivate,true);
  assert.equal(child.sapName,'searchbar');f.session.close();
});

test('retained presenter handles cache notifications without rebuilding unrelated state',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  onPresentationCacheInvalidated('),end=source.indexOf('\n  }',start)+4;
  const context=vm.createContext({});vm.runInContext('this.Presenter=class {'+source.slice(start,end)+'}',context);
  const presenter=new context.Presenter(),calls=[];
  presenter.clearTopSitesCache=()=>calls.push('topsites');presenter.clearL10nCache=()=>calls.push('l10n');
  for(const name of ['topsites','l10n','unknown'])presenter.onPresentationCacheInvalidated(name);
  assert.deepEqual(calls,['topsites','l10n']);
});
