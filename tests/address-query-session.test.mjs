import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {UrlbarQueryLifecycle} from '../upstream/firefox/browser/components/urlbar/content/UrlbarQueryLifecycle.mjs';

const sharedPolicy=vm.createContext({URL,UrlbarContentUtils:{},UrlbarPrefs:{get:()=>false}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarShared.mjs',import.meta.url),'utf8')
  .replace(/^import .*;$/gm,'').replace('export const UrlbarShared','var UrlbarShared'),sharedPolicy);

function fixture({hold=false,uninitializedResults=false,onPresentationRequest=null}={}) {
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
      RESULTS_CLOSE_REQUESTED:'onResultsCloseRequested',QUERY_RESULT_REMOVED:'onQueryResultRemoved',
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:key=>prefs[key] ?? 10},UrlbarContentUtils:{usesMessagePath:()=>false,getDisplaySpec:spec=>spec,getFixupPrimitives:()=>({})},
    UrlbarQueryContext:class {constructor(options){Object.assign(this,options,{id:++id,results:uninitializedResults?undefined:[]});}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarUrlValue.mjs',import.meta.url),'utf8')
    .replace(/^import .*;$/gm,'').replaceAll('export function ','function '),sandbox);
  Object.assign(sandbox.UrlbarShared, {RESTRICT_TOKENS:sharedPolicy.UrlbarShared.RESTRICT_TOKENS,
    LOCAL_SEARCH_MODES:sharedPolicy.UrlbarShared.LOCAL_SEARCH_MODES,
    SEARCH_MODE_RESTRICT:sharedPolicy.UrlbarShared.SEARCH_MODE_RESTRICT});
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarSearchModeModel','UrlbarQuerySession','UrlbarAutofillModel','UrlbarResultPresentation','UrlbarEditSession']) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${file}.mjs`,import.meta.url),'utf8');
    vm.runInContext(source.replace(/^import .*;$/gm,'').replaceAll('export class ','class ')+`\nthis.${file}=${file};`,sandbox);
  }
  let current=true;
  const context={sapName:'urlbar',isPrivate:true,actor:{browsingContext:{isContent:false}}};
  const session=new sandbox.UrlbarQuerySession({context,isCurrent:()=>current,onPresentationRequest});
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

test('provider selection is ordered, element-free and acknowledged exactly once',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'select'}),result={id:1};q.results=[result];
  const calls=[];f.parent.onBeforeSelection=(...args)=>calls.push(['before',...args]);
  f.parent.onSelection=(...args)=>calls.push(['selected',...args]);
  assert.throws(()=>f.session.beginResultSelection(q.id,result),/Invalid/);
  f.session.publishPresentation(q.id,[result],true);f.session.freezeResults(q.id);
  const request=f.session.beginResultSelection(q.id,result);
  assert.deepEqual(calls,[['before',result]]);assert.equal(Object.isFrozen(request),true);
  assert.equal(f.session.getResultSelection({}),null);
  assert.equal(f.session.acknowledgeResultSelection({}),false);
  assert.equal(f.session.getResultSelection(request.token),request);
  assert.equal(f.session.acknowledgeResultSelection(request.token),true);
  assert.equal(f.session.acknowledgeResultSelection(request.token),false);
  assert.deepEqual(calls,[['before',result],['selected',result]]);f.session.close();
});

