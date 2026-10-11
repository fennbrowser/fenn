import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {UrlbarQueryLifecycle} from '../upstream/firefox/browser/components/urlbar/content/UrlbarQueryLifecycle.mjs';

const sharedPolicy=vm.createContext({URL,UrlbarContentUtils:{},UrlbarPrefs:{}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarShared.mjs',import.meta.url),'utf8')
  .replace(/^import .*;$/gm,'').replace('export const UrlbarShared','var UrlbarShared'),sharedPolicy);

function fixture({hold=false,uninitializedResults=false}={}) {
  const parents=[];
  const prefs={"experimental.hideHeuristic":false};
  class Parent {
    constructor(identity){this.identity=identity;this.started=[];parents.push(this);}
    setChild(child){this.child=child;}
    initEngineStore(){if(!hold)this.child.updateEngineStore('init',[],null);}
    startQuery(query){query.results??=[];this.started.push(query);return Promise.resolve(query);}
    cancelQuery(){this.cancelled=(this.cancelled??0)+1;}
    destroy(){this.destroyed=(this.destroyed??0)+1;}
  }
  let id=0;
  const sandbox=vm.createContext({console,URL,UrlbarQueryLifecycle,
    UrlbarShared:{stripPrefixAndTrim:sharedPolicy.UrlbarShared.stripPrefixAndTrim,getLogger:()=>({debug(){}}),isPasteEvent:e=>e.inputType?.startsWith('insertFromPaste') || e.inputType==='insertFromYank',RESULT_TYPE:sharedPolicy.UrlbarShared.RESULT_TYPE,RESULT_SOURCE:sharedPolicy.UrlbarShared.RESULT_SOURCE,canAutofillURL:sharedPolicy.UrlbarShared.canAutofillURL.bind(sharedPolicy.UrlbarShared),NOTIFICATIONS:{
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:key=>prefs[key] ?? 10},UrlbarContentUtils:{usesMessagePath:()=>false,getDisplaySpec:spec=>spec,getFixupPrimitives:()=>({})},
    UrlbarQueryContext:class {constructor(options){Object.assign(this,options,{id:++id,results:uninitializedResults?undefined:[]});}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarUrlValue.mjs',import.meta.url),'utf8')
    .replace(/^import .*;$/gm,'').replaceAll('export function ','function '),sandbox);
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarQuerySession','UrlbarAutofillModel','UrlbarResultPresentation','UrlbarEditSession']) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${file}.mjs`,import.meta.url),'utf8');
    vm.runInContext(source.replace(/^import .*;$/gm,'').replaceAll('export class ','class ')+`\nthis.${file}=${file};`,sandbox);
  }
  let current=true;
  const context={sapName:'urlbar',isPrivate:true,actor:{browsingContext:{isContent:false}}};
  const session=new sandbox.UrlbarQuerySession({context,isCurrent:()=>current});
  return {session,parent:parents[0],parents,sandbox,context,prefs,invalidate(){current=false;}};
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

test('streaming updates preserve context identity and suppress stale, cancelled and out-of-scope callbacks',async()=>{
  const f=fixture(),events=[];await f.parent.child.engineStore.init();
  let query;const gate=Promise.withResolvers();
  f.parent.startQuery=q=>{query=q;return gate.promise;};
  const pending=f.session.query({}, {searchString:'stream'},e=>{if(e.kind!=='prepared')events.push(e);});
  for(const name of ['onFirstResult','onQueryResults','onQueryFinished'])f.parent.child.notify(name,query);
  assert.deepEqual(events.map(e=>e.kind),['first-result','results','finished']);
  assert.ok(events.every(e=>e.query===query && Object.isFrozen(e)));
  f.parent.child.notify('onQueryResults',{...query});assert.equal(events.length,3);
  f.invalidate();f.parent.child.notify('onQueryResults',query);assert.equal(events.length,3);
  gate.resolve(query);await assert.rejects(pending,/scope changed/);
});

test('a first-result callback can replace the query without leaking the old result batch',async()=>{
  const f=fixture(),events=[];await f.parent.child.engineStore.init();
  let first,second,newer;
  f.parent.startQuery=q=>{if(!first)first=q;else second=q;return Promise.resolve(q);};
  const old=f.session.query({}, {searchString:'old'},e=>{
    if(e.kind==='prepared')return;
    events.push('old '+e.kind);
    newer=f.session.query({}, {searchString:'new'},n=>{if(n.kind!=='prepared')events.push('new '+n.kind);});
  });
  f.parent.child.notify('onFirstResult',first);
  f.parent.child.notify('onQueryResults',first);f.parent.child.notify('onQueryFinished',first);
  f.parent.child.notify('onQueryResults',second);
  await assert.rejects(old,/superseded/);await newer;
  assert.deepEqual(events,['old first-result','new results']);f.session.close();
});

async function editorFixture(options) {
  const f=fixture(options);f.session.close();const updates=[];
  const editor=new f.sandbox.UrlbarEditSession({context:f.context,isCurrent:()=>!f.invalid,
    onUpdate:update=>updates.push(update)});
  const parent=f.parents.at(-1);await parent.child.engineStore.init();
  return {...f,editor,parent,updates,loseScope(){f.invalid=true;}};
}

test('freezing stops providers once, retains identities and exposure, and suppresses synchronous and late updates',async()=>{
  const f=fixture();await f.parent.child.engineStore.init();
  const events=[],gate=Promise.withResolvers();let query;
  f.parent.startQuery=q=>{query=q;return gate.promise;};
  const pending=f.session.query({}, {searchString:'freeze'},e=>events.push(e.kind));
  const first={payload:{}},late={payload:{}};query.results=[first];
  f.parent.child.notify('onFirstResult',query);
  f.session.publishPresentation(query.id,[first],true);
  f.parent.cancelQuery=()=>{f.parent.cancelled=(f.parent.cancelled??0)+1;f.parent.child.notify('onQueryFinished',query);};
  const frozen=f.session.freezeResults(query.id);
  assert.ok(Object.isFrozen(frozen));assert.ok(Object.isFrozen(frozen.results));
  assert.equal(frozen.results[0],first);assert.equal(f.parent.cancelled,1);
  assert.equal(f.parent.child.visibleResults[0],first);assert.equal(f.parent.child.resultsOpen,true);
  query.results=[late];f.parent.child.notify('onQueryResults',query);
  query.results=[];f.parent.child.notify('onQueryFinished',query);
  assert.equal(f.session.freezeResults(query.id),frozen);assert.equal(f.parent.cancelled,1);
  assert.deepEqual(events,['prepared','first-result']);
  assert.equal(f.session.ownsResult(query.id,first),true);assert.equal(f.session.ownsResult(query.id,late),false);
  f.session.publishPresentation(query.id,[first],true);
  assert.throws(()=>f.session.publishPresentation(query.id,[late],true),/Invalid/);
  gate.resolve(query);assert.equal(await pending,query);
  f.session.cancel();assert.equal(f.session.ownsResult(query.id,first),false);
  assert.throws(()=>f.session.freezeResults(query.id),/not ready/);f.session.close();
});

test('freezing rejects unready or stale queries without interrupting valid work and new queries revoke retained results',async()=>{
  const f=fixture({uninitializedResults:true}),events=[];
  const q=await f.session.query({}, {searchString:'ready'},e=>{
    events.push(e.kind);assert.throws(()=>f.session.freezeResults(e.query.id),/not ready/);
  });
  assert.throws(()=>f.session.freezeResults(q.id),/not ready/);assert.equal(f.parent.cancelled,undefined);
  q.results=[{payload:{}}];assert.throws(()=>f.session.freezeResults(q.id+1),/not ready/);
  const frozen=f.session.freezeResults(q.id);
  const next=await f.session.query({}, {searchString:'replacement'},e=>events.push('next '+e.kind));
  assert.equal(f.session.ownsResult(q.id,frozen.results[0]),false);
  next.results=[{payload:{}}];f.parent.child.notify('onQueryResults',next);
  assert.deepEqual(events,['prepared','next prepared','next results']);
  f.session.freezeResults(next.id);f.invalidate();
  assert.throws(()=>f.session.ownsResult(next.id,next.results[0]),/scope changed/);
  assert.equal(f.parent.destroyed,1);
});

test('reentrant cancellation cannot return a frozen descriptor for a replaced query',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'old'});q.results=[{payload:{}}];
  let newer;f.parent.cancelQuery=()=>{newer=f.session.query({}, {searchString:'new'});};
  assert.throws(()=>f.session.freezeResults(q.id),/superseded/);
  const current=await newer;assert.equal(f.session.ownsQuery(current.id),true);
  assert.equal(f.session.ownsResult(q.id,q.results[0]),false);
  f.parent.cancelQuery=()=>{};f.session.close();
});

test('editor freeze revokes completion authority without writing text and keeps current result descriptions available',async()=>{
  const f=await completionFixture(),snapshot=f.editor.snapshot;
  assert.throws(()=>f.editor.freezeResults(0,f.query.id),/Stale/);
  assert.throws(()=>f.editor.freezeResults(1,f.query.id+1),/not ready/);
  assert.equal(f.editor.getCompletion(f.proposal.token),f.proposal);
  const frozen=f.editor.freezeResults(1,f.query.id);
  assert.equal(frozen.results[0],f.result);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),false);
  f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH;f.result.payload.query='retained';
  assert.equal(f.editor.describeResultInput(1,f.query.id,f.result).value,'retained');
  await f.editor.edit({},edit(2,'new'));
  assert.throws(()=>f.editor.describeResultInput(2,f.query.id,f.result),/Stale/);
  await f.editor.edit({},edit(3,'composing',{composing:true}));
  assert.throws(()=>f.editor.freezeResults(3,f.query.id),/Stale/);f.editor.close();
});
const edit=(revision,text,extra={})=>({revision,text,selectionStart:text.length,selectionEnd:text.length,...extra});

