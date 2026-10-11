import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {UrlbarQueryLifecycle} from '../upstream/firefox/browser/components/urlbar/content/UrlbarQueryLifecycle.mjs';

const deferred=()=>Promise.withResolvers();
function fixture({ready=true,failed=false}={}) {
  const init=deferred(),events=[];
  const store={initialized:ready,failed,init:()=>init.promise};
  const completed=deferred();
  const model=new UrlbarQueryLifecycle({engineStore:store,
    dispatch:q=>{events.push(['dispatch',q.id]);return completed.promise;},
    cancel:()=>{events.push(['cancel']);return 'cancelled';},arm:q=>events.push(['arm',q.id])});
  return {model,store,init,events,completed};
}

test('ready dispatch preserves the exact parent promise and arms after synchronous teardown',()=>{
  const f=fixture(),q={id:1};
  assert.equal(f.model.start(q),f.completed.promise);
  assert.deepEqual(f.events,[['dispatch',1],['arm',1]]);
});

test('engine waiting arms immediately, then only dispatches the newest query',async()=>{
  const f=fixture({ready:false}),old={id:1},current={id:2};
  const a=f.model.start(old),b=f.model.start(current);
  assert.deepEqual(f.events,[['arm',1],['arm',2]]);
  f.store.initialized=true;f.init.resolve();f.completed.resolve(current);
  assert.equal(await a,old);assert.equal(await b,current);
  assert.deepEqual(f.events,[['arm',1],['arm',2],['dispatch',2],['arm',2]]);
});

test('cancellation blocks held dispatch but preserves in-flight result acceptance',async()=>{
  const f=fixture({ready:false}),q={id:5},pending=f.model.start(q);
  assert.equal(f.model.cancel(),'cancelled');f.init.resolve();
  assert.equal(await pending,q);assert.deepEqual(f.events,[['arm',5],['cancel']]);
  assert.equal(f.model.acceptsResults(q),true);
  assert.equal(f.model.acceptsResults({id:4}),false);
});

test('discard revokes held queries and late results independently of cancellation',async()=>{
  const f=fixture({ready:false}),q={id:8},pending=f.model.start(q);
  f.model.discard();f.init.resolve();assert.equal(await pending,q);
  assert.equal(f.model.acceptsResults(q),false);
  assert.deepEqual(f.events,[['arm',8]]);
});

test('failed initialization and an already failed store never wedge query or fallback readiness',async()=>{
  const f=fixture({ready:false}),q={id:1};
  const pending=f.model.start(q),fallback=f.model.whenReady();
  f.init.reject(Error('search failed'));f.completed.resolve(q);
  assert.equal(await pending,q);await fallback;
  assert.deepEqual(f.events,[['arm',1],['dispatch',1],['arm',1]]);
  const failed=fixture({ready:false,failed:true});
  failed.store.init=()=>{throw Error('must not retry permanent failure');};
  assert.equal(failed.model.start(q),failed.completed.promise);await failed.model.whenReady();
});

test('a later query can start after cancellation while retaining stale-result rejection',async()=>{
  const f=fixture({ready:false}),old={id:1},next={id:2};
  const a=f.model.start(old);f.model.cancel();const b=f.model.start(next);
  f.init.resolve();f.completed.resolve(next);
  assert.equal(await a,old);assert.equal(await b,next);
  assert.equal(f.model.acceptsResults(old),false);
  assert.equal(f.model.acceptsResults(next),true);
});

test('the real editor listener defers Enter during an engine wait and after old-query teardown',async()=>{
  const keys={DOM_VK_RETURN:13,DOM_VK_DOWN:40,DOM_VK_TAB:9};
  const c=vm.createContext({console,KeyboardEvent:keys,KeyEvent:keys,performance:{now:()=>0},
    setTimeout(){return 1;},clearTimeout(){},UrlbarPrefs:{get:()=>1000},
    UrlbarContentUtils:{getPlatform:()=> 'win'},UrlbarShared:{getLogger:()=>({debug(){}})}});
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarEventBufferer.mjs',import.meta.url),'utf8');
  vm.runInContext(source.replace(/^import .*;$/gm,'').replace('export class UrlbarEventBufferer','this.Buffer = class UrlbarEventBufferer'),c);
  let listener;
  const input={inputField:{addEventListener(){}},view:{isOpen:true,selectedResult:{heuristic:true}},
    controller:{addListener(value){listener=value;},keyEventMovesCaret:()=>false}};
  const buffer=new c.Buffer(input),init=deferred(),enter={keyCode:13};
  const q={id:1,searchString:'query',pendingHeuristicProviders:new Set(),deferUserSelectionProviders:new Set()};
  const model=new UrlbarQueryLifecycle({engineStore:{initialized:false,init:()=>init.promise},
    dispatch:context=>{buffer.onQueryCancelled();assert.equal(buffer.shouldDeferEvent(enter),false);return Promise.resolve(context);},
    cancel:()=>buffer.onQueryCancelled(),arm:context=>listener.onQueryArmed(context)});
  assert.equal(buffer.shouldDeferEvent(enter),false);
  const pending=model.start(q);assert.equal(buffer.shouldDeferEvent(enter),true);
  init.resolve();await pending;assert.equal(buffer.shouldDeferEvent(enter),true);
  buffer.onQueryResults(q);assert.equal(buffer.shouldDeferEvent(enter),false);
});

test('real child controller admits queries and local editor listeners without an input bufferer',async()=>{
  const events=[];
  class Parent {
    setChild(child){this.child=child;}
    startQuery(q){events.push('dispatch');this.child.notify('onQueryCancelled',q);return Promise.resolve(q);}
    cancelQuery(){events.push('cancel');}
    resolveFallbackNavigation(value){return value;}
  }
  class Store {initialized=true;}
  const context=vm.createContext({console,UrlbarQueryLifecycle,SearchEngineStore:Store,
    UrlbarContentUtils:{usesMessagePath:()=>false},UrlbarParentControllerProxy:class{},
    UrlbarShared:{NOTIFICATIONS:{QUERY_ARMED:'onQueryArmed',QUERY_RESULTS:'onQueryResults',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_FINISHED:'onQueryFinished',QUERY_CANCELLED:'onQueryCancelled'}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;}}});
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarChildController.mjs',import.meta.url),'utf8');
  vm.runInContext(source.replace(/^import .*;$/gm,'').replace('export class UrlbarChildController','this.Controller = class UrlbarChildController'),context);
  const input={sapName:'urlbar',isPrivate:false,window:{windowGlobalChild:{getActor:()=>({})}},
    get eventBufferer(){throw Error('DOM bufferer read');}};
  const controller=new context.Controller({input});
  const listener={onQueryArmed:q=>events.push('arm '+q.id),onQueryCancelled:()=>events.push('cancelled'),
    onQueryResults:q=>events.push('results '+q.id),onQueryFinished:q=>events.push('finished '+q.id)};
  controller.addListener(listener);const q={id:2};assert.equal(await controller.startQuery(q),q);
  assert.deepEqual(events,['dispatch','cancelled','arm 2']);events.length=0;
  controller.notify('onQueryResults',{id:1});controller.notify('onQueryFinished',{id:1});
  controller.notify('onQueryResults',q);assert.deepEqual(events,['results 2']);
  controller.discardResults(q);events.length=0;controller.notify('onQueryResults',q);
  assert.deepEqual(events,[]);controller.removeListener(listener);
  await controller.startQuery({id:4});assert.deepEqual(events,['dispatch']);
  assert.equal(await controller.resolveFallbackNavigation('fallback'),'fallback');
});