test('selection requests cannot survive hiding, removal, replacement or cancellation',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'select'}),result={id:1};q.results=[result];
  f.parent.onBeforeSelection=()=>{};f.parent.onSelection=()=>assert.fail('stale selection notified');
  f.session.publishPresentation(q.id,[result],true);
  let request=f.session.beginResultSelection(q.id,result);
  f.session.publishPresentation(q.id,[result],false);f.session.publishPresentation(q.id,[result],true);
  assert.equal(f.session.getResultSelection(request.token),null);
  request=f.session.beginResultSelection(q.id,result);
  const newer=f.session.beginResultSelection(q.id,result);
  assert.equal(f.session.getResultSelection(request.token),null);
  f.parent.child.notify('onQueryResultRemoved',1,undefined,q.id);
  assert.equal(f.session.acknowledgeResultSelection(newer.token),false);
  const next=await f.session.query({}, {searchString:'next'});next.results=[result];
  f.session.publishPresentation(next.id,[result],true);
  request=f.session.beginResultSelection(next.id,result);f.session.cancel();
  assert.equal(f.session.acknowledgeResultSelection(request.token),false);f.session.close();
});

test('reentrant and throwing provider selection hooks cannot revive or reuse requests',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'select'}),result={id:1};q.results=[result];
  f.session.publishPresentation(q.id,[result],true);
  f.parent.onBeforeSelection=()=>f.session.cancel();
  assert.equal(f.session.beginResultSelection(q.id,result),null);
  const next=await f.session.query({}, {searchString:'next'});next.results=[result];
  f.session.publishPresentation(next.id,[result],true);
  f.parent.onBeforeSelection=()=>{throw Error('provider failure');};
  assert.throws(()=>f.session.beginResultSelection(next.id,result),/provider failure/);
  f.parent.onBeforeSelection=()=>{};
  const request=f.session.beginResultSelection(next.id,result);
  f.parent.onSelection=()=>{
    assert.equal(f.session.acknowledgeResultSelection(request.token),false);
    throw Error('selection failure');
  };
  assert.throws(()=>f.session.acknowledgeResultSelection(request.token),/selection failure/);
  assert.equal(f.session.getResultSelection(request.token),null);f.session.close();
});

test('editor selection acknowledgment is bound to the exact observed editor snapshot',async()=>{
  const f=await completionFixture();f.result.id=7;
  const calls=[];f.parent.onBeforeSelection=()=>calls.push('before');f.parent.onSelection=()=>calls.push('selected');
  f.editor.publishPresentation(1,f.query.id,[f.result],true);
  const snapshot=f.editor.snapshot,request=f.editor.beginResultSelection(1,f.query.id,f.result);
  assert.equal(request.edit,snapshot);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  assert.equal(f.editor.acknowledgeResultSelection(request.token),true);
  assert.equal(f.editor.acknowledgeResultSelection(request.token),false);
  const stale=f.editor.beginResultSelection(1,f.query.id,f.result);
  f.editor.select(1,0,0);assert.equal(f.editor.getResultSelection(stale.token),null);
  assert.equal(f.editor.acknowledgeResultSelection(stale.token),false);
  f.parent.onBeforeSelection=()=>f.editor.select(1,1,1);
  assert.equal(f.editor.beginResultSelection(1,f.query.id,f.result),null);
  assert.deepEqual(calls,['before','selected','before']);f.editor.close();
});

async function previewFixture() {
  const f=await editorFixture(),query=await f.editor.edit({},edit(1,'typed words'));
  const result={id:5,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{query:'suggested words'}};
  query.results=[result];f.editor.publishPresentation(1,query.id,[result],true);
  const calls=[];f.parent.onBeforeSelection=()=>calls.push('before');
  f.parent.onSelection=()=>calls.push(f.editor.snapshot.text);
  return {...f,query,result,calls};
}

test('ordinary result previews observe exact writes before provider selection without requerying',async()=>{
  const f=await previewFixture(),snapshot=f.editor.snapshot,started=f.parent.started.length;
  const p=f.editor.beginResultPreview(1,f.query.id,f.result);
  assert.equal(p.value,'suggested words');assert.equal(p.selectionStart,p.value.length);
  assert.equal(p.edit,snapshot);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.acknowledgeResultSelection(p.token),false,'row-only ack must not bypass text observation');
  assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),text:'wrong'}),false);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),true);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),false);
  assert.deepEqual(f.calls,['before','suggested words']);assert.equal(f.parent.started.length,started);
  assert.equal(f.query.searchString,'typed words');assert.equal(f.editor.snapshot.selectionRevision,1);
  f.editor.close();
});