test('independent edits validate UTF-16 selection and monotonically increasing revision without cancelling valid work',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'a😀'));
  assert.equal(f.editor.snapshot.selectionEnd,3);assert.ok(Object.isFrozen(f.editor.snapshot));
  const cancels=f.parent.cancelled;
  for(const bad of [edit(1,'stale'),edit(2,'a',{selectionEnd:2}),edit(2,'a',{selectionStart:-1}),edit(2,'a',{pasteLength:-1})]) {
    await assert.rejects(f.editor.edit({},bad),/Invalid/);
  }
  assert.equal(f.parent.cancelled,cancels);assert.equal(f.editor.snapshot.revision,1);
  assert.throws(()=>f.editor.select(0,0,0),/Stale/);
  const selected=f.editor.select(1,1,3,'backward');assert.equal(selected.selectionRevision,1);
  assert.equal(selected.selectionDirection,'backward');f.editor.close();
});

test('input intent and selection gate autofill and paste metadata reaches the shared query model',async()=>{
  const f=await editorFixture();let revision=0;
  for(const [inputType,allow] of [['insertText',true],['deleteContentBackward',false],['historyUndo',false],['insertFromPaste',false],['insertFromYank',false]]) {
    const q=await f.editor.edit({},edit(++revision,'long paste text',{inputType,pasteLength:20}));
    assert.equal(q.allowAutofill,allow);
    assert.equal(q.prohibitRemoteResults,['insertFromPaste','insertFromYank'].includes(inputType));
  }
  assert.equal((await f.editor.edit({},edit(++revision,'middle',{selectionStart:2,selectionEnd:2}))).allowAutofill,false);
  const started=f.parent.started.length;
  assert.equal(await f.editor.edit({},edit(++revision,'composing',{composing:true})),null);
  assert.equal(f.parent.started.length,started);
  assert.throws(()=>f.editor.publishPresentation(revision,1,[],true),/Stale/);
  await f.editor.edit({},edit(++revision,'committed'));assert.equal(f.parent.started.length,started+1);f.editor.close();
});

