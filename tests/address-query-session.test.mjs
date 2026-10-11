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
    UrlbarShared:{getLogger:()=>({debug(){}}),isPasteEvent:e=>e.inputType?.startsWith('insertFromPaste') || e.inputType==='insertFromYank',RESULT_SOURCE:{ACTIONS:7},NOTIFICATIONS:{
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:()=>10},UrlbarContentUtils:{usesMessagePath:()=>false},
    UrlbarQueryContext:class {constructor(options){Object.assign(this,options,{id:++id,results:[]});}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;}}});
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarQuerySession','UrlbarAutofillModel','UrlbarEditSession']) {
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

test('streaming updates preserve context identity and suppress stale, cancelled and out-of-scope callbacks',async()=>{
  const f=fixture(),events=[];await f.parent.child.engineStore.init();
  let query;const gate=Promise.withResolvers();
  f.parent.startQuery=q=>{query=q;return gate.promise;};
  const pending=f.session.query({}, {searchString:'stream'},e=>events.push(e));
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
    events.push('old '+e.kind);
    newer=f.session.query({}, {searchString:'new'},n=>events.push('new '+n.kind));
  });
  f.parent.child.notify('onFirstResult',first);
  f.parent.child.notify('onQueryResults',first);f.parent.child.notify('onQueryFinished',first);
  f.parent.child.notify('onQueryResults',second);
  await assert.rejects(old,/superseded/);await newer;
  assert.deepEqual(events,['old first-result','new results']);f.session.close();
});

async function editorFixture() {
  const f=fixture();f.session.close();const updates=[];
  const editor=new f.sandbox.UrlbarEditSession({context:f.context,isCurrent:()=>!f.invalid,
    onUpdate:update=>updates.push(update)});
  const parent=f.parents.at(-1);await parent.child.engineStore.init();
  return {...f,editor,parent,updates,loseScope(){f.invalid=true;}};
}
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