test('preview writes revalidate live text, result policy, exposure and caret state',async()=>{
  const f=await previewFixture();let p=f.editor.beginResultPreview(1,f.query.id,f.result);
  f.result.payload.query='changed';assert.equal(f.editor.getResultPreview(p.token),null);
  p=f.editor.beginResultPreview(1,f.query.id,f.result);f.result.payload.providesSearchMode=true;
  assert.equal(f.editor.getResultPreview(p.token),null);delete f.result.payload.providesSearchMode;
  p=f.editor.beginResultPreview(1,f.query.id,f.result);
  f.editor.publishPresentation(1,f.query.id,[],false);assert.equal(f.editor.getResultPreview(p.token),null);
  f.editor.publishPresentation(1,f.query.id,[f.result],true);
  p=f.editor.beginResultPreview(1,f.query.id,f.result);f.editor.select(1,0,0);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),false);
  p=f.editor.beginResultPreview(1,f.query.id,f.result);
  f.parent.child.notify('onQueryResultRemoved',5,undefined,f.query.id);
  assert.equal(f.editor.getResultPreview(p.token),null);f.editor.close();
});

test('unsupported preview transitions return no proposal or provider notification',async()=>{
  const f=await previewFixture();
  for (const property of ['keyword','providesSearchMode']) {
    f.result.payload[property]='mode';assert.equal(f.editor.beginResultPreview(1,f.query.id,f.result),null);
    delete f.result.payload[property];
  }
  f.result.autofill={};assert.equal(f.editor.beginResultPreview(1,f.query.id,f.result),null);delete f.result.autofill;
  f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.DYNAMIC;
  assert.equal(f.editor.beginResultPreview(1,f.query.id,f.result),null);
  assert.deepEqual(f.calls,[]);f.editor.close();
});

test('URL previews preserve scheme and a reentrant selection hook sees committed text only once',async()=>{
  const f=await previewFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.URL;
  f.result.payload={url:'https://example.test/path'};
  const p=f.editor.beginResultPreview(1,f.query.id,f.result);assert.equal(p.value,'https://example.test/path');
  f.parent.onSelection=()=>{
    assert.equal(f.editor.snapshot.text,p.value);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),false);
    f.editor.cancel();
  };
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),true);
  assert.equal(f.editor.getResultPreview(p.token),null);f.editor.close();
});

test('selected autofill previews preserve suffix selection and placeholder metadata after exact observation',async()=>{
  const f=await previewFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.URL;
  f.result.payload={url:'https://typed.example.test/'};
  f.result.autofill={value:'typed.example.test/',selectionStart:5,selectionEnd:19,
    type:'adaptive_origin',adaptiveHistoryInput:'type',untrimmedValue:'https://typed.example.test/'};
  const snapshot=f.editor.snapshot,p=f.editor.beginResultPreview(1,f.query.id,f.result);
  assert.equal(p.kind,'autofill');assert.equal(p.selectionDirection,'backward');
  assert.equal(p.selectionStart,5);assert.equal(p.autofill.untrimmedValue,'https://typed.example.test/');
  assert.equal(Object.isFrozen(p.autofill),true);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),selectionDirection:'forward'}),false);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),true);
  assert.deepEqual(f.calls,['before',p.value]);assert.equal(f.query.searchString,'typed words');
  const next=f.editor.edit({},edit(2,'typed.'));
  const placeholder=f.updates.find(u=>u.edit.revision===2 && u.kind==='prepared')?.completion;
  assert.equal(placeholder?.kind,'placeholder');assert.equal(placeholder.value,p.value);
  assert.equal(placeholder.selectionStart,6);assert.equal(placeholder.untrimmedValue,p.autofill.untrimmedValue);
  await next;f.editor.close();
});