test('streamed completion proposals use live selection, immutable stamps and never overwrite observed text',async()=>{
  const f=await editorFixture();let query;const gate=Promise.withResolvers();
  f.parent.startQuery=q=>{query=q;return gate.promise;};
  const pending=f.editor.edit({},edit(1,'exam'));
  const result={payload:{},autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}};
  query.results.push(result);f.parent.child.notify('onFirstResult',query);
  const proposed=f.updates[0];assert.equal(proposed.completion.value,'example.test/');
  assert.equal(proposed.completion.selectionDirection,'backward');assert.ok(Object.isFrozen(proposed.completion));
  assert.equal(f.editor.snapshot.text,'exam');assert.equal(proposed.edit.selectionRevision,0);
  f.editor.select(1,1,1);f.parent.child.notify('onFirstResult',query);
  assert.equal(f.updates.at(-1).completion,null);assert.equal(f.updates.at(-1).edit.selectionRevision,1);
  f.editor.select(1,4,4);result.payload.keyword='alias';f.parent.child.notify('onFirstResult',query);
  assert.equal(f.updates.at(-1).completion,null,'keyword transitions are not silently emulated');
  gate.resolve(query);await pending;f.editor.publishPresentation(1,query.id,[result],true);
  f.editor.cancel();assert.throws(()=>f.editor.publishPresentation(1,query.id,[result],true),/Invalid/);f.editor.close();
});

test('new edits and scope loss revoke older streaming publications and presentation',async()=>{
  const f=await editorFixture();const old=await f.editor.edit({},edit(1,'old'));
  const current=await f.editor.edit({},edit(2,'new'));
  f.parent.child.notify('onQueryResults',old);assert.equal(f.updates.length,0);
  f.parent.child.notify('onQueryResults',current);assert.equal(f.updates.length,1);
  assert.equal(f.updates[0].edit.revision,2);
  assert.throws(()=>f.editor.publishPresentation(1,old.id,[],true),/Stale/);
  f.loseScope();f.parent.child.notify('onQueryResults',current);assert.equal(f.updates.length,1);
  assert.throws(()=>f.editor.select(2,0,0),/scope changed/);assert.equal(f.parent.destroyed,1);
  await assert.rejects(f.editor.edit({},edit(3,'closed')),/closed/);
});

async function completionFixture() {
  const f=await editorFixture();
  const query=await f.editor.edit({},edit(1,'exam'));
  const result={payload:{},autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}};
  query.results.push(result);f.parent.child.notify('onFirstResult',query);
  return {...f,query,result,proposal:f.updates.at(-1).completion};
}
const observed=p=>({text:p.value,selectionStart:p.selectionStart,selectionEnd:p.selectionEnd,
  selectionDirection:p.selectionDirection});

test('completion acknowledgment observes exactly one host write without issuing a new query',async()=>{
  const f=await completionFixture(),p=f.proposal,count=f.parent.started.length;
  assert.equal(f.editor.getCompletion(p.token),p);
  assert.equal(f.editor.acknowledgeCompletion(p.token,{...observed(p),text:'different'}),false);
  assert.equal(f.editor.snapshot.text,'exam');
  assert.equal(f.editor.acknowledgeCompletion(p.token,observed(p)),true);
  assert.equal(f.editor.snapshot.text,p.value);assert.equal(f.editor.snapshot.revision,1);
  assert.equal(f.editor.snapshot.selectionRevision,1);assert.equal(f.parent.started.length,count);
  assert.equal(f.editor.getCompletion(p.token),null);
  assert.equal(f.editor.acknowledgeCompletion(p.token,observed(p)),false);
  f.parent.child.notify('onFirstResult',f.query);
  assert.ok(f.updates.at(-1).completion,'acknowledged suffix is eligible under shared placeholder admission');
  f.editor.close();
});

test('same-query replacement, in-place mutations and removed results revoke completion authority',async()=>{
  for(const mutate of [f=>{f.result.autofill.value='changed.test/';},f=>{f.query.results=[];},
    f=>{f.query.results=[{...f.result},f.result];},f=>{f.result.payload.keyword='alias';}]) {
    const f=await completionFixture();mutate(f);
    assert.equal(f.editor.getCompletion(f.proposal.token),null);
    assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),false);
    assert.equal(f.editor.snapshot.text,'exam');f.editor.close();
  }
  const f=await completionFixture(),old=f.proposal;
  f.parent.child.notify('onFirstResult',f.query);const current=f.updates.at(-1).completion;
  assert.notEqual(old.token,current.token);assert.equal(f.editor.getCompletion(old.token),null);
  assert.equal(f.editor.getCompletion(current.token),current);f.editor.close();
});