test('autofill previews revalidate selection, metadata and transition kind before host writes',async()=>{
  for (const mutate of [r=>r.autofill.selectionStart++,r=>r.autofill.untrimmedValue='changed',
    r=>r.autofill.type='adaptive_url',r=>delete r.autofill,r=>r.payload.providesSearchMode=true]) {
    const f=await previewFixture();f.result.autofill={value:'suggested words',selectionStart:3,selectionEnd:15,
      type:'origin',untrimmedValue:'https://suggested.test/'};
    const p=f.editor.beginResultPreview(1,f.query.id,f.result);mutate(f.result);
    assert.equal(f.editor.getResultPreview(p.token),null);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),false);f.editor.close();
  }
});

test('malformed autofill selection cannot produce a preview or notify providers',async()=>{
  const f=await previewFixture();
  for (const autofill of [{},{value:'abc',selectionStart:-1,selectionEnd:3},
    {value:'abc',selectionStart:2,selectionEnd:1},{value:'abc',selectionStart:0,selectionEnd:4},
    {value:'abc',selectionStart:0.5,selectionEnd:3}]) {
    f.result.autofill=autofill;assert.equal(f.editor.beginResultPreview(1,f.query.id,f.result),null);
  }
  assert.deepEqual(f.calls,[]);f.editor.close();
});

test('removed frozen results lose identity and exposure even if an old descriptor or late batch retains them',async()=>{
  const requests=[],f=fixture({onPresentationRequest:r=>requests.push(r)});
  const q=await f.session.query({}, {searchString:'remove'}),a={id:1},b={id:2};q.results=[a,b];
  const old=f.session.freezeResults(q.id);f.session.publishPresentation(q.id,[a,b],true);
  q.results.splice(0,1);const l10n={id:'dismissed'};
  f.parent.child.notify('onQueryResultRemoved',a.id,l10n,q.id);
  assert.equal(old.results[0],a,'previous descriptors are immutable historical values');
  assert.equal(f.session.ownsResult(q.id,a),false);assert.equal(f.session.ownsResult(q.id,b),true);
  assert.deepEqual([...f.session.freezeResults(q.id).results],[b]);
  assert.deepEqual([...f.parent.child.visibleResults],[b]);
  assert.equal(f.parent.child.resultsOpen,true);
  assert.equal(requests[0].kind,'remove-result');assert.equal(requests[0].acknowledgeDismissalL10n,l10n);
  q.results=[a,b];assert.equal(f.session.ownsResult(q.id,a),false);
  assert.throws(()=>f.session.publishPresentation(q.id,[a],true),/Invalid/);
  f.parent.child.notify('onQueryResultRemoved',a.id,l10n,q.id);assert.equal(requests.length,1);
  f.parent.child.notify('onQueryResultRemoved',b.id,undefined,q.id);
  assert.equal(f.session.freezeResults(q.id).results.length,0);f.session.close();
});

test('removal notifications are query scoped through replacement, cancellation and scope loss',async()=>{
  const requests=[],f=fixture({onPresentationRequest:r=>requests.push(r)}),delivered=[];
  f.parent.child.addListener({onQueryResultRemoved:(...args)=>delivered.push(args)});
  const old=await f.session.query({}, {searchString:'old'});old.results=[{id:1}];
  const next=await f.session.query({}, {searchString:'next'});next.results=[{id:1}];
  f.parent.child.notify('onQueryResultRemoved',1,undefined,old.id);
  assert.equal(delivered.length,0);assert.equal(f.session.ownsResult(next.id,next.results[0]),true);
  f.parent.child.notify('onQueryResultRemoved',1,undefined,next.id);
  assert.equal(requests.length,1);assert.equal(f.session.ownsResult(next.id,next.results[0]),false);
  assert.throws(()=>f.session.freezeResults(next.id),/not ready/);
  f.session.cancel();f.parent.child.notify('onQueryResultRemoved',2,undefined,next.id);
  const current=await f.session.query({}, {searchString:'scope'});f.invalidate();
  f.parent.child.notify('onQueryResultRemoved',3,undefined,current.id);
  assert.equal(requests.length,1);f.session.close();
});