test('edit, caret, cancellation, scope and cross-session tokens cannot acknowledge an old proposal',async()=>{
  for(const action of [f=>f.editor.select(1,4,4),f=>f.editor.cancel(),
    f=>f.editor.edit({},edit(2,'new'))]) {
    const f=await completionFixture();await action(f);
    assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),false);f.editor.close();
  }
  const a=await completionFixture(),b=await completionFixture();
  assert.equal(b.editor.acknowledgeCompletion(a.proposal.token,observed(a.proposal)),false);
  assert.equal(b.editor.getCompletion(b.proposal.token),b.proposal);a.editor.close();b.editor.close();
  const f=await completionFixture();f.loseScope();
  assert.throws(()=>f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),/scope changed/);
  assert.equal(f.parent.destroyed,1);
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


test('a later non-autofill first result proposes exact typed-text restoration without requery',async()=>{
  const f=await completionFixture();
  assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),true);
  const count=f.parent.started.length;
  f.query.results=[{payload:{}}];f.parent.child.notify('onFirstResult',f.query);
  const restore=f.updates.at(-1).completion;
  assert.equal(restore.kind,'restore');assert.equal(restore.value,'exam');
  assert.equal(restore.selectionStart,4);assert.equal(restore.selectionEnd,4);
  assert.equal(restore.selectionDirection,'none');
  assert.equal(f.editor.snapshot.text,'example.test/','proposal must not observe its own write');
  assert.equal(f.editor.getCompletion(restore.token),restore);
  assert.equal(f.editor.acknowledgeCompletion(restore.token,observed(restore)),true);
  assert.equal(f.editor.snapshot.text,'exam');assert.equal(f.editor.snapshot.selectionRevision,2);
  assert.equal(f.parent.started.length,count);
  assert.equal(f.editor.acknowledgeCompletion(restore.token,observed(restore)),false);
  f.parent.child.notify('onFirstResult',f.query);assert.equal(f.updates.at(-1).completion,null);
  f.editor.close();
});

test('restoration never claims an unacknowledged suffix or intervening caret/text edit',async()=>{
  for(const action of ['unacknowledged','caret','edit','keyword']) {
    const f=await completionFixture();
    if(action!=='unacknowledged')f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    if(action==='caret')f.editor.select(1,2,2);
    if(action==='edit')await f.editor.edit({},edit(2,'example.test/ '));
    const before=f.updates.length;
    f.query.results=[{payload:action==='keyword'?{keyword:'alias'}:{}}];
    f.parent.child.notify('onFirstResult',f.query);
    assert.ok(f.updates.slice(before).every(u=>u.completion===null),action);
    assert.equal(f.editor.snapshot.text,action==='unacknowledged'?'exam':action==='edit'?'example.test/ ':'example.test/');
    f.editor.close();
  }
});

test('restoration authority rejects changed provider data, replacement and pending selection',async()=>{
  for(const mutate of [f=>{f.query.results[0].autofill=f.result.autofill;},
    f=>{f.query.results[0].payload.keyword='alias';},f=>{f.query.results=[{payload:{}}];},
    f=>f.editor.select(1,4,13,'backward'),f=>f.editor.cancel()]) {
    const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    f.query.results=[{payload:{}}];f.parent.child.notify('onFirstResult',f.query);
    const restore=f.updates.at(-1).completion;mutate(f);
    assert.equal(f.editor.getCompletion(restore.token),null);
    assert.equal(f.editor.acknowledgeCompletion(restore.token,observed(restore)),false);
    assert.equal(f.editor.snapshot.text,'example.test/');f.editor.close();
  }
});


test('prepared query identity permits cancellation or replacement before provider dispatch',async()=>{
  const f=fixture();await f.parent.child.engineStore.init();
  await assert.rejects(f.session.query({}, {searchString:'cancel'},update=>{
    assert.equal(update.kind,'prepared');assert.equal(f.session.ownsQuery(update.query.id),true);
    f.session.cancel();
  }),/superseded or cancelled/);
  assert.equal(f.parent.started.length,0);
  let replacement;
  await assert.rejects(f.session.query({}, {searchString:'replace'},()=>{
    replacement=f.session.query({}, {searchString:'current'});
  }),/superseded or cancelled/);
  const current=await replacement;
  assert.deepEqual(f.parent.started,[current]);f.session.close();
});

test('acknowledged placeholders are proposed before providers and restored without querying the suffix',async()=>{
  const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
  const gate=Promise.withResolvers();let query;
  f.parent.startQuery=q=>{query=q;return gate.promise;};
  const pending=f.editor.edit({},edit(2,'examp'));
  const update=f.updates.at(-1),p=update.completion;
  assert.equal(update.kind,'prepared');assert.equal(update.query,query);
  assert.equal(p.kind,'placeholder');assert.equal(p.value,'example.test/');
  assert.equal(p.selectionStart,5);assert.equal(query.searchString,'examp');
  assert.equal(f.editor.snapshot.text,'examp');
  assert.equal(f.editor.getCompletion(p.token),p);
  assert.equal(f.editor.acknowledgeCompletion(p.token,observed(p)),true);
  query.results=[{payload:{}}];f.parent.child.notify('onFirstResult',query);
  const restore=f.updates.at(-1).completion;assert.equal(restore.kind,'restore');
  assert.equal(restore.value,'examp');f.editor.acknowledgeCompletion(restore.token,observed(restore));
  assert.equal(f.editor.snapshot.text,'examp');
  gate.resolve(query);await pending;f.editor.close();
});

test('result arrival revokes speculative tokens and replaces an acknowledged prediction',async()=>{
  for(const acknowledged of [false,true]) {
    const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    const q=await f.editor.edit({},edit(2,'examp')),p=f.updates.at(-1).completion;
    if(acknowledged)f.editor.acknowledgeCompletion(p.token,observed(p));
    q.results=[{payload:{},autofill:{value:'example.other/',selectionStart:5,selectionEnd:14}}];
    assert.equal(f.editor.getCompletion(p.token),null,'results invalidate an unpublished prediction too');
    f.parent.child.notify('onFirstResult',q);
    const actual=f.updates.at(-1).completion;assert.equal(actual.kind,'autofill');
    assert.equal(actual.value,'example.other/');assert.equal(f.editor.getCompletion(actual.token),actual);
    f.editor.acknowledgeCompletion(actual.token,observed(actual));
    assert.equal(f.editor.snapshot.text,'example.other/');f.editor.close();
  }
});

test('speculative reuse obeys input intent, search mode, caret and composition gates',async()=>{
  for(const extra of [{inputType:'deleteContentBackward'},{inputType:'historyUndo'},
    {inputType:'historyRedo'},{inputType:'insertFromPaste'},
    {selectionStart:1,selectionEnd:1},{composing:true}]) {
    const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    const before=f.updates.length;
    const q=await f.editor.edit({},edit(2,'examp',extra));
    assert.ok(!q || !q.allowAutofill);assert.equal(f.updates.length,before);f.editor.close();
  }
  for(const state of [{searchMode:{engineName:'engine'}},
    {searchMode:{source:sharedPolicy.UrlbarShared.RESULT_SOURCE.SEARCH}}]) {
    const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    const before=f.updates.length,q=await f.editor.edit(state,edit(2,'examp'));
    assert.equal(q.allowAutofill,false);assert.equal(f.updates.length,before);f.editor.close();
  }
  const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
  await f.editor.edit({},edit(2,'exam',{inputType:'deleteContentBackward'}));
  await f.editor.edit({},edit(3,'examp'));
  assert.equal(f.updates.at(-1).completion.kind,'placeholder','delete suppresses application, not the reusable candidate');
  f.editor.close();
});

test('only acknowledged candidates survive edits and cancellation, selection or composition clear reuse',async()=>{
  const unack=await completionFixture(),before=unack.updates.length;
  await unack.editor.edit({},edit(2,'examp'));assert.equal(unack.updates.length,before);unack.editor.close();
  for(const revoke of [f=>f.editor.cancel(),f=>f.editor.select(1,4,13,'backward'),
    f=>f.editor.edit({},edit(2,'examp',{composing:true})),f=>f.editor.edit({},edit(2,'other'))]) {
    const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
    await revoke(f);const before=f.updates.length;
    await f.editor.edit({},edit(3,'examp'));assert.equal(f.updates.length,before);f.editor.close();
  }
  const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
  await f.editor.edit({},edit(2,'examp'));const p=f.updates.at(-1).completion;
  f.editor.select(2,1,1);assert.equal(f.editor.getCompletion(p.token),null);
  assert.equal(f.editor.acknowledgeCompletion(p.token,observed(p)),false);f.editor.close();
});