test('editor forwards scoped removal metadata and revokes a matching completion without writing text',async()=>{
  const f=await completionFixture();f.result.id=7;const snapshot=f.editor.snapshot;
  f.parent.child.notify('onQueryResultRemoved',7,{id:'dismissed'},f.query.id);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  assert.equal(f.requests[0].kind,'remove-result');assert.equal(f.requests[0].edit,snapshot);
  assert.equal(f.requests[0].acknowledgeDismissalL10n.id,'dismissed');assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.getCloseRequest(f.requests[0].token),null);
  assert.throws(()=>f.editor.describeResultInput(1,f.query.id,f.result),/Stale/);f.editor.close();
});

test('query-scoped close requests survive result freezing but not cancellation, replacement or scope loss',async()=>{
  const requests=[],f=fixture({onPresentationRequest:request=>requests.push(request)});
  const q=await f.session.query({}, {searchString:'close'});q.results=[{}];
  f.session.freezeResults(q.id);
  f.parent.child.notify('onResultsCloseRequested',q.id,true);
  assert.deepEqual({...requests[0]},{kind:'close',queryId:q.id,elementPicked:true});
  assert.ok(Object.isFrozen(requests[0]));assert.equal(f.session.ownsQuery(q.id),true,'request does not apply host UI effects');
  for(const args of [[q.id-1,true],[q.id,false],[q.id,{}]])f.parent.child.notify('onResultsCloseRequested',...args);
  const next=await f.session.query({}, {searchString:'new'});
  f.parent.child.notify('onResultsCloseRequested',q.id,true);
  f.session.cancel();f.parent.child.notify('onResultsCloseRequested',next.id,true);
  const last=await f.session.query({}, {searchString:'scope'});f.invalidate();
  f.parent.child.notify('onResultsCloseRequested',last.id,true);
  assert.equal(requests.length,1);f.session.close();
});

test('child drops delayed close requests as soon as a newer query is armed before its results arrive',async()=>{
  const f=fixture(),received=[];
  f.parent.child.addListener({onResultsCloseRequested:id=>received.push(id)});
  const old=await f.session.query({}, {searchString:'old'});
  const gate=Promise.withResolvers();let current;
  f.parent.startQuery=q=>{current=q;return gate.promise;};
  const pending=f.session.query({}, {searchString:'new'});
  f.parent.child.notify('onResultsCloseRequested',old.id,true);
  assert.deepEqual(received,[]);
  f.parent.child.notify('onResultsCloseRequested',current.id,true);
  assert.deepEqual(received,[current.id]);gate.resolve(current);await pending;f.session.close();
});

test('parent close requests require exact current context and original view ignores delayed requests',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  requestResultsClose(queryContext) {');
  const method=source.slice(start,source.indexOf('\n  }',start)+4);
  const Parent=new Function('lazy',`return class {#destroyed=false; destroy(){this.#destroyed=true;} ${method}}`)({UrlbarShared:{NOTIFICATIONS:{RESULTS_CLOSE_REQUESTED:'close'}}});
  const parent=new Parent(),q={id:1},calls=[];parent.notify=(...args)=>calls.push(args);
  assert.equal(parent.requestResultsClose(q),false);
  parent._lastQueryContextWrapper={queryContext:q,done:true};
  assert.equal(parent.requestResultsClose({...q}),false);
  assert.equal(parent.requestResultsClose(q),true);assert.deepEqual(calls,[['close',1,true]]);
  parent.destroy();assert.equal(parent.requestResultsClose(q),false);
  const viewSource=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const a=viewSource.indexOf('  onResultsCloseRequested(queryId, elementPicked) {');
  const viewMethod=viewSource.slice(a,viewSource.indexOf('\n  }',a)+4);
  const View=new Function(`return class {#queryContext={id:2}; ${viewMethod}}`)(),view=new View();
  view.close=options=>calls.push({...options});
  view.onResultsCloseRequested(1,true);view.onResultsCloseRequested(2,false);
  assert.equal(calls.length,1);view.onResultsCloseRequested(2,true);
  assert.deepEqual(calls[1],{elementPicked:true});
});

test('contextual commands retain effects and close-before-removal order without reading a view or container',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarProviderQuickSuggestContextualOptIn.sys.mjs',import.meta.url),'utf8');
  const a=source.indexOf('  onEngagement(queryContext,'),b=source.indexOf('  #dismiss()',a);
  const calls=[],Provider=new Function('lazy',`return class {
    #dismiss(){this.calls.push('dismiss');} #shouldDisplayContextualOptIn(){return false;}
    ${source.slice(a,b)}
  }`)({UrlbarPrefs:{set:(...args)=>calls.push(['pref',...args])}});
  const provider=new Provider();provider.calls=calls;
  const q={},result={},controller={get view(){throw Error('provider accessed a view');},
    browserWindow:{openHelpLink:name=>calls.push(['help',name])},
    requestResultsClose:context=>{assert.equal(context,q);calls.push('close');},
    removeResult:r=>{assert.equal(r,result);calls.push('remove');}};
  for(const [selType,effect] of [['allow',['pref','quicksuggest.online.enabled',true]],
    ['learn_more',['help','firefox-suggest']],['dismiss','dismiss']]) {
    calls.length=0;provider.onEngagement(q,controller,{selType,result});
    assert.deepEqual(calls,[effect,'close','remove']);
  }
  calls.length=0;provider.onEngagement(q,controller,{selType:'unknown',result});assert.deepEqual(calls,[]);
  assert.doesNotMatch(source,/controller\.view|container\.hidden/);
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
  const f=fixture(options);f.session.close();const updates=[],requests=[];
  const editor=new f.sandbox.UrlbarEditSession({context:f.context,isCurrent:()=>!f.invalid,
    onUpdate:update=>updates.push(update),onPresentationRequest:request=>{
      requests.push(request);options?.onRequest?.(request,editor);
    }});
  const parent=f.parents.at(-1);await parent.child.engineStore.init();
  return {...f,editor,parent,updates,requests,loseScope(){f.invalid=true;}};
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

test('independent mode descriptions require exact current result identity and remain usable after freezing',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'mode'));
  const result={type:1,providerName:'UrlbarProviderTabToSearch',payload:{engine:'Engine'}};q.results=[result];
  const mode=f.editor.describeResultSearchMode(1,q.id,result);
  assert.equal(mode.engineName,'Engine');assert.equal(mode.entry,'tabtosearch');assert.ok(Object.isFrozen(mode));
  assert.throws(()=>f.editor.describeResultSearchMode(0,q.id,result),/Stale/);
  assert.throws(()=>f.editor.describeResultSearchMode(1,q.id,{...result}),/Stale/);
  f.editor.freezeResults(1,q.id);q.results=[];
  assert.equal(f.editor.describeResultSearchMode(1,q.id,result).engineName,'Engine');
  result.payload={keyword:'^'};
  assert.equal(f.editor.describeResultSearchMode(1,q.id,result,{isAddressbar:false}).restrictType,'symbol');
  assert.equal(f.editor.describeResultSearchMode(1,q.id,result,{actionEngine:'Action'}).engineName,'Action');
  await f.editor.edit({},edit(2,'next'));
  assert.throws(()=>f.editor.describeResultSearchMode(2,q.id,result),/Stale/);
  await f.editor.edit({},edit(3,'compose',{composing:true}));
  assert.throws(()=>f.editor.describeResultSearchMode(3,q.id,result),/Stale/);f.editor.close();
});

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