test('speculation validates before real QueryContext results have been initialized',async()=>{
  const f=await editorFixture({uninitializedResults:true});
  const first=await f.editor.edit({},edit(1,'exam'));
  first.results=[{payload:{},autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}}];
  f.parent.child.notify('onFirstResult',first);
  const p=f.updates.at(-1).completion;f.editor.acknowledgeCompletion(p.token,observed(p));
  let prepared=false;
  f.updates.push=update=>{
    assert.equal(update.kind,'prepared');assert.equal(update.query.results,undefined);
    assert.equal(f.editor.getCompletion(update.completion.token),update.completion);
    assert.equal(f.editor.acknowledgeCompletion(update.completion.token,observed(update.completion)),true);
    prepared=true;
  };
  const q=await f.editor.edit({},edit(2,'examp'));
  assert.equal(prepared,true);assert.equal(q.searchString,'examp');
  assert.equal(f.editor.snapshot.text,'example.test/');f.editor.close();
});


test('independent updates publish shared defaults without mutating observed text or query results',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'words'));
  const first={heuristic:true,type:sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH,payload:{}};
  q.results=[first];q.trimmedSearchString='words';
  f.parent.child.notify('onQueryResults',q);
  assert.equal(f.updates.at(-1).defaultSelection.kind,'select-first');
  f.prefs['experimental.hideHeuristic']=true;f.parent.child.notify('onQueryResults',q);
  const associated=f.updates.at(-1).defaultSelection;
  assert.equal(associated.kind,'associate-first');assert.equal(associated.valueResult,first);
  assert.ok(Object.isFrozen(associated));assert.equal(f.editor.snapshot.text,'words');
  const offer={payload:{providesSearchMode:true}};q.results=[offer];q.trimmedSearchString='@eng';
  f.parent.child.notify('onQueryResults',q);assert.equal(f.updates.at(-1).defaultSelection.valueResult,offer);
  q.trimmedSearchString='@';f.parent.child.notify('onQueryResults',q);
  assert.equal(f.updates.at(-1).defaultSelection.kind,'none');
  const count=f.updates.length;f.editor.cancel();f.parent.child.notify('onQueryResults',q);
  assert.equal(f.updates.length,count);f.editor.close();
});


test('independent result input descriptions require current edit, query and exact result identity',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  const result={type:sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH,payload:{keyword:'@engine',suggestion:'suggestion',query:'typed'}};
  q.results=[result];const count=f.parent.started.length;
  const text=f.editor.describeResultInput(1,q.id,result);
  assert.equal(text.value,'@engine suggestion');assert.ok(Object.isFrozen(text));
  assert.equal(f.editor.snapshot.text,'typed');assert.equal(f.parent.started.length,count);
  for(const args of [[0,q.id,result],[1,q.id+1,result],[1,q.id,{...result}]]) {
    assert.throws(()=>f.editor.describeResultInput(...args),/Stale/);
  }
  result.payload.suggestion='updated';assert.equal(f.editor.describeResultInput(1,q.id,result).value,'@engine updated');
  result.type=sharedPolicy.UrlbarShared.RESULT_TYPE.URL;assert.equal(f.editor.describeResultInput(1,q.id,result).value,'');
  f.editor.cancel();assert.throws(()=>f.editor.describeResultInput(1,q.id,result),/Stale/);f.editor.close();
});


test('independent URL descriptions use typed text and fixed privacy even after completion acknowledgment',async()=>{
  const f=await completionFixture();f.result.type=sharedPolicy.UrlbarShared.RESULT_TYPE.URL;
  f.result.heuristic=true;f.result.payload.url='http://example.test/';
  f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal));
  const calls=[];f.sandbox.UrlbarContentUtils.getFixupPrimitives=(text,isPrivate)=>{calls.push({text,isPrivate});return {};};
  assert.equal(f.editor.describeResultInput(1,f.query.id,f.result,{isPrivate:false,userTypedValue:'https://override',isAddressbar:false}).value,'example.test/');
  assert.deepEqual(calls,[{text:'example.test/',isPrivate:true}]);
  assert.equal(f.editor.snapshot.text,'example.test/');
  assert.equal(f.editor.describeResultInput(1,f.query.id,f.result,{urlOverride:'https://safe.test/path'}).value,'https://safe.test/path');
  assert.equal(calls.length,1,'override bypasses heuristic trimming');f.editor.close();
});