test('editor close requests revoke query and completion authority before host notification and require observed closure',async()=>{
  const f=await completionFixture(),snapshot=f.editor.snapshot;
  f.editor.publishPresentation(1,f.query.id,[f.result],true);
  assert.equal(f.editor.getCloseRequest(undefined),null);
  f.parent.child.notify('onResultsCloseRequested',f.query.id+1,true);
  f.parent.child.notify('onResultsCloseRequested',f.query.id,false);
  assert.equal(f.requests.length,0);assert.equal(f.editor.getCompletion(f.proposal.token),f.proposal);
  f.parent.child.notify('onResultsCloseRequested',f.query.id,true);
  const request=f.requests[0];assert.ok(Object.isFrozen(request));assert.ok(Object.isFrozen(request.token));
  assert.equal(request.edit,snapshot);assert.equal(request.queryId,f.query.id);assert.equal(request.elementPicked,true);
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.child.resultsOpen,false);
  assert.equal(f.parent.child.visibleResults.length,0);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  assert.throws(()=>f.editor.describeResultInput(1,f.query.id,f.result),/Stale/);
  assert.throws(()=>f.editor.publishPresentation(1,f.query.id,[f.result],true),/Invalid/);
  assert.equal(f.editor.acknowledgeClose({}, {open:false}),false);
  assert.equal(f.editor.acknowledgeClose(request.token,{open:true}),false);
  assert.equal(f.editor.getCloseRequest(request.token),request);
  assert.equal(f.editor.acknowledgeClose(request.token,{open:false}),true);
  assert.equal(f.editor.acknowledgeClose(request.token,{open:false}),false);
  f.parent.child.notify('onResultsCloseRequested',f.query.id,true);
  f.parent.child.notify('onFirstResult',f.query);
  assert.equal(f.requests.length,1);assert.equal(f.parent.started.length,1);f.editor.close();
});

test('frozen editor close requests cannot outlive a caret move, edit, cancel, scope or another session',async()=>{
  for(const revoke of [f=>f.editor.select(1,1,1),f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    const f=await completionFixture();f.editor.freezeResults(1,f.query.id);
    f.parent.child.notify('onResultsCloseRequested',f.query.id,true);
    const request=f.requests[0];await revoke(f);
    assert.equal(f.editor.getCloseRequest(request.token),null);
    assert.equal(f.editor.acknowledgeClose(request.token,{open:false}),false);f.editor.close();
  }
  const a=await completionFixture(),b=await completionFixture();
  for(const f of [a,b])f.parent.child.notify('onResultsCloseRequested',f.query.id,true);
  assert.equal(b.editor.acknowledgeClose(a.requests[0].token,{open:false}),false);
  a.loseScope();assert.throws(()=>a.editor.getCloseRequest(a.requests[0].token),/scope changed/);
  assert.equal(a.parent.destroyed,1);b.editor.close();
  assert.throws(()=>b.editor.getCloseRequest(b.requests[0].token),/closed/);
});

test('host close callbacks may synchronously replace the edit without reviving the old request',async()=>{
  let next;
  const f=await editorFixture({onRequest:(request,editor)=>{
    assert.equal(editor.getCloseRequest(request.token),request);
    next=editor.edit({},edit(2,'new host edit'));
  }});
  const q=await f.editor.edit({},edit(1,'old'));q.results=[{payload:{}}];
  f.parent.child.notify('onResultsCloseRequested',q.id,true);
  const current=await next;
  assert.equal(f.editor.getCloseRequest(f.requests[0].token),null);
  f.editor.publishPresentation(2,current.id,[],true);assert.equal(f.parent.child.resultsOpen,true);
  f.parent.child.notify('onResultsCloseRequested',q.id,true);assert.equal(f.requests.length,1);f.editor.close();
});

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
