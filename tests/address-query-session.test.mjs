import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {UrlbarQueryLifecycle} from '../upstream/firefox/browser/components/urlbar/content/UrlbarQueryLifecycle.mjs';

const sharedPolicy=vm.createContext({URL,UrlbarContentUtils:{},UrlbarPrefs:{get:()=>false}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarShared.mjs',import.meta.url),'utf8')
  .replace(/^import .*;$/gm,'').replace('export const UrlbarShared','var UrlbarShared'),sharedPolicy);

function fixture({hold=false,uninitializedResults=false,onPresentationRequest=null,isPrivate=true,browserId}={}) {
  const parents=[];
  const prefs={"experimental.hideHeuristic":false,openintab:false,ctrlCanonizesURLs:false,"browser.search.openintab":false};
  class Parent {
    constructor(identity){this.identity=identity;this.started=[];this.engagements=[];
      this.engagementEvent={start:(...args)=>this.engagements.push({kind:'start',args}),
        record:(...args)=>this.engagements.push({kind:'record',args}),discard:()=>this.engagements.push({kind:'discard'})};parents.push(this);}
    setChild(child){this.child=child;}
    initEngineStore(){if(!hold)this.child.updateEngineStore('init',[],null);}
    startQuery(query){query.results??=[];this.started.push(query);return Promise.resolve(query);}
    cancelQuery(){this.cancelled=(this.cancelled??0)+1;}
    destroy(){this.destroyed=(this.destroyed??0)+1;}
  }
  let id=0;
  const sandbox=vm.createContext({console,URL,UrlbarQueryLifecycle,UrlbarParentControllerProxy:class {},
    KeyboardEvent:class {constructor(facts={}){Object.assign(this,{keyCode:13},facts);}getModifierState(key){return key==='AltGraph' && !!this.altGraph;}},KeyEvent:{DOM_VK_RETURN:13},
    UrlbarShared:{stripPrefixAndTrim:sharedPolicy.UrlbarShared.stripPrefixAndTrim,getLogger:()=>({debug(){}}),isPasteEvent:e=>e.inputType?.startsWith('insertFromPaste') || e.inputType==='insertFromYank',RESULT_TYPE:sharedPolicy.UrlbarShared.RESULT_TYPE,RESULT_SOURCE:sharedPolicy.UrlbarShared.RESULT_SOURCE,canAutofillURL:sharedPolicy.UrlbarShared.canAutofillURL.bind(sharedPolicy.UrlbarShared),NOTIFICATIONS:{
      RESULTS_CLOSE_REQUESTED:'onResultsCloseRequested',QUERY_RESULT_REMOVED:'onQueryResultRemoved',
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:key=>prefs[key] ?? 10},UrlbarContentUtils:{usesMessagePath:()=>false,getDisplaySpec:spec=>spec,getFixupPrimitives:()=>({}),getPlatform:()=> 'win',whereToOpenLink:e=>e?.where || 'current',willLoadInBackground:(_where,params)=>!params.forceForeground},
    UrlbarQueryContext:class {constructor(options){Object.assign(this,options,{id:++id,results:uninitializedResults?undefined:[]});}},
    ChromeUtils:{defineESModuleGetters(target){target.UrlbarParentController=Parent;target.QuickSuggest={getFeature:()=>sandbox.backend};target.UrlUtils={LOOKS_LIKE_ORIGIN:{NONE:0},looksLikeOrigin:text=>text.includes(".")?1:0};target.UrlbarTokenizer={tokenize:context=>[{value:context.searchString}]};}}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarUrlValue.mjs',import.meta.url),'utf8')
    .replace(/^import .*;$/gm,'').replaceAll('export function ','function '),sandbox);
  Object.assign(sandbox.UrlbarShared, {isSearchbarSAP:sharedPolicy.UrlbarShared.isSearchbarSAP.bind(sharedPolicy.UrlbarShared),getLoadRequestFromResult:sharedPolicy.UrlbarShared.getLoadRequestFromResult.bind(sharedPolicy.UrlbarShared),looksLikeSingleWordHost:sharedPolicy.UrlbarShared.looksLikeSingleWordHost.bind(sharedPolicy.UrlbarShared),isInstance:(event,type)=>event instanceof type,REGEXP_SPACES:sharedPolicy.UrlbarShared.REGEXP_SPACES,RESTRICT_TOKENS:sharedPolicy.UrlbarShared.RESTRICT_TOKENS,
    LOCAL_SEARCH_MODES:sharedPolicy.UrlbarShared.LOCAL_SEARCH_MODES,
    SEARCH_MODE_RESTRICT:sharedPolicy.UrlbarShared.SEARCH_MODE_RESTRICT,
    SEARCH_MODE_ENTRY:sharedPolicy.UrlbarShared.SEARCH_MODE_ENTRY,
    deepEqual:sharedPolicy.UrlbarShared.deepEqual.bind(sharedPolicy.UrlbarShared),
    getResultSourceName:sharedPolicy.UrlbarShared.getResultSourceName.bind(sharedPolicy.UrlbarShared)});
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarSelectedSearch','UrlbarSearchModeModel','UrlbarQuerySession','UrlbarAutofillModel','UrlbarResultPresentation','UrlbarEditSession']) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${file}.mjs`,import.meta.url),'utf8');
    vm.runInContext(source.replace(/^import .*;$/gm,'').replaceAll('export class ','class ').replaceAll('export function ','function ')+`\nthis.${file}=${file==='UrlbarSelectedSearch'?'querySelectedSearch':file};`,sandbox);
  }
  let current=true;
  const context={sapName:'urlbar',isPrivate,browserId,actor:{browsingContext:{isContent:false}}};
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

test('clearing selection restores typed text exactly without provider notification or another query',async()=>{
  const f=await previewFixture();const p=f.editor.beginResultPreview(1,f.query.id,f.result);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),true);
  const snapshot=f.editor.snapshot,started=f.parent.started.length,calls=[...f.calls];
  const restore=f.editor.beginResultRestore(1,f.query.id);
  assert.equal(restore.kind,'restore');assert.equal(restore.value,'typed words');
  assert.equal(restore.edit,snapshot);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.editor.acknowledgeResultSelection(restore.token),false);
  assert.equal(f.editor.acknowledgeResultPreview(restore.token,{...observed(restore),selectionStart:0}),false);
  assert.equal(f.editor.acknowledgeResultPreview(restore.token,observed(restore)),true);
  assert.equal(f.editor.acknowledgeResultPreview(restore.token,observed(restore)),false);
  assert.equal(f.editor.snapshot.text,'typed words');assert.equal(f.parent.started.length,started);
  assert.deepEqual(f.calls,calls);f.editor.close();
});

test('restoration survives removal of the old row but not hiding or a new selection',async()=>{
  const f=await previewFixture();let restore=f.editor.beginResultRestore(1,f.query.id);
  f.parent.child.notify('onQueryResultRemoved',5,undefined,f.query.id);
  f.editor.publishPresentation(1,f.query.id,[],true);
  assert.equal(f.editor.getResultPreview(restore.token),restore);
  f.editor.publishPresentation(1,f.query.id,[],false);
  f.editor.publishPresentation(1,f.query.id,[],true);
  assert.equal(f.editor.getResultPreview(restore.token),null);
  restore=f.editor.beginResultRestore(1,f.query.id);f.editor.select(1,0,0);
  assert.equal(f.editor.getResultPreview(restore.token),null);f.editor.close();
  const g=await previewFixture();restore=g.editor.beginResultRestore(1,g.query.id);
  g.editor.beginResultSelection(1,g.query.id,g.result);
  assert.equal(g.editor.getResultPreview(restore.token),null);g.editor.close();
});

test('restoration clears autofill placeholders and preserves an explicitly empty typed value',async()=>{
  const f=await previewFixture();f.result.autofill={value:'typed.example/',selectionStart:5,selectionEnd:14};
  const p=f.editor.beginResultPreview(1,f.query.id,f.result);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),true);
  const restore=f.editor.beginResultRestore(1,f.query.id);
  assert.equal(f.editor.acknowledgeResultPreview(restore.token,observed(restore)),true);
  await f.editor.edit({},edit(2,'typed.'));
  assert.equal(f.updates.some(u=>u.edit.revision===2 && u.kind==='prepared'),false);
  const q=await f.editor.edit({},edit(3,''));q.results=[f.result];
  f.editor.publishPresentation(3,q.id,[f.result],true);
  const empty=f.editor.beginResultRestore(3,q.id);assert.equal(empty.value,'');
  assert.equal(f.editor.acknowledgeResultPreview(empty.token,observed(empty)),true);f.editor.close();
});

test('restoration rejects stale edits, query replacement, scope loss and preview search mode',async()=>{
  const f=await previewFixture(),restore=f.editor.beginResultRestore(1,f.query.id);
  await f.editor.edit({},edit(2,'new'));
  assert.equal(f.editor.getResultPreview(restore.token),null);
  assert.throws(()=>f.editor.beginResultRestore(1,f.query.id),/Stale/);
  const q=await f.editor.edit({searchMode:{isPreview:true}},edit(3,'mode'));q.results=[f.result];
  f.editor.publishPresentation(3,q.id,[f.result],true);
  assert.equal(f.editor.beginResultRestore(3,q.id),null);
  f.loseScope();assert.throws(()=>f.editor.beginResultRestore(3,q.id),/scope changed/);
});

test('focus queries preserve displayed page text and restore it after zero-prefix result preview',async()=>{
  const f=await editorFixture(),text='https://page.example/path';
  const q=await f.editor.focus({}, {...edit(1,text),pageProxyValid:true});
  assert.equal(q.searchString,'');assert.equal(q.allowAutofill,false);assert.equal(f.editor.snapshot.text,text);
  const result={id:1,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{query:'suggestion'}};q.results=[result];
  f.parent.child.notify('onFirstResult',q);assert.equal(f.updates.at(-1).completion,null);
  f.parent.onBeforeSelection=()=>{};f.parent.onSelection=()=>{};
  f.editor.publishPresentation(1,q.id,[result],true);
  const p=f.editor.beginResultPreview(1,q.id,result);f.editor.acknowledgeResultPreview(p.token,observed(p));
  const restore=f.editor.beginResultRestore(1,q.id);assert.equal(restore.value,text);
  assert.equal(f.editor.acknowledgeResultPreview(restore.token,observed(restore)),true);
  assert.equal(q.searchString,'');assert.equal(f.parent.started.length,1);f.editor.close();
});

test('focus query text uses explicit page validity and prefix while composition suppresses dispatch',async()=>{
  const f=await editorFixture();
  const q=await f.editor.focus({}, {...edit(1,'restored words'),pageProxyValid:false});
  assert.equal(q.searchString,'restored words');
  const prefix=await f.editor.focus({}, {...edit(2,'prefix suffix'),pageProxyValid:true,searchString:'prefix'});
  prefix.results=[{id:2}];f.editor.publishPresentation(2,prefix.id,[],true);
  assert.equal(f.editor.beginResultRestore(2,prefix.id).value,'prefix');
  await assert.rejects(f.editor.focus({}, {...edit(3,'other'),pageProxyValid:true,searchString:'prefix'}),/doesn't start/);
  assert.equal(f.editor.snapshot.revision,2);
  const count=f.parent.started.length;
  assert.equal(await f.editor.focus({}, {...edit(3,'composing'),composing:true,pageProxyValid:false}),null);
  assert.equal(f.parent.started.length,count);assert.equal(f.editor.snapshot.composing,true);f.editor.close();
});

test('focus replaces old completion authority and new edits revoke focus-query publications',async()=>{
  const f=await completionFixture();
  const q=await f.editor.focus({}, {...edit(2,'https://page.example/'),pageProxyValid:true});
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  await f.editor.edit({},edit(3,'new text'));
  f.parent.child.notify('onQueryResults',q);
  assert.throws(()=>f.editor.publishPresentation(2,q.id,[],true),/Stale/);
  await assert.rejects(f.editor.focus({}, {...edit(3,'stale'),pageProxyValid:false}),/stale/);
  await assert.rejects(f.editor.focus({},edit(4,'missing validity')),/Invalid/);
  f.loseScope();await assert.rejects(f.editor.focus({}, {...edit(4,'scope'),pageProxyValid:false}),/scope changed/);
});

test('hiding revokes exposure and writes while retaining result identity for explicit reopening',async()=>{
  const f=await previewFixture(),snapshot=f.editor.snapshot;
  const p=f.editor.beginResultPreview(1,f.query.id,f.result),cancelled=f.parent.cancelled;
  const hidden=f.editor.hideResults(1,f.query.id);
  assert.equal(hidden.results[0],f.result);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.parent.cancelled,cancelled);assert.deepEqual([...f.parent.child.visibleResults],[]);
  assert.equal(f.parent.child.resultsOpen,false);assert.equal(f.editor.getResultPreview(p.token),null);
  assert.throws(()=>f.editor.beginResultPreview(1,f.query.id,f.result),/Hidden/);
  assert.throws(()=>f.editor.beginResultRestore(1,f.query.id),/Hidden/);
  assert.throws(()=>f.editor.publishPresentation(1,f.query.id,[f.result],true),/explicit reopening/);
  f.editor.select(1,0,0);
  assert.equal(f.editor.getHiddenResults(1,f.query.id).results[0],f.result);
  assert.equal(f.editor.reopenResults(1,f.query.id,[f.result]),true);
  assert.equal(f.parent.child.resultsOpen,true);assert.equal(f.editor.reopenResults(1,f.query.id,[f.result]),false);
  assert.equal(f.editor.getResultPreview(p.token),null);assert.equal(f.parent.started.length,1);f.editor.close();
});

test('providers finish hidden queries without publishing completion or reopening suggestions',async()=>{
  const f=await editorFixture(),gate=Promise.withResolvers();let q;
  f.parent.startQuery=query=>{q=query;return gate.promise;};
  const pending=f.editor.edit({},edit(1,'exam'));
  const cancelled=f.parent.cancelled,hidden=f.editor.hideResults(1);
  assert.equal(hidden.queryId,q.id);
  assert.equal(hidden.results.length,0);assert.equal(f.editor.reopenResults(1,q.id,[]),false);
  const result={id:1,payload:{},autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}};
  q.results=[result];f.parent.child.notify('onFirstResult',q);f.parent.child.notify('onQueryResults',q);
  assert.equal(f.updates.length,0);assert.equal(f.parent.cancelled,cancelled);
  assert.equal(f.editor.snapshot.text,'exam');assert.equal(f.parent.child.resultsOpen,false);
  gate.resolve(q);await pending;
  assert.equal(f.editor.getHiddenResults(1,q.id).results[0],result);
  assert.equal(f.editor.reopenResults(1,q.id,[result]),true);f.editor.close();
});

test('hidden removals cannot regain exposure and newer queries or scope changes invalidate reopening',async()=>{
  const f=await previewFixture();f.editor.hideResults(1,f.query.id);
  f.parent.child.notify('onQueryResultRemoved',5,undefined,f.query.id);
  assert.equal(f.editor.getHiddenResults(1,f.query.id).results.length,0);
  assert.equal(f.editor.reopenResults(1,f.query.id,[f.result]),false);
  const q=await f.editor.edit({},edit(2,'next'));q.results=[f.result];
  assert.equal(f.editor.getHiddenResults(1,f.query.id),null);
  f.editor.hideResults(2,q.id);f.loseScope();
  assert.throws(()=>f.editor.reopenResults(2,q.id,[f.result]),/scope changed/);
});

test('hiding clears automatic completion and declines retention for preview search modes',async()=>{
  const f=await completionFixture();f.editor.hideResults(1,f.query.id);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  f.editor.cancel();assert.equal(f.editor.getHiddenResults(1,f.query.id),null);
  const q=await f.editor.edit({searchMode:{isPreview:true}},edit(2,'mode'));
  assert.equal(f.editor.hideResults(2,q.id),null);
  assert.equal(f.editor.getHiddenResults(2,q.id),null);f.editor.close();
});

test('independent result-mode transition descriptions use current observed text and revoke stale identity',async()=>{
  const f=await previewFixture();f.result.payload={keyword:'@engine',engine:'Engine',query:'  mode words  '};
  assert.equal(f.editor.describeResultModeTransition(1,f.query.id,f.result),null);
  const p=f.editor.describeResultModeTransition(1,f.query.id,f.result,{checkValue:false,startQuery:false});
  assert.equal(p.value,'mode words  ');assert.equal(p.searchMode.engineName,'Engine');
  assert.equal(p.storeTypedValue,false);assert.equal(p.startQuery,false);assert.equal(Object.isFrozen(p),true);
  assert.equal(f.editor.snapshot.text,'typed words');assert.equal(f.parent.started.length,1);
  f.result.payload.query=' changed';
  assert.equal(f.editor.describeResultModeTransition(1,f.query.id,f.result,{checkValue:false}).value,'changed');
  f.parent.child.notify('onQueryResultRemoved',5,undefined,f.query.id);
  assert.throws(()=>f.editor.describeResultModeTransition(1,f.query.id,f.result,{checkValue:false}),/Stale/);
  f.editor.close();
});

test('independent mode application descriptions normalize against live engines without applying state',async()=>{
  const f=await previewFixture();f.result.payload={engine:'Engine',query:'words'};
  const describe=()=>f.editor.describeResultModeApplication(1,f.query.id,f.result,{checkValue:false,startQuery:false});
  assert.equal(describe(),null);
  const engine={id:'engine',name:'Engine',isGeneralPurposeEngine:true};
  f.parent.child.updateEngineStore('changed',engine,-1);
  const plan=describe();assert.equal(plan.searchMode.isPreview,true);
  assert.equal(plan.searchMode.source,f.sandbox.UrlbarShared.RESULT_SOURCE.SEARCH);
  assert.equal(plan.searchMode.entry,'keywordoffer');assert.equal(Object.isFrozen(plan.searchMode),true);
  assert.equal(f.editor.snapshot.text,'typed words');assert.equal(f.parent.started.length,1);
  f.parent.child.updateEngineStore('changed',{...engine,isGeneralPurposeEngine:false},0);
  assert.equal(describe().searchMode.source,undefined);
  f.parent.child.updateEngineStore('removed',engine,-1);assert.equal(describe(),null);
  f.parent.child.notify('onQueryResultRemoved',5,undefined,f.query.id);
  assert.throws(describe,/Stale/);f.editor.close();
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


async function modeWriteFixture(options={}) {
  const f=await editorFixture(),query=await f.editor.edit({},edit(1,'@engine'));
  const engine={id:'engine',name:'Engine',isGeneralPurposeEngine:true};
  f.parent.child.updateEngineStore('changed',engine,-1);
  const result={id:1,type:sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH,
    payload:{engine:'Engine',keyword:'@engine',query:' words  ',providesSearchMode:true}};
  query.results=[result];f.editor.publishPresentation(1,query.id,[result],true);
  const calls=[];f.parent.onBeforeSelection=()=>calls.push('before');
  f.parent.onSelection=()=>calls.push('selected');
  const proposal=f.editor.beginResultModeApplication(1,query.id,result,options);
  return {...f,query,result,engine,calls,proposal};
}
const observedMode=p=>({...observed(p),searchMode:{...p.searchMode}});

test('mode writes require exact mode, text and caret acknowledgment before provider notification',async()=>{
  const f=await modeWriteFixture(),p=f.proposal,before=f.editor.snapshot;
  assert.equal(p.value,'words  ');assert.equal(p.searchMode.isPreview,false);
  assert.equal(p.startQuery,true);assert.equal(p.storeTypedValue,true);
  assert.deepEqual(f.calls,['before']);assert.equal(f.editor.snapshot,before);
  assert.equal(f.editor.acknowledgeResultSelection(p.token),false);
  assert.equal(f.editor.getResultPreview(p.token),null);
  for(const wrong of [{searchMode:null},{searchMode:{...p.searchMode,isPreview:true}},
    {text:'wrong'},{selectionStart:0},{selectionEnd:0},{selectionDirection:'backward'}]) {
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observedMode(p),...wrong}),false);
    assert.equal(f.editor.snapshot,before);assert.deepEqual(f.calls,['before']);
  }
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);
  assert.equal(f.editor.snapshot.text,'words  ');assert.deepEqual(f.calls,['before','selected']);
  assert.equal(f.parent.started.length,1,'acknowledgment must not dispatch the requested requery');
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),false);
  const restore=f.editor.beginResultRestore(1,f.query.id);
  assert.equal(restore.value,'words  ','confirmation updates typed-text restoration');f.editor.close();
});

test('mode previews retain preview semantics and reject ordinary text restoration',async()=>{
  const f=await modeWriteFixture({startQuery:false}),p=f.proposal;
  assert.equal(p.searchMode.isPreview,true);assert.equal(p.storeTypedValue,false);assert.equal(p.startQuery,false);
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);
  assert.equal(f.editor.beginResultRestore(1,f.query.id),null);
  assert.equal(f.editor.hideResults(1,f.query.id),null,'preview dismissal needs the mode-exit path');
  f.editor.close();
});

test('pending mode writes revalidate live result payload and engine properties',async()=>{
  for(const mutate of [f=>{f.result.payload.query='changed';},f=>{f.result.payload.keyword='@other';},
    f=>{f.result.payload.engine='Missing';},
    f=>f.parent.child.updateEngineStore('changed',{...f.engine,isGeneralPurposeEngine:false},0),
    f=>f.parent.child.updateEngineStore('removed',f.engine,-1)]) {
    const f=await modeWriteFixture(),p=f.proposal;mutate(f);
    assert.equal(f.editor.getResultModeApplication(p.token),null);
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),false);
    assert.equal(f.editor.snapshot.text,'@engine');assert.deepEqual(f.calls,['before']);f.editor.close();
  }
});

test('mode write authority is revoked by replacement, caret, edit, hide, removal and cancellation',async()=>{
  for(const revoke of [f=>f.editor.beginResultSelection(1,f.query.id,f.result),
    f=>f.editor.beginResultModeApplication(1,f.query.id,f.result),
    f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new')),
    f=>f.editor.hideResults(1,f.query.id),f=>f.editor.cancel(),
    f=>f.editor.publishPresentation(1,f.query.id,[],false),
    f=>f.parent.child.notify('onQueryResultRemoved',1,null,f.query.id)]) {
    const f=await modeWriteFixture(),p=f.proposal;await revoke(f);
    assert.equal(f.editor.getResultModeApplication(p.token),null);
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),false);
    assert.equal(f.calls.includes('selected'),false);f.editor.close();
  }
  const f=await modeWriteFixture();f.loseScope();
  assert.throws(()=>f.editor.getResultModeApplication(f.proposal.token),/scope changed/);
  assert.throws(()=>f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal)),/closed/);
});

test('mode preparation rechecks provider pre-selection mutations and consumes before reentrant notification',async()=>{
  const f=await modeWriteFixture();
  f.parent.onBeforeSelection=()=>f.parent.child.updateEngineStore('removed',f.engine,-1);
  assert.equal(f.editor.beginResultModeApplication(1,f.query.id,f.result),null);
  assert.equal(f.editor.getResultModeApplication(f.proposal.token),null);f.editor.close();
  const g=await modeWriteFixture(),p=g.proposal;
  g.parent.onSelection=()=>{
    assert.equal(g.editor.getResultModeApplication(p.token),null);
    assert.equal(g.editor.snapshot.text,p.value);
    assert.equal(g.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),false);
    g.editor.cancel();
  };
  assert.equal(g.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);g.editor.close();
});


async function modeExitFixture() {
  const f=await modeWriteFixture({startQuery:false});
  assert.equal(f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal)),true);
  const proposal=f.editor.beginResultModeExit(1,f.query.id);
  return {...f,proposal};
}

test('clearing a mode-preview selection restores typed text only after exact null-mode acknowledgment',async()=>{
  const f=await modeExitFixture(),p=f.proposal,before=f.editor.snapshot;
  assert.equal(p.kind,'mode-exit');assert.equal(p.searchMode,null);assert.equal(p.value,'@engine');
  assert.equal(p.startQuery,false);assert.equal(p.storeTypedValue,false);
  assert.equal(f.editor.getResultModeApplication(p.token),p);
  assert.equal(f.editor.acknowledgeResultSelection(p.token),false);
  assert.equal(f.editor.getResultPreview(p.token),null);
  for(const wrong of [{searchMode:{source:1}},{searchMode:undefined},{text:'words  '},
    {selectionStart:0},{selectionDirection:'backward'}]) {
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observed(p),searchMode:null,...wrong}),false);
    assert.equal(f.editor.snapshot,before);
  }
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observed(p),searchMode:null}),true);
  assert.equal(f.editor.snapshot.text,'@engine');assert.equal(f.parent.started.length,1);
  assert.deepEqual(f.calls,['before','selected'],'clearing selection must not notify the result provider');
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observed(p),searchMode:null}),false);
  assert.equal(f.editor.beginResultModeExit(1,f.query.id),null);
  assert.equal(f.editor.beginResultRestore(1,f.query.id).value,'@engine');f.editor.close();
});

test('preview-mode exits revoke on edit, caret, selection, close and scope loss',async()=>{
  for(const revoke of [f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new')),
    f=>f.editor.beginResultSelection(1,f.query.id,f.result),f=>f.editor.cancel(),
    f=>f.editor.publishPresentation(1,f.query.id,[],false),f=>f.editor.hideResults(1,f.query.id)]) {
    const f=await modeExitFixture(),p=f.proposal;await revoke(f);
    assert.equal(f.editor.getResultModeApplication(p.token),null);
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observed(p),searchMode:null}),false);
    f.editor.close();
  }
  const f=await modeExitFixture();f.loseScope();
  assert.throws(()=>f.editor.getResultModeApplication(f.proposal.token),/scope changed/);
  const g=await modeExitFixture();g.editor.close();
  assert.throws(()=>g.editor.getResultModeApplication(g.proposal.token),/closed/);
});

test('mode-exit restoration rejects stale revisions and preserves confirmed mode',async()=>{
  const f=await modeWriteFixture();
  assert.equal(f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal)),true);
  assert.equal(f.editor.beginResultModeExit(1,f.query.id),null,'confirmed mode requires explicit exit semantics');
  assert.throws(()=>f.editor.beginResultModeExit(0,f.query.id),/Stale/);
  assert.throws(()=>f.editor.beginResultModeExit(1,f.query.id+1),/Stale/);
  f.editor.close();
});


async function previewDismissalFixture() {
  const f=await modeWriteFixture({startQuery:false});
  f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal));
  return {...f,proposal:f.editor.beginPreviewModeDismissal(1,f.query.id)};
}

test('preview dismissal revokes results immediately and requires observed close plus cleared browser mode/text state',async()=>{
  const f=await previewDismissalFixture(),p=f.proposal,snapshot=f.editor.snapshot;
  assert.equal(p.kind,'close');assert.equal(p.searchMode,null);assert.equal(p.userTypedValue,null);
  assert.equal(p.elementPicked,false);assert.equal(p.edit,snapshot);
  assert.equal(snapshot.text,'words  ','dismissal must not restore the original typed query');
  assert.throws(()=>f.editor.publishPresentation(1,f.query.id,[f.result],true),/Invalid query presentation/);
  assert.throws(()=>f.editor.beginResultModeApplication(1,f.query.id,f.result),/Stale/);
  assert.equal(f.editor.getHiddenResults(1,f.query.id),null);
  for(const observed of [{open:false},{open:true,searchMode:null,userTypedValue:null},
    {open:false,searchMode:{source:1},userTypedValue:null},
    {open:false,searchMode:null,userTypedValue:'old'}]) {
    assert.equal(f.editor.acknowledgeClose(p.token,observed),false);
    assert.equal(f.editor.getCloseRequest(p.token),p);
  }
  assert.equal(f.editor.acknowledgeClose(p.token,{open:false,searchMode:null,userTypedValue:null}),true);
  assert.equal(f.editor.acknowledgeClose(p.token,{open:false,searchMode:null,userTypedValue:null}),false);
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.started.length,1);
  assert.deepEqual(f.calls,['before','selected']);f.editor.close();
});

test('hiding previews and provider close requests publish the same required mode cleanup',async()=>{
  for(const close of [f=>f.editor.hideResults(1,f.query.id),
    f=>f.parent.child.notify('onResultsCloseRequested',f.query.id,true)]) {
    const f=await modeWriteFixture({startQuery:false});
    f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal));
    close(f);const request=f.requests.at(-1);
    assert.equal(request.searchMode,null);assert.equal(request.userTypedValue,null);
    assert.equal(f.editor.getCloseRequest(request.token),request);
    assert.equal(f.editor.acknowledgeClose(request.token,{open:false}),false);
    assert.equal(f.editor.acknowledgeClose(request.token,{open:false,searchMode:null,userTypedValue:null}),true);
    assert.equal(f.editor.snapshot.text,'words  ');f.editor.close();
  }
});

test('preview close acknowledgment cannot survive caret, new edit, cancellation or scope changes',async()=>{
  for(const revoke of [f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    const f=await previewDismissalFixture(),p=f.proposal;await revoke(f);
    assert.equal(f.editor.getCloseRequest(p.token),null);
    assert.equal(f.editor.acknowledgeClose(p.token,{open:false,searchMode:null,userTypedValue:null}),false);
    f.editor.close();
  }
  const f=await previewDismissalFixture();f.loseScope();
  assert.throws(()=>f.editor.getCloseRequest(f.proposal.token),/scope changed/);
  const g=await modeWriteFixture();g.editor.acknowledgeResultModeApplication(g.proposal.token,observedMode(g.proposal));
  assert.equal(g.editor.beginPreviewModeDismissal(1,g.query.id),null,'confirmed mode does not use preview cleanup');
  assert.throws(()=>g.editor.beginPreviewModeDismissal(0,g.query.id),/Stale/);g.editor.close();
});


async function modeToOrdinaryFixture(autofill=false) {
  const f=await editorFixture(),query=await f.editor.edit({},edit(1,'@engine'));
  f.parent.child.updateEngineStore('changed',{id:'engine',name:'Engine',isGeneralPurposeEngine:true},-1);
  const mode={id:1,type:sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH,
    payload:{engine:'Engine',keyword:'@engine',query:'preview words',providesSearchMode:true}};
  const result={id:2,type:sharedPolicy.UrlbarShared.RESULT_TYPE.URL,payload:{url:'https://example.test/path'}};
  if(autofill)result.autofill={value:'example.test/',selectionStart:3,selectionEnd:13,
    untrimmedValue:'https://example.test/',type:'origin'};
  query.results=[mode,result];f.editor.publishPresentation(1,query.id,[mode,result],true);
  const calls=[];f.parent.onBeforeSelection=r=>calls.push(['before',r.id]);
  f.parent.onSelection=r=>calls.push(['selected',r.id]);
  const entering=f.editor.beginResultModeApplication(1,query.id,mode,{startQuery:false});
  assert.equal(f.editor.acknowledgeResultModeApplication(entering.token,observedMode(entering)),true);
  return {...f,query,result,calls};
}

test('ordinary row selection exits preview mode only after observing both cleared mode and result text',async()=>{
  for(const search of [false,true]) {
    const f=await modeToOrdinaryFixture();
    if(search){f.result.type=sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH;f.result.payload={query:'suggested words'};}
    const before=f.editor.snapshot,p=f.editor.beginResultPreview(1,f.query.id,f.result);
    assert.equal(p.searchMode,null);assert.equal(f.editor.snapshot,before);
    assert.equal(p.value,search?'suggested words':'https://example.test/path');
    assert.equal(f.editor.acknowledgeResultSelection(p.token),false);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,observed(p)),false);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:{source:1}}),false);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:null,text:'wrong'}),false);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:null}),true);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:null}),false);
    assert.deepEqual(f.calls,[['before',1],['selected',1],['before',2],['selected',2]]);
    assert.equal(f.parent.started.length,1);
    assert.equal(f.editor.beginResultRestore(1,f.query.id).value,'@engine');f.editor.close();
  }
});

test('leaving mode preview for autofill keeps its exact suffix and placeholder metadata',async()=>{
  const f=await modeToOrdinaryFixture(true),p=f.editor.beginResultPreview(1,f.query.id,f.result);
  assert.equal(p.searchMode,null);assert.equal(p.selectionDirection,'backward');assert.equal(p.selectionStart,3);
  assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:null}),true);
  assert.equal(f.editor.snapshot.selectionEnd,13);
  await f.editor.edit({},edit(2,'exam'));
  const next=f.updates.find(u=>u.edit.revision===2 && u.kind==='prepared').completion;
  assert.equal(next.kind,'placeholder');assert.equal(next.value,'example.test/');
  assert.equal(next.untrimmedValue,'https://example.test/');f.editor.close();
});

test('mode-exit row writes reject changed semantics, hidden results, and caret replacement',async()=>{
  for(const mutate of [f=>{f.result.payload.providesSearchMode=true;},
    f=>{f.result.payload.url='https://changed.test/';},f=>f.editor.select(1,0,0),
    f=>f.editor.hideResults(1,f.query.id)]) {
    const f=await modeToOrdinaryFixture(),p=f.editor.beginResultPreview(1,f.query.id,f.result);mutate(f);
    assert.equal(f.editor.getResultPreview(p.token),null);
    assert.equal(f.editor.acknowledgeResultPreview(p.token,{...observed(p),searchMode:null}),false);
    assert.equal(f.calls.some(([kind,id])=>kind==='selected' && id===2),false);f.editor.close();
  }
});


test('confirmed mode requery requires acknowledgment and runs once with exact text and no autofill',async()=>{
  const f=await modeWriteFixture(),p=f.proposal,state={searchMode:{...p.searchMode}};
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,state),/Stale/);
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);
  const snapshot=f.editor.snapshot;
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:null}),/Stale/);
  const q=await f.editor.requeryAfterModeApplication(p.token,state);
  assert.equal(q.searchString,'words  ');assert.equal(q.allowAutofill,false);
  assert.equal(q.searchMode.engineName,'Engine');assert.equal(f.editor.snapshot,snapshot);
  state.searchMode.engineName='Mutated host';assert.equal(q.searchMode.engineName,'Engine');
  assert.equal(f.parent.started.length,2);assert.notEqual(q.id,f.query.id);
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,state),/Stale/);
  assert.throws(()=>f.editor.describeResultInput(1,f.query.id,f.result),/Stale/);
  q.results=[{id:9,type:sharedPolicy.UrlbarShared.RESULT_TYPE.SEARCH,heuristic:true,payload:{query:'words'}}];
  q.trimmedSearchString='words';f.parent.child.notify('onQueryResults',q);
  assert.equal(f.updates.at(-1).completion,null);assert.equal(f.updates.at(-1).defaultSelection.kind,'select-first');
  f.editor.close();
});

test('mode requery revalidates engine/result facts and rejects preview-only transitions',async()=>{
  for(const mutate of [f=>{f.result.payload.query='changed';},
    f=>f.parent.child.updateEngineStore('removed',f.engine,-1),
    f=>f.parent.child.updateEngineStore('changed',{...f.engine,isGeneralPurposeEngine:false},0)]) {
    const f=await modeWriteFixture(),p=f.proposal;
    f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));mutate(f);
    await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Changed/);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
  const f=await modeWriteFixture({startQuery:false}),p=f.proposal;
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Stale/);f.editor.close();
});

test('acknowledged mode requery is revoked by later editor work and provider reentrancy',async()=>{
  for(const revoke of [f=>f.editor.select(1,0,0),f=>f.editor.cancel(),
    f=>f.editor.edit({},edit(2,'new')),f=>f.editor.hideResults(1,f.query.id),
    f=>f.editor.beginResultSelection(1,f.query.id,f.result),
    f=>f.editor.publishPresentation(1,f.query.id,[],false),
    f=>f.editor.publishPresentation(1,f.query.id,[],true)]) {
    const f=await modeWriteFixture(),p=f.proposal;f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
    await revoke(f);await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Stale/);
    f.editor.close();
  }
  for(const action of ['cancel','throw']) {
    const f=await modeWriteFixture(),p=f.proposal;
    f.parent.onSelection=()=>{if(action==='throw')throw Error('provider failed');f.editor.cancel();};
    if(action==='throw')assert.throws(()=>f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),/provider failed/);
    else f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
    await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Stale/);f.editor.close();
  }
});


test('failed mode requery consumes its dispatch authority and rejects stale completion',async()=>{
  const f=await modeWriteFixture(),p=f.proposal;
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  f.parent.startQuery=()=>Promise.reject(Error('dispatch failed'));
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/dispatch failed/);
  await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Stale/);
  f.editor.close();
  const g=await modeWriteFixture(),proposal=g.proposal,gate=Promise.withResolvers();
  g.editor.acknowledgeResultModeApplication(proposal.token,observedMode(proposal));
  g.parent.startQuery=()=>gate.promise;
  const pending=g.editor.requeryAfterModeApplication(proposal.token,{searchMode:proposal.searchMode});
  g.editor.cancel();gate.resolve();await assert.rejects(pending,/cancelled/);g.editor.close();
});


const observedExit=p=>({searchMode:null,text:p.edit.text,selectionStart:p.edit.selectionStart,
  selectionEnd:p.edit.selectionEnd,selectionDirection:p.edit.selectionDirection});

test('explicit mode exit accepts confirmed and preview modes with exact unchanged editor observation',async()=>{
  for(const startQuery of [true,false]) {
    const f=await modeWriteFixture({startQuery}),write=f.proposal;
    f.editor.acknowledgeResultModeApplication(write.token,observedMode(write));
    f.editor.select(1,1,3,'backward');const snapshot=f.editor.snapshot;
    const p=f.editor.beginSearchModeExit(1);
    assert.equal(p.previousSearchMode.isPreview,!startQuery);assert.equal(p.searchMode,null);
    assert.equal(p.startQuery,true);assert.equal(p.edit,snapshot);
    assert.equal(f.editor.getSearchModeExit(p.token),p);
    assert.throws(()=>f.editor.publishPresentation(1,f.query.id,[f.result],true),/Invalid/);
    for(const wrong of [{searchMode:write.searchMode},{searchMode:undefined},{text:'changed'},
      {selectionStart:0},{selectionEnd:4},{selectionDirection:'forward'}]) {
      assert.equal(f.editor.acknowledgeSearchModeExit(p.token,{...observedExit(p),...wrong}),false);
      assert.equal(f.editor.snapshot,snapshot);
    }
    assert.equal(f.editor.acknowledgeSearchModeExit(p.token,observedExit(p)),true);
    assert.equal(f.editor.acknowledgeSearchModeExit(p.token,observedExit(p)),false);
    assert.equal(f.editor.beginSearchModeExit(1),null);assert.equal(f.editor.snapshot,snapshot);
    assert.equal(f.parent.started.length,1);assert.deepEqual(f.calls,['before','selected']);f.editor.close();
  }
});

test('mode exit works without result rows and snapshots host-owned mode facts',async()=>{
  const f=await editorFixture(),mode={source:1,isPreview:false};
  await f.editor.edit({searchMode:mode},edit(1,'typed'));mode.source=99;
  const p=f.editor.beginSearchModeExit(1);
  assert.equal(p.previousSearchMode.source,1);assert.equal(Object.isFrozen(p.previousSearchMode),true);
  assert.equal(f.editor.acknowledgeSearchModeExit(p.token,observedExit(p)),true);
  f.editor.close();
});

test('explicit mode exit requests revoke on replacement, caret, edits, cancellation and scope loss',async()=>{
  for(const revoke of [f=>f.editor.beginSearchModeExit(1),f=>f.editor.select(1,0,0),
    f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    const f=await modeWriteFixture();f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal));
    const p=f.editor.beginSearchModeExit(1);await revoke(f);
    assert.equal(f.editor.getSearchModeExit(p.token),null);
    assert.equal(f.editor.acknowledgeSearchModeExit(p.token,observedExit(p)),false);f.editor.close();
  }
  const f=await modeWriteFixture();f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal));
  const p=f.editor.beginSearchModeExit(1);f.loseScope();
  assert.throws(()=>f.editor.getSearchModeExit(p.token),/scope changed/);
});

test('explicit mode exit rejects composition and stale revisions and leaves no-mode state alone',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
  assert.equal(f.editor.beginSearchModeExit(1),null);
  assert.throws(()=>f.editor.beginSearchModeExit(0),/Stale/);
  await f.editor.edit({searchMode:{source:1,isPreview:false}},edit(2,'composing',{composing:true}));
  assert.throws(()=>f.editor.beginSearchModeExit(2),/Stale/);f.editor.close();
});


async function explicitExitQueryFixture(text='exam',selection={}) {
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1,isPreview:false}},edit(1,text,selection));
  const proposal=f.editor.beginSearchModeExit(1);
  return {...f,proposal};
}

test('explicit exit requery follows default autofill policy and shares exact completion acknowledgment',async()=>{
  const f=await explicitExitQueryFixture(),p=f.proposal,state={searchMode:null};
  await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,state,{pageProxyValid:false}),/Stale/);
  f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));const snapshot=f.editor.snapshot;
  await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,state),/page-proxy/);
  await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:{source:1}},{pageProxyValid:false}),/Stale/);
  const q=await f.editor.requeryAfterSearchModeExit(p.token,state,{pageProxyValid:false});
  assert.equal(q.searchString,'exam');assert.equal(q.allowAutofill,true);assert.equal(q.searchMode,undefined);
  assert.equal(f.editor.snapshot,snapshot);
  const result={id:7,type:sharedPolicy.UrlbarShared.RESULT_TYPE.URL,payload:{},
    autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}};
  q.results=[result];q.trimmedSearchString='exam';f.parent.child.notify('onFirstResult',q);
  const completion=f.updates.at(-1).completion;assert.equal(completion.kind,'autofill');
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.editor.acknowledgeCompletion(completion.token,observed(completion)),true);
  assert.equal(f.editor.snapshot.text,'example.test/');
  await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,state,{pageProxyValid:false}),/Stale/);f.editor.close();
});

test('explicit exit zero-prefix query preserves valid-page display and caret suppresses completion writes',async()=>{
  for(const valid of [true,false]) {
    const f=await explicitExitQueryFixture('https://page.test/',{selectionStart:0,selectionEnd:18});
    const p=f.proposal;f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
    const q=await f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:valid});
    assert.equal(q.searchString,valid?'':'https://page.test/');assert.equal(q.allowAutofill,true);
    assert.equal(f.editor.snapshot.text,'https://page.test/');
    q.results=[{id:1,type:sharedPolicy.UrlbarShared.RESULT_TYPE.URL,payload:{},
      autofill:{value:'https://page.test/more',selectionStart:18,selectionEnd:22}}];
    f.parent.child.notify('onFirstResult',q);assert.equal(f.updates.at(-1).completion,null);
    f.editor.publishPresentation(1,q.id,q.results,true);
    assert.equal(f.editor.beginResultRestore(1,q.id).value,'https://page.test/');f.editor.close();
  }
});

test('explicit exit requery revokes on newer editor work, cancellation and scope loss',async()=>{
  for(const revoke of [f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    const f=await explicitExitQueryFixture(),p=f.proposal;f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
    await revoke(f);await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false}),/Stale/);
    f.editor.close();
  }
  const f=await explicitExitQueryFixture(),p=f.proposal;f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
  f.loseScope();await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false}),/scope changed/);
});

test('explicit exit requery consumes authority on failure and rejects cancelled completion',async()=>{
  for(const failure of [true,false]) {
    const f=await explicitExitQueryFixture(),p=f.proposal,gate=Promise.withResolvers();
    f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
    f.parent.startQuery=()=>failure?Promise.reject(Error('dispatch failed')):gate.promise;
    const pending=f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false});
    if(!failure){f.editor.cancel();gate.resolve();}
    await assert.rejects(pending,failure?/dispatch failed/:/cancelled/);
    await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false}),/Stale/);
    f.editor.close();
  }
});


function observedRestoration(p) {
  return {text:p.edit.text,selectionStart:p.edit.selectionStart,selectionEnd:p.edit.selectionEnd,
    selectionDirection:p.edit.selectionDirection,searchMode:p.searchMode ? {...p.searchMode} : null};
}

test('confirmed restoration preserves text and caret, revokes results, and acknowledges once without a query',async()=>{
  const f=await previewFixture(),snapshot=f.editor.snapshot,started=f.parent.started.length;
  const mode={source:1,isPreview:false,entry:'searchbutton'};
  const pending=f.editor.beginSearchModeRestoration(1,mode);mode.source=99;
  const p=await pending;
  assert.equal(p.searchMode.source,1);assert.equal(p.startQuery,false);
  assert.equal(Object.isFrozen(p.searchMode),true);
  assert.equal(f.parent.child.resultsOpen,false);assert.deepEqual([...f.parent.child.visibleResults],[]);
  assert.throws(()=>f.editor.describeResultInput(1,f.query.id,f.result),/Stale/);
  for(const wrong of [{searchMode:null},{text:'changed'},{selectionStart:0},{selectionDirection:'backward'}]) {
    assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,{...observedRestoration(p),...wrong}),false);
  }
  assert.equal(f.editor.getSearchModeRestoration(p.token),p);
  assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,observedRestoration(p)),true);
  assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,observedRestoration(p)),false);
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.started.length,started);
  assert.equal(f.editor.beginSearchModeExit(1).previousSearchMode.source,1);f.editor.close();
});

test('restoration requires confirmed-only state and clears absent or no-longer-valid modes',async()=>{
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'typed'));
  for(const invalid of [undefined,{source:1},{source:1,isPreview:true}]) {
    await assert.rejects(f.editor.beginSearchModeRestoration(1,invalid),/confirmed mode/);
  }
  for(const mode of [null,{engineName:'Removed',isPreview:false},{source:999,isPreview:false}]) {
    const p=await f.editor.beginSearchModeRestoration(1,mode);assert.equal(p.searchMode,null);
    assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,observedRestoration(p)),true);
    assert.equal(f.editor.beginSearchModeExit(1),null);
  }
  await assert.rejects(f.editor.beginSearchModeRestoration(0,null),/Stale/);
  await f.editor.edit({},edit(2,'composing',{composing:true}));
  await assert.rejects(f.editor.beginSearchModeRestoration(2,null),/Stale/);f.editor.close();
});

test('engine restoration revalidates live facts before acknowledgment and can restart after removal',async()=>{
  const f=await editorFixture(),engine={id:'engine',name:'Engine',isGeneralPurposeEngine:true};
  await f.editor.edit({},edit(1,'typed'));f.parent.child.updateEngineStore('changed',engine,-1);
  const mode={engineName:'Engine',isPreview:false,entry:'searchbutton'};
  const p=await f.editor.beginSearchModeRestoration(1,mode);
  assert.equal(p.searchMode.isGeneralPurposeEngine,true);
  f.parent.child.updateEngineStore('changed',{...engine,isGeneralPurposeEngine:false},0);
  assert.equal(f.editor.getSearchModeRestoration(p.token),null);
  assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,observedRestoration(p)),false);
  const replacement=await f.editor.beginSearchModeRestoration(1,mode);
  assert.equal(replacement.searchMode.isGeneralPurposeEngine,false);
  f.parent.child.updateEngineStore('removed',engine,-1);
  assert.equal(f.editor.getSearchModeRestoration(replacement.token),null);
  const cleared=await f.editor.beginSearchModeRestoration(1,mode);
  assert.equal(cleared.searchMode,null);
  assert.equal(f.editor.acknowledgeSearchModeRestoration(cleared.token,observedRestoration(cleared)),true);
  f.editor.close();
});

test('restoration cannot outlive newer mode requests, caret changes, edits, cancellation or scope',async()=>{
  for(const revoke of [f=>f.editor.beginSearchModeRestoration(1,null),f=>f.editor.select(1,0,0),
    f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    for(const during of [true,false]) {
      const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
      const pending=f.editor.beginSearchModeRestoration(1,{source:1,isPreview:false});
      const p=during?null:await pending;await revoke(f);
      if(during) assert.equal(await pending,null);
      else {
        assert.equal(f.editor.getSearchModeRestoration(p.token),null);
        assert.equal(f.editor.acknowledgeSearchModeRestoration(p.token,observedRestoration(p)),false);
      }
      f.editor.close();
    }
  }
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
  const pending=f.editor.beginSearchModeRestoration(1,{source:1,isPreview:false});f.loseScope();
  await assert.rejects(pending,/scope changed/);
});

test('restoration resolves a fresh engine store without querying and rejects scope loss during initialization',async()=>{
  for(const invalidate of [false,true]) {
    const f=fixture({hold:true}),mode={engineName:'Engine',isPreview:false};
    const pending=f.session.prepareSearchModeRestoration(mode);mode.engineName='mutated';
    if(invalidate)f.invalidate();
    f.parent.child.updateEngineStore('init',[{id:'engine',name:'Engine',isGeneralPurposeEngine:true}],null);
    if(invalidate)await assert.rejects(pending,/scope changed/);
    else assert.equal((await pending).engineName,'Engine');
    assert.equal(f.parent.started.length,0);f.session.close();
  }
});


test('independent search-command descriptions resolve local modes, aliases and live engines without editor writes',async()=>{
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1}},edit(1,'displayed'));
  const engine={id:'engine',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true};
  f.parent.child.updateEngineStore('changed',engine,-1);
  const snapshot=f.editor.snapshot,started=f.parent.started.length;
  const local=await f.editor.describeSearchCommand(1,'^ history words');
  assert.equal(local.value,'history words');assert.equal(local.changeSearchMode,true);
  assert.equal(local.searchMode.source,sharedPolicy.UrlbarShared.RESULT_SOURCE.HISTORY);assert.equal(local.searchMode.isPreview,true);
  const remote=await f.editor.describeSearchCommand(1,'@e engine words',{engineId:'engine'});
  assert.equal(remote.value,'engine words');assert.equal(remote.searchMode.engineName,'Engine');assert.equal(remote.searchMode.entry,'searchbutton');
  const unchanged=await f.editor.describeSearchCommand(1,'plain words');
  assert.equal(unchanged.changeSearchMode,false);assert.equal(unchanged.value,'plain words');
  const unsupported=await f.editor.describeSearchCommand(1,'#');
  assert.equal(unsupported.changeSearchMode,true);assert.equal(unsupported.searchMode,null);assert.equal(unsupported.value,'# ');
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.started.length,started);
  f.parent.child.updateEngineStore('removed',engine,-1);
  assert.equal(await f.editor.describeSearchCommand(1,'@e words',{engineId:'engine'}),null);f.editor.close();
});

test('search-command preparation initializes default engines and preserves missing-default failure policy',async()=>{
  for(const fail of [false,true]) {
    const f=fixture({hold:true}),pending=f.session.prepareSearchCommand('? words');
    if(fail)f.parent.child.updateEngineStore('error');
    else f.parent.child.updateEngineStore('init',[{id:'engine',name:'Engine',aliases:[],isGeneralPurposeEngine:true}],0);
    const plan=await pending;assert.equal(plan.value,'words');assert.equal(plan.changeSearchMode,true);
    if(fail)assert.equal(plan.searchMode,null);else assert.equal(plan.searchMode.engineName,'Engine');
    assert.equal(f.parent.started.length,0);f.session.close();
  }
});

test('independent command descriptions reject stale editor snapshots and scope during asynchronous preparation',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
  const pending=f.editor.describeSearchCommand(1,'^ words');f.editor.select(1,0,0);
  await assert.rejects(pending,/superseded/);
  await assert.rejects(f.editor.describeSearchCommand(0,'^ words'),/Stale/);
  await assert.rejects(f.editor.describeSearchCommand(1,42),/Invalid/);
  await assert.rejects(f.editor.describeSearchCommand(1,'words',{engineId:''}),/Invalid/);
  await f.editor.edit({},edit(2,'composing',{composing:true}));
  await assert.rejects(f.editor.describeSearchCommand(2,'^ words'),/Stale/);f.editor.close();
  const g=fixture({hold:true}),initializing=g.session.prepareSearchCommand('? words');
  g.invalidate();g.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(initializing,/scope changed/);
});


test('search-command writes revoke results and require exact one-use mode, text and caret observation',async()=>{
  const f=await previewFixture(),snapshot=f.editor.snapshot,started=f.parent.started.length;
  const p=await f.editor.beginSearchCommand(1,'^ command words');
  assert.equal(p.value,'command words');assert.equal(p.searchMode.isPreview,true);
  assert.equal(Object.isFrozen(p),true);assert.equal(f.editor.snapshot,snapshot);
  assert.equal(f.parent.child.resultsOpen,false);assert.deepEqual([...f.parent.child.visibleResults],[]);
  assert.throws(()=>f.editor.describeResultInput(1,f.query.id,f.result),/Stale/);
  for(const wrong of [{searchMode:null},{text:'wrong'},{selectionStart:0},{selectionDirection:'backward'}]) {
    assert.equal(f.editor.acknowledgeSearchCommand(p.token,{...observedCommand(p),...wrong}),false);
  }
  assert.equal(f.editor.getSearchCommand(p.token),p);
  assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedCommand(p)),true);
  assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedCommand(p)),false);
  assert.equal(f.editor.snapshot.text,p.value);assert.equal(f.editor.snapshot.selectionRevision,snapshot.selectionRevision+1);
  assert.equal(f.parent.started.length,started);assert.deepEqual(f.calls,[]);
  assert.equal(f.editor.beginSearchModeExit(1).previousSearchMode.source,p.searchMode.source);f.editor.close();
});

test('ordinary search commands preserve the current mode while unsupported restrictions explicitly clear it',async()=>{
  const f=await editorFixture(),mode={source:1,isPreview:false,entry:'searchbutton'};
  await f.editor.edit({searchMode:mode},edit(1,'old'));
  const plain=await f.editor.beginSearchCommand(1,'new ordinary text');
  assert.equal(plain.changeSearchMode,false);assert.equal(plain.searchMode.source,1);
  assert.equal(f.editor.acknowledgeSearchCommand(plain.token,{...observedCommand(plain),searchMode:null}),false);
  assert.equal(f.editor.acknowledgeSearchCommand(plain.token,observedCommand(plain)),true);
  const clear=await f.editor.beginSearchCommand(1,'#');
  assert.equal(clear.changeSearchMode,true);assert.equal(clear.searchMode,null);assert.equal(clear.value,'# ');
  assert.equal(f.editor.acknowledgeSearchCommand(clear.token,observedCommand(clear)),true);
  assert.equal(f.editor.beginSearchModeExit(1),null);f.editor.close();
});

test('search-command acknowledgment revalidates live aliases and engine removal',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'old'));
  const engine={id:'engine',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true};
  f.parent.child.updateEngineStore('changed',engine,-1);
  const options={engineId:'engine'},p=await f.editor.beginSearchCommand(1,'@e words',options);
  options.engineId='missing';assert.equal(f.editor.getSearchCommand(p.token),p);
  f.parent.child.updateEngineStore('changed',{...engine,aliases:[]},0);
  assert.equal(f.editor.getSearchCommand(p.token),null);
  assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedCommand(p)),false);
  const changed=await f.editor.beginSearchCommand(1,'@e words',{engineId:'engine'});
  assert.equal(changed.value,'@e words');f.parent.child.updateEngineStore('removed',engine,-1);
  assert.equal(f.editor.getSearchCommand(changed.token),null);
  assert.equal(await f.editor.beginSearchCommand(1,'words',{engineId:'engine'}),null);f.editor.close();
});

test('search-command proposals cannot survive supersession during preparation or after publication',async()=>{
  for(const revoke of [f=>f.editor.beginSearchCommand(1,'new'),f=>f.editor.beginSearchModeRestoration(1,null),
    f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel()]) {
    for(const during of [true,false]) {
      const f=await editorFixture();await f.editor.edit({},edit(1,'old'));
      const pending=f.editor.beginSearchCommand(1,'^ words'),p=during?null:await pending;
      await revoke(f);
      if(during)assert.equal(await pending,null);
      else {assert.equal(f.editor.getSearchCommand(p.token),null);assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedCommand(p)),false);}
      f.editor.close();
    }
  }
  for(const close of [true,false]) {
    const f=await editorFixture();await f.editor.edit({},edit(1,'old'));
    const pending=f.editor.beginSearchCommand(1,'^ words');if(close)f.editor.close();else f.loseScope();
    await assert.rejects(pending,close?/closed/:/scope changed/);
  }
});

test('search-command admission rejects stale/composing edits and failed initialization cannot erase a newer request',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'old'));
  await assert.rejects(f.editor.beginSearchCommand(0,'words'),/Stale/);
  await assert.rejects(f.editor.beginSearchCommand(1,42),/Invalid/);
  const gate=Promise.withResolvers();f.parent.child.engineStore.init=()=>gate.promise;
  const failed=f.editor.beginSearchCommand(1,'words',{engineId:'engine'});
  const newer=await f.editor.beginSearchCommand(1,'^ new words');
  const rejection=assert.rejects(failed,/engine failure/);gate.reject(Error('engine failure'));await rejection;
  assert.equal(f.editor.getSearchCommand(newer.token),newer);
  assert.equal(f.editor.acknowledgeSearchCommand(newer.token,observedCommand(newer)),true);
  await f.editor.edit({},edit(2,'composing',{composing:true}));
  await assert.rejects(f.editor.beginSearchCommand(2,'words'),/Stale/);f.editor.close();
});

function observedCommand(proposal) {
  return {...observed(proposal),searchMode:proposal.searchMode ? {...proposal.searchMode} : null};
}


async function commandRequeryFixture(value='^ command words',options={}) {
  const f=await editorFixture();await f.editor.edit({},edit(1,'old'));
  f.parent.child.updateEngineStore('changed',{id:'engine',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true},-1);
  const proposal=await f.editor.beginSearchCommand(1,value,options);
  return {...f,proposal};
}

test('command follow-up requires acknowledged text and confirmed host mode while preserving query mode order',async()=>{
  const f=await commandRequeryFixture(),p=f.proposal;
  assert.equal(f.editor.getSearchCommandRequery(p.token),null);
  await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:null}),/Stale/);
  f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
  const snapshot=f.editor.snapshot,request=f.editor.getSearchCommandRequery(p.token);
  assert.equal(request.searchMode.isPreview,false);assert.equal(p.searchMode.isPreview,true);
  await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:p.searchMode}),/Stale/);
  const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode});
  assert.equal(q.searchString,p.value);assert.equal(q.searchMode.isPreview,true);assert.equal(q.allowAutofill,true);
  assert.equal(f.editor.snapshot,snapshot);assert.equal(f.editor.getSearchCommandRequery(p.token),null);
  await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode}),/Stale/);
  assert.equal(f.editor.beginSearchModeExit(1).previousSearchMode.isPreview,false);f.editor.close();
});

test('command queries suppress engine-mode autofill and share first-result writes for ordinary text',async()=>{
  for(const engine of [false,true]) {
    const f=await commandRequeryFixture(engine?'@e exam':'exam',engine?{engineId:'engine'}:{}),p=f.proposal;
    f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    const request=f.editor.getSearchCommandRequery(p.token);
    const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode});
    assert.equal(q.allowAutofill,!engine);
    q.results=[{id:1,type:sharedPolicy.UrlbarShared.RESULT_TYPE.URL,payload:{},
      autofill:{value:'example.test/',selectionStart:4,selectionEnd:13}}];q.trimmedSearchString='exam';
    f.parent.child.notify('onFirstResult',q);
    const completion=f.updates.at(-1).completion;
    if(engine)assert.equal(completion,null);
    else {assert.equal(completion.kind,'autofill');assert.equal(f.editor.acknowledgeCompletion(completion.token,observed(completion)),true);}
    f.editor.publishPresentation(1,q.id,q.results,true);
    assert.equal(f.editor.beginResultRestore(1,q.id).value,'exam');f.editor.close();
  }
});

test('command follow-up revokes on composition, edits, caret changes, cancellation and engine mutation',async()=>{
  for(const revoke of [f=>f.editor.edit({},edit(2,'composing',{composing:true})),
    f=>f.editor.edit({},edit(2,'new')),f=>f.editor.select(1,0,0),f=>f.editor.cancel(),
    f=>f.parent.child.updateEngineStore('removed',{id:'engine',name:'Engine'},-1)]) {
    const f=await commandRequeryFixture('@e words',{engineId:'engine'}),p=f.proposal;
    f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    const request=f.editor.getSearchCommandRequery(p.token);await revoke(f);
    assert.equal(f.editor.getSearchCommandRequery(p.token),null);
    await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode}),/Stale/);f.editor.close();
  }
});

test('failed, cancelled and scope-lost command dispatch cannot reuse follow-up authority',async()=>{
  for(const kind of ['failure','cancel','scope']) {
    const f=await commandRequeryFixture('words'),p=f.proposal,gate=Promise.withResolvers();
    f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    const request=f.editor.getSearchCommandRequery(p.token);
    f.parent.startQuery=()=>kind==='failure'?Promise.reject(Error('dispatch failed')):gate.promise;
    const pending=f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode});
    if(kind==='cancel')f.editor.cancel();if(kind==='scope')f.loseScope();gate.resolve();
    await assert.rejects(pending,kind==='failure'?/dispatch failed/:kind==='scope'?/scope changed/:/cancelled/);
    await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode}));f.editor.close();
  }
});


test('independent menu keys feed acknowledged engine/local commands and page-proxy query text',async()=>{
  for(const [key,valid,expected,autofill] of [['engine:engine',true,'',false],['engine:engine',false,'page words',false],
    ['local:^',false,'page words',true],['local:^',true,'',true]]) {
    const f=await commandRequeryFixture();await f.editor.edit({},edit(2,'page words'));
    const p=await f.editor.beginSearchModeMenuCommand(2,key,{pageProxyValid:valid});
    assert.equal(p.value,expected);assert.equal(p.menuPicked,key.startsWith('local')?'local_search':'addon_search');
    assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedCommand(p)),true);
    const request=f.editor.getSearchCommandRequery(p.token);
    const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode});
    assert.equal(q.searchString,expected);assert.equal(q.allowAutofill,autofill);
    assert.equal(q.searchMode.isPreview,true);f.editor.close();
  }
});

test('menu commands reject unavailable entries and leave navigation/install/settings on existing paths',async()=>{
  const f=await commandRequeryFixture();
  for(const key of ['engine:removed','local:unknown','install:https://offer.test/','settings','exit']) {
    assert.equal(await f.editor.beginSearchModeMenuCommand(1,key,{pageProxyValid:false}),null);
  }
  for(const facts of [{where:'tab'},{where:'tabshifted'},{where:'window',shiftKey:true}]) {
    assert.equal(await f.editor.beginSearchModeMenuCommand(1,'engine:engine',{pageProxyValid:false,...facts}),null);
  }
  assert.throws(()=>f.editor.beginSearchModeMenuCommand(1,'engine:engine'),/Invalid menu/);
  f.parent.child.updateEngineStore('changed',{id:'engine',name:'Engine',aliases:[],hideOneOffButton:true},0);
  assert.equal(await f.editor.beginSearchModeMenuCommand(1,'engine:engine',{pageProxyValid:false}),null);f.editor.close();
});

test('menu eligibility changes revoke writes and follow-up queries while redesign keeps local entries available',async()=>{
  for(const acknowledge of [true,false]) {
    const f=await commandRequeryFixture();f.prefs['browser.settings-redesign.enabled']=false;f.prefs['shortcuts.history']=true;
    const p=await f.editor.beginSearchModeMenuCommand(1,'local:^',{pageProxyValid:false});
    if(acknowledge)f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    f.prefs['shortcuts.history']=false;
    assert.equal(acknowledge?f.editor.getSearchCommandRequery(p.token):f.editor.getSearchCommand(p.token),null);
    f.prefs['browser.settings-redesign.enabled']=true;
    assert.ok(await f.editor.beginSearchModeMenuCommand(1,'local:^',{pageProxyValid:false}));f.editor.close();
  }
});

test('menu commands retain local fallback after engine initialization fails and obey pinned SAP',async()=>{
  const f=fixture({hold:true}),pending=f.session.prepareSearchCommand('words',{menuKey:'local:^',pageProxyValid:false});
  f.parent.child.updateEngineStore('error');const p=await pending;assert.equal(p.value,'words');assert.equal(p.menuPicked,'local_search');
  const searchbar=new f.sandbox.UrlbarQuerySession({context:{...f.context,sapName:'searchbar'},isCurrent:()=>true});
  const parent=f.parents.at(-1);parent.child.updateEngineStore('init',[],null);
  assert.equal(await searchbar.prepareSearchCommand('words',{menuKey:'local:^',pageProxyValid:false}),null);
  searchbar.close();f.session.close();
});


test('independent engagement start preserves event facts and abandonment records only validated exposure',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'typed words'}),a={id:1},b={id:2};q.results=[a,b];
  f.session.publishPresentation(q.id,[a,b],true);
  assert.equal(f.session.abandonEngagement(q.id,{type:'blur',timeStamp:20},{searchString:'typed words'}),false);
  const event={type:'input',inputType:'insertText',timeStamp:1};f.session.startEngagement(q.id,event);event.timeStamp=99;
  f.parent.child.notify('onQueryResultRemoved',2,undefined,q.id);
  assert.equal(f.session.abandonEngagement(q.id,{type:'blur',timeStamp:20},{searchString:'typed words'}),true);
  const start=f.parent.engagements.find(e=>e.kind==='start'),record=f.parent.engagements.find(e=>e.kind==='record');
  assert.equal(start.args[0].timeStamp,1);assert.equal(start.args[1],q);assert.equal(start.args[2],'typed words');
  assert.equal(record.args[1].windowMode,'private');assert.equal(record.args[1].searchString,'typed words');
  assert.deepEqual([...record.args[2].engagementData.visibleResults],[a]);assert.equal(record.args[2].engagementData.viewIsOpen,true);
  assert.equal(f.session.ownsQuery(q.id),false);assert.equal(f.parent.child.resultsOpen,false);
  assert.throws(()=>f.session.abandonEngagement(q.id,{type:'blur',timeStamp:21},{searchString:'typed words'}),/Stale/);f.session.close();
});

test('editor abandonment records typed text rather than result preview and hidden results have no exposure',async()=>{
  for(const hidden of [false,true]) {
    const f=await previewFixture();assert.equal(f.editor.startEngagement(1,f.query.id,{type:'focus',timeStamp:1}),true);
    const p=f.editor.beginResultPreview(1,f.query.id,f.result);f.editor.acknowledgeResultPreview(p.token,observed(p));
    if(hidden)f.editor.hideResults(1,f.query.id);
    assert.equal(f.editor.abandonEngagement(1,f.query.id,{type:'tabswitch',timeStamp:2}),true);
    const record=f.parent.engagements.find(e=>e.kind==='record');assert.equal(record.args[1].searchString,'typed words');
    assert.equal(record.args[2].engagementData.visibleResults.length,hidden?0:1);
    assert.equal(record.args[2].engagementData.viewIsOpen,!hidden);assert.equal(f.editor.snapshot.text,p.value);f.editor.close();
  }
});

test('engagement survives ordinary query replacement but discard and teardown drop measurement',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'first'});
  f.session.startEngagement(q.id,{type:'input',timeStamp:1});f.session.cancel();
  const next=await f.session.query({}, {searchString:'second'});
  assert.throws(()=>f.session.startEngagement(q.id,{type:'input',timeStamp:2}),/Stale/);
  f.session.startEngagement(next.id,{type:'input',timeStamp:3},{interactionType:'returned'});
  f.session.discardEngagement();assert.equal(f.session.ownsQuery(next.id),true);
  assert.equal(f.session.abandonEngagement(next.id,{type:'blur',timeStamp:4},{searchString:'second'}),false);
  f.session.startEngagement(next.id,{type:'input',timeStamp:5});f.session.close();f.session.close();
  assert.equal(f.parent.engagements.filter(e=>e.kind==='discard').length,2);
});

test('abandonment is consumed before callbacks and does not cancel a reentrant newer query',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'first'});f.session.startEngagement(q.id,{type:'input',timeStamp:1});
  let newer;
  f.parent.engagementEvent.record=()=>{
    assert.equal(f.session.abandonEngagement(q.id,{type:'blur',timeStamp:3},{searchString:'first'}),false);
    assert.throws(()=>f.session.startEngagement(q.id,{type:'input',timeStamp:3}),/ending/);
    newer=f.session.query({}, {searchString:'new'});
  };
  assert.equal(f.session.abandonEngagement(q.id,{type:'blur',timeStamp:2},{searchString:'first'}),true);
  const next=await newer;assert.equal(f.session.ownsQuery(next.id),true);
  f.session.startEngagement(next.id,{type:'input',timeStamp:4});f.parent.engagementEvent.record=()=>{throw Error('record failed');};
  assert.throws(()=>f.session.abandonEngagement(next.id,{type:'blur',timeStamp:5},{searchString:'new'}),/record failed/);
  assert.equal(f.session.ownsQuery(next.id),false);f.session.close();
});

test('engagement rejects invalid events, stale editor revisions, composition and lost scope',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  for(const event of [{type:'resize',timeStamp:1},{type:'input',timeStamp:-1},{type:'input',timeStamp:NaN}]) {
    assert.throws(()=>f.editor.startEngagement(1,q.id,event),/Invalid/);
  }
  assert.throws(()=>f.editor.startEngagement(0,q.id,{type:'focus',timeStamp:1}),/Stale/);
  f.editor.startEngagement(1,q.id,{type:'focus',timeStamp:1});
  assert.throws(()=>f.editor.abandonEngagement(1,q.id,{type:'click',timeStamp:2}),/Invalid/);
  await f.editor.edit({},edit(2,'composing',{composing:true}));
  assert.throws(()=>f.editor.startEngagement(2,q.id,{type:'input',timeStamp:3}),/Stale/);
  f.loseScope();assert.throws(()=>f.editor.discardEngagement(),/scope changed/);
});

test('history effects preserve nonheuristic, adaptive and deferred-origin input rules',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel,T=f.sandbox.UrlbarShared.RESULT_TYPE;
  const request={urlLoad:{url:'https://history.test/'}};
  f.prefs['autoFill.adaptiveHistory.enabled']=true;
  for(const [result,text,input,whenReady,reintegrate] of [
    [{type:T.URL},'', '',false,true],
    [{type:T.URL,heuristic:true,autofill:{type:'adaptive_url',adaptiveHistoryInput:'adaptive'}},'typed','adaptive',false,false],
    [{type:T.URL,heuristic:true,autofill:{type:'adaptive_origin',adaptiveHistoryInput:''}},'typed','',false,false],
    [{type:T.URL,heuristic:true,autofill:{type:'origin'}},'typed','typed',true,false],
    [{type:T.URL,heuristic:true,autofill:{type:'url'}},'typed',undefined,undefined,true],
    [{type:T.KEYWORD},'typed','typed',false,false],
  ]) {
    const effects=model.resultHistoryEffects(result,request,{searchString:text,isPrivate:false});
    assert.equal(effects.inputHistory?.input,input);assert.equal(effects.inputHistory?.whenReady,whenReady);
    assert.equal(effects.reintegrateURL,reintegrate?request.urlLoad.url:null);
    assert.equal(Object.isFrozen(effects),true);if(effects.inputHistory)assert.equal(Object.isFrozen(effects.inputHistory),true);
  }
  f.session.close();
});

test('private, engine-only and disabled adaptive effects do not propose persistence',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel,T=f.sandbox.UrlbarShared.RESULT_TYPE;
  const result={type:T.URL,heuristic:true,autofill:{type:'origin'},payload:{url:'https://history.test/'}};
  f.prefs['autoFill.adaptiveHistory.enabled']=true;
  for(const [request,isPrivate] of [[{urlLoad:{url:result.payload.url}},true],[{engineSearch:{query:'q',engineName:'e'}},false]]) {
    const p=model.resultHistoryEffects(result,request,{searchString:'typed',isPrivate});
    assert.equal(p.inputHistory,null);assert.equal(p.reintegrateURL,null);
  }
  assert.equal(model.autofillBackspaceURL(result,true),null);
  assert.equal(model.autofillBackspaceURL(result,false),result.payload.url);
  f.prefs['autoFill.adaptiveHistory.enabled']=false;
  assert.equal(model.autofillBackspaceURL(result,false),null);
  assert.equal(model.resultHistoryEffects(result,{urlLoad:{url:result.payload.url}},{searchString:'typed',isPrivate:false}).inputHistory,null);
  f.session.close();
});

test('acknowledged navigation history uses typed basis and revokes on adaptive policy changes',async()=>{
  const f=await editorFixture({isPrivate:false}),q=await f.editor.edit({},edit(1,'typed query'));
  const result={id:1,type:f.sandbox.UrlbarShared.RESULT_TYPE.URL,payload:{url:'https://history.test/'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'typed query',searchString:'untrusted override'};
  f.prefs['autoFill.adaptiveHistory.enabled']=true;
  const p=f.editor.beginNavigationWrite(1,q.id,result,{},options);
  assert.equal(p.plan.history.inputHistory.input,'typed query');
  assert.equal(f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),{},options),true);
  assert.equal(f.editor.getAcknowledgedNavigation(p.token,{},options).plan.history.inputHistory.input,'typed query');
  f.prefs['autoFill.adaptiveHistory.enabled']=false;
  assert.equal(f.editor.getAcknowledgedNavigation(p.token,{},options),null);
  f.prefs['autoFill.adaptiveHistory.enabled']=true;
  assert.equal(f.editor.getAcknowledgedNavigation(p.token,{},options),null);f.editor.close();
});

async function navigationWriteFixture() {
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  const result={id:99,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{engine:'engine',query:'chosen search'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'typed'},event={};
  const begin=()=>f.editor.beginNavigationWrite(1,q.id,result,event,options);
  return {...f,q,result,options,event,begin};
}
const navigationObservation=p=>({text:p.value,selectionStart:0,selectionEnd:0,selectionDirection:'none',searchMode:p.searchMode});

test('navigation write acknowledgment is exact, one-use and preserves typed engagement basis',async()=>{
  const f=await navigationWriteFixture(),before=f.editor.snapshot,p=f.begin();
  assert.equal(f.editor.snapshot,before);assert.equal(p.value,'chosen search');assert.equal(Object.isFrozen(p),true);
  assert.equal(f.editor.getAcknowledgedNavigation(p.token,f.event,f.options),null);
  for(const wrong of [{text:'typed'},{selectionStart:1},{selectionEnd:1},{selectionDirection:'forward'},{searchMode:{source:4}}]) {
    assert.equal(f.editor.acknowledgeNavigationWrite(p.token,{...navigationObservation(p),...wrong},f.event,f.options),false);
  }
  assert.equal(f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options),true);
  assert.equal(f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options),false);
  const ready=f.editor.getAcknowledgedNavigation(p.token,f.event,f.options);
  assert.equal(ready.userTypedValue,'chosen search');assert.equal(ready.edit.selectionStart,0);
  assert.equal(ready.edit.selectionRevision,before.selectionRevision+1);
  f.editor.startEngagement(1,f.q.id,{type:'input',timeStamp:1});
  f.editor.abandonEngagement(1,f.q.id,{type:'blur',timeStamp:2});
  assert.equal(f.parent.engagements.find(e=>e.kind==='record').args[1].searchString,'typed');f.editor.close();
});

test('navigation write authority expires on live policy changes and cannot revive',async()=>{
  for(const changed of ['target','host','result']) {
    const f=await navigationWriteFixture(),p=f.begin();
    if(changed==='result')f.result.payload.query='replacement';
    const event=changed==='target'?{where:'tab'}:f.event;
    const options=changed==='host'?{...f.options,selectedTabIsEmpty:true}:f.options;
    assert.equal(f.editor.getNavigationWrite(p.token,event,options),null);
    f.result.payload.query='chosen search';
    assert.equal(f.editor.getNavigationWrite(p.token,f.event,f.options),null);f.editor.close();
  }
});

test('acknowledged navigation is revoked by dismissal, edits, cancellation and competing writes',async()=>{
  for(const action of ['hide','edit','cancel','replace','viewport']) {
    const f=await navigationWriteFixture(),p=f.begin();
    f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options);
    if(action==='hide')f.editor.hideResults(1,f.q.id);
    if(action==='edit')await f.editor.edit({},edit(2,'new'));
    if(action==='cancel')f.editor.cancel();
    if(action==='replace')f.begin();
    if(action==='viewport'){
      f.editor.publishPresentation(1,f.q.id,[],false);
      f.editor.publishPresentation(1,f.q.id,[f.result],true);
    }
    assert.equal(f.editor.getAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
  }
});

test('navigation writes reject deferred focus and other-tab preparation without changing editor',async()=>{
  const f=await navigationWriteFixture(),before=f.editor.snapshot;
  assert.equal(f.editor.beginNavigationWrite(1,f.q.id,f.result,{where:'tab'},f.options),null);
  assert.equal(f.editor.beginNavigationWrite(1,f.q.id,f.result,new f.sandbox.KeyboardEvent(),{...f.options,deferEnterFocus:true}),null);
  assert.equal(f.editor.snapshot,before);f.editor.close();
});

test('navigation preparation preserves current-load security flags and explicit private overrides',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel;
  for(const [url,popups] of [['javascript:alert(1)',true],['JavaScript:alert(1)',false],['https://example.test/',false]]) {
    const params={private:false};const plan=model.navigationPlan({loadRequest:{urlLoad:{url}},where:'current',params,sapName:'urlbar',isPrivate:true});
    assert.equal(plan.openParams.allowPopups,popups);assert.equal(plan.openParams.allowPinnedTabHostChange,true);
    assert.equal(plan.openParams.allowThirdPartyFixup,true);assert.equal(plan.openParams.indicateErrorPageLoad,true);
    assert.equal(plan.openParams.private,false);assert.deepEqual(params,{private:false});assert.equal(plan.resetSelection,true);
    assert.equal(plan.revertBeforeLoad,false);
  }
  const plan=model.navigationPlan({loadRequest:{engineSearch:{query:'terms'}},where:'current',params:{},sapName:'urlbar',isPrivate:true,deferEnterFocus:true});
  assert.equal(plan.editorValue,'terms');assert.equal(plan.openParams.private,true);assert.equal(plan.resetSelection,false);
  assert.equal(plan.openParams.avoidBrowserFocus,true);assert.equal(plan.openParams.allowPopups,false);f.session.close();
});

test('navigation preparation preserves SAP-specific reversion and background focus',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel;
  for(const sapName of ['urlbar','searchbar','newtab_searchbar','smartbar'])for(const where of ['current','tab','window']) {
    const plan=model.navigationPlan({loadRequest:{urlLoad:{url:'not a valid URL'}},where,params:{avoidBrowserFocus:true},sapName,isPrivate:false});
    assert.equal(plan.editorValue,sapName==='urlbar' && where==='current'?'not a valid URL':undefined);
    assert.equal(plan.revertBeforeLoad,where!=='current' && !['searchbar','newtab_searchbar'].includes(sapName));
    assert.equal(plan.resetSelection,false);
    assert.equal(plan.openParams.allowPinnedTabHostChange,where==='current'?sapName==='urlbar':undefined);
    assert.equal(plan.openParams.indicateErrorPageLoad,where==='current'?true:undefined);
  }
  f.session.close();
});

test('independent navigation preparation preserves editor and requires valid scope and focus facts',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'terms')),T=f.sandbox.UrlbarShared.RESULT_TYPE;
  const result={id:1,type:T.SEARCH,payload:{engine:'engine',query:'terms'}};q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'terms',deferEnterFocus:true};
  const before=f.editor.snapshot;
  const plan=f.editor.describeResultNavigation(1,q.id,result,new f.sandbox.KeyboardEvent(),options);
  assert.equal(plan.editorValue,'terms');assert.equal(plan.resetSelection,false);assert.equal(plan.openParams.avoidBrowserFocus,true);
  assert.equal(Object.isFrozen(plan),true);assert.equal(Object.isFrozen(plan.openParams),true);assert.equal(f.editor.snapshot,before);
  assert.equal(f.editor.describeResultNavigation(1,q.id,result,{},options).resetSelection,true,'non-Enter must not defer focus');
  assert.throws(()=>f.editor.describeResultNavigation(1,q.id,result,{}, {...options,deferEnterFocus:1}),/Invalid/);
  assert.throws(()=>f.editor.describeResultNavigation(0,q.id,result,{},options),/Stale/);
  f.editor.hideResults(1,q.id);assert.throws(()=>f.editor.describeResultNavigation(1,q.id,result,{},options),/Invalid/);f.editor.close();
});

test('shared load policy preserves heuristic DNS and scheme handling without mutating requests',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel,T=f.sandbox.UrlbarShared.RESULT_TYPE;
  const result={type:T.URL,heuristic:true},request={urlLoad:{url:'http://intranet/',postData:null}};
  f.prefs['browser.fixup.dns_first_for_single_words']=true;
  const policy=model.resultLoadPolicy(result,request,'intranet');
  assert.equal(policy.loadRequest.urlLoad.url,'intranet');assert.equal(request.urlLoad.url,'http://intranet/');
  assert.equal(policy.openParams.schemelessInput,2);
  for(const [value,scheme] of [['https://example.test/',1],['http://intranet',1],['file:///test',1],['example.test/',2]]) {
    const p=model.resultLoadPolicy(result,request,value);assert.equal(p.loadRequest,request);assert.equal(p.openParams.schemelessInput,scheme);
  }
  f.prefs['browser.fixup.dns_first_for_single_words']=false;
  assert.equal(model.resultLoadPolicy(result,request,'intranet').loadRequest,request);
  assert.equal(model.resultLoadPolicy({type:T.URL},request,'intranet').openParams.schemelessInput,undefined);
  assert.equal(model.resultLoadPolicy({type:T.KEYWORD},request,'intranet').openParams.allowInheritPrincipal,true);
  assert.equal(model.resultLoadPolicy({type:T.SEARCH},request,'intranet').openParams.allowInheritPrincipal,undefined);f.session.close();
});

test('independent primary load descriptions retain search terms, POST data and keyword principals',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'load'}),T=f.sandbox.UrlbarShared.RESULT_TYPE;
  for(const result of [{id:1,type:T.URL,payload:{url:'https://example.test/'}},
    {id:2,type:T.KEYWORD,payload:{url:'https://keyword.test/',postData:'q=terms'}},
    {id:3,type:T.SEARCH,payload:{engine:'engine',suggestion:'suggested',query:'original'}}]) {
    q.results=[result];f.session.publishPresentation(q.id,[result],true);
    const p=f.session.describeResultLoad(q.id,result,{}, {selectedTabIsEmpty:false,originalUntrimmedValue:'original'});
    assert.equal(p.openParams.allowInheritPrincipal,result.type===T.KEYWORD);
    assert.equal(Object.isFrozen(p.loadRequest),true);
    if(result.type===T.SEARCH){assert.equal(p.loadRequest.engineSearch.query,'suggested');assert.equal(p.loadRequest.engineSearch.engineName,'engine');}
    else {assert.equal(p.loadRequest.urlLoad.url,result.payload.url);assert.equal(p.loadRequest.urlLoad.postData,result.payload.postData??null);}
    assert.equal(f.session.ownsResult(q.id,result),true);
  }
  f.session.close();
});

test('load descriptions reject unsupported, canonized, hidden, stale and composing requests',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'load')),T=f.sandbox.UrlbarShared.RESULT_TYPE;
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'load'};
  for(const result of [{id:1,type:T.TAB_SWITCH,payload:{url:'https://tab.test/'}},
    {id:2,type:T.SEARCH,payload:{engine:'e',query:'q',providesSearchMode:true}},
    {id:3,type:T.URL,payload:{}}]) {
    q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
    assert.equal(f.editor.describeResultLoad(1,q.id,result,{},options),null);
  }
  const result={id:4,type:T.URL,payload:{url:'https://example.test/'}};q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  f.prefs.ctrlCanonizesURLs=true;
  assert.equal(f.editor.describeResultLoad(1,q.id,result,new f.sandbox.KeyboardEvent({ctrlKey:true}),options),null);
  assert.throws(()=>f.editor.describeResultLoad(1,q.id,result,{}, {selectedTabIsEmpty:false}),/untrimmed/);
  assert.throws(()=>f.editor.describeResultLoad(0,q.id,result,{},options),/Stale/);
  f.editor.hideResults(1,q.id);assert.throws(()=>f.editor.describeResultLoad(1,q.id,result,{},options),/Invalid/);
  await f.editor.edit({},edit(2,'ime',{composing:true}));assert.throws(()=>f.editor.describeResultLoad(2,q.id,result,{},options),/Stale/);f.editor.close();
});

test('shared result opening preserves explicit destinations, privacy, containers and background policy',()=>{
  const f=fixture(),model=f.sandbox.UrlbarQueryModel;
  const result={payload:{engine:'engine',isSponsored:true,url:'https://sponsored.test/'}};
  for(const [action,target,privateValue,container] of [[{openIn:'tab'},'tab',false],
    [{openIn:'window'},'window',false],[{openIn:'private-window'},'window',true],
    [{openIn:'window',userContextId:'7'},'tab',false,7]]) {
    let backgroundCalls=0;
    const policy=model.resultOpenPolicy({result,sapName:'urlbar',isPrivate:false,resultAction:action},
      ()=>assert.fail('explicit destination read event policy'),(where,params)=>{backgroundCalls++;
        assert.equal(where,target);assert.equal(params.forceForeground,false);return true;});
    assert.equal(backgroundCalls,1);assert.equal(policy.where,target);assert.equal(policy.keepViewOpen,true);
    assert.equal(policy.openParams.avoidBrowserFocus,true);assert.equal(policy.openParams.private,privateValue);
    assert.equal(policy.openParams.userContextId,container);assert.equal(policy.openParams.allowInheritPrincipal,false);
    assert.equal(policy.openParams.globalHistoryOptions.triggeringSponsoredURL,result.payload.url);
    if(container)assert.equal(policy.openParams.eventDetail.containerSource,'urlbar_result_context_menu');
  }
  for(const [action,target] of [[null,'current'],[{url:'https://help.test/'},'tab']]) {
    const policy=model.resultOpenPolicy({result,sapName:'urlbar',isPrivate:true,resultAction:action},()=> 'current',
      (_where,params)=>{assert.equal(params.forceForeground,true);return false;});
    assert.equal(policy.where,target);assert.equal(policy.openParams.private,true);assert.equal(policy.keepViewOpen,false);
  }
  f.session.close();
});

test('independent opening shares keyboard, canonization, preference and empty-tab rules',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'open'}),result={id:1,payload:{}};
  q.results=[result];f.session.publishPresentation(q.id,[result],true);
  const key=facts=>new f.sandbox.KeyboardEvent(facts);
  const describe=(event,empty=false)=>f.session.describeResultOpenPolicy(q.id,result,event,{selectedTabIsEmpty:empty});
  assert.equal(describe(key({altKey:true})).where,'tab');
  assert.equal(describe(key({altGraph:true,shiftKey:true})).where,'tabshifted');
  assert.equal(describe(key({altKey:true}),true).where,'current');
  assert.equal(describe({where:'tab'},true).where,'tab','mouse does not reuse empty tab');
  f.prefs.ctrlCanonizesURLs=true;assert.equal(describe(key({ctrlKey:true,where:'tab'})).where,'current');
  assert.equal(describe(key({ctrlKey:true,_disableCanonization:true,where:'tab'})).where,'tab');
  f.prefs.openintab=true;
  assert.equal(describe(key({})).where,'tab');assert.equal(describe({where:'tab'}).where,'current');
  assert.equal(describe({where:'current'},true).where,'current');
  const policy=describe(key({}));assert.equal(policy.openParams.private,true);
  assert.equal(Object.isFrozen(policy),true);assert.equal(Object.isFrozen(policy.openParams.globalHistoryOptions),true);
  f.session.close();
});

test('opening descriptions reject unexposed, stale and reentrant results without granting navigation',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'open'}),result={id:1,payload:{}};q.results=[result];
  assert.throws(()=>f.session.describeResultOpenPolicy(q.id,result,{},{}),/empty-tab/);
  const describe=()=>f.session.describeResultOpenPolicy(q.id,result,{}, {selectedTabIsEmpty:false});
  assert.throws(describe,/Invalid/);f.session.publishPresentation(q.id,[result],true);assert.ok(describe());
  f.sandbox.UrlbarContentUtils.willLoadInBackground=()=>{f.session.cancel();return false;};
  assert.throws(describe,/Stale/);f.session.close();
});

test('editor opening descriptions require the current non-composing revision',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'open')),result={id:1,payload:{}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const before=f.editor.snapshot;assert.ok(f.editor.describeResultOpenPolicy(1,q.id,result,{}, {selectedTabIsEmpty:false}));
  assert.equal(f.editor.snapshot,before);
  assert.throws(()=>f.editor.describeResultOpenPolicy(0,q.id,result,{}, {selectedTabIsEmpty:false}),/Stale/);
  await f.editor.edit({},edit(2,'ime',{composing:true}));
  assert.throws(()=>f.editor.describeResultOpenPolicy(2,q.id,result,{}, {selectedTabIsEmpty:false}),/Stale/);f.editor.close();
});

test('search attribution preserves handoff, switcher, mode and persisted precedence across SAPs',()=>{
  const {sandbox}=fixture();const model=sandbox.UrlbarQueryModel;
  for(const isAddressbar of [false,true])for(const isHandoff of [false,true])
    for(const switcherItem of [false,true])for(const searchMode of [null,{source:4}])
      for(const isOneOff of [false,true])for(const persistedSearchTerms of [false,true]) {
        const facts={sapName:isAddressbar?'urlbar':'searchbar',isAddressbar,isHandoff,switcherItem,searchMode,isOneOff,persistedSearchTerms};
        const expected=!isAddressbar?'searchbar':isHandoff?'urlbar_handoff':
          switcherItem || searchMode && !isOneOff?'urlbar_searchmode':persistedSearchTerms && !isOneOff?'urlbar_persisted':'urlbar';
        assert.equal(model.searchSource(facts),expected,JSON.stringify(facts));
      }
});

test('independent abandonment forwards host attribution with editor-owned mode and rejects invalid facts',async()=>{
  for(const [facts,expected] of [[{},'urlbar'],[{persistedSearchTerms:true},'urlbar_persisted'],
    [{isHandoff:true,persistedSearchTerms:true},'urlbar_handoff']]) {
    const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
    f.editor.startEngagement(1,q.id,{type:'input',timeStamp:1});
    for(const invalid of [{isHandoff:'true'},{persistedSearchTerms:1}]) {
      assert.throws(()=>f.editor.abandonEngagement(1,q.id,{type:'blur',timeStamp:2},invalid),/Invalid/);
      assert.equal(f.parent.engagements.filter(e=>e.kind==='record').length,0);
    }
    assert.equal(f.editor.abandonEngagement(1,q.id,{type:'blur',timeStamp:3},facts),true);
    const record=f.parent.engagements.find(e=>e.kind==='record');
    assert.equal(record.args[1].searchSource,expected);
    assert.equal(record.args[2].engagementData.searchSource,expected);f.editor.close();
  }
  const f=fixture(),q=await f.session.query({}, {searchString:'mode'});
  f.session.startEngagement(q.id,{type:'focus',timeStamp:1});
  f.session.abandonEngagement(q.id,{type:'blur',timeStamp:2},
    {searchString:'mode',searchMode:{source:4},persistedSearchTerms:true});
  assert.equal(f.parent.engagements.find(e=>e.kind==='record').args[1].searchSource,'urlbar_searchmode');f.session.close();
});

test('retained attribution binding preserves lazy DOM and browser-state reads',()=>{
  const {sandbox}=fixture();
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarInputBase.mjs',import.meta.url),'utf8');
  const method=source.slice(source.indexOf('  getSearchSource(event) {'),source.indexOf('  // Private methods below.'))
    .replaceAll('this.#isAddressbar','this.isAddressbar').replaceAll('this.#sapName','this.sapName');
  const context=vm.createContext({UrlbarQueryModel:sandbox.UrlbarQueryModel});
  const binding=vm.runInContext('({'+method+'})',context);
  for(const [facts,expected,reads] of [
    [{isAddressbar:false},'searchbar',[]],
    [{_isHandoffSession:true},'urlbar_handoff',[]],
    [{switcher:true},'urlbar_searchmode',['switcher']],
    [{mode:{source:4}},'urlbar_searchmode',['switcher','oneoff','mode']],
    [{terms:'kept terms',shouldPersist:false},'urlbar_persisted',['switcher','oneoff','mode','browser']],
    [{mode:{source:4},oneoff:true,terms:'kept terms'},'urlbar',['switcher','oneoff','mode','browser']],
  ]) {
    const calls=[],event={};const host={isAddressbar:true,sapName:facts.isAddressbar===false?'searchbar':'urlbar',
      ...facts,window:{gBrowser:{selectedBrowser:{}}},
      searchModeSwitcher:{eventTargetIsPanelItem:e=>{assert.equal(e,event);calls.push('switcher');return !!facts.switcher;}},
      view:{oneOffSearchButtons:{eventTargetIsAOneOff:e=>{assert.equal(e,event);calls.push('oneoff');return !!facts.oneoff;}}},
      get searchMode(){calls.push('mode');return facts.mode;},
      getBrowserState(){calls.push('browser');return {persist:{searchTerms:facts.terms,shouldPersist:facts.shouldPersist}};}};
    assert.equal(binding.getSearchSource.call(host,event),expected);assert.deepEqual(calls,reads);
  }
});

test('Firefox recorder shares event building for explicit snapshots and retained input without hidden input reads',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const builds=[],records=[],errors=[],fallback={visibleResults:[],searchMode:null},smart={};let reads=0;
  const utils={startInteractionType:()=> 'typed',collectSnapshot:(event,details,start)=>start?{method:'abandonment',internalDetails:details}:null,
    buildRecordedEngagement:(snapshot,data,smartbar)=>{builds.push({data,smartbar});return {built:null,previousSearchWords:new Set()};},
    buildRecordedDisableCandidate:()=>({test:true}),engagementData:input=>input.data,smartbarData:input=>input.smart};
  const context=vm.createContext({console:{error:(...args)=>errors.push(args)},
    Glean:{urlbar:new Proxy({}, {get:()=>({set(){}})})},lazy:{UrlbarPrefs:{addObserver(){},get:()=>true},UrlbarTelemetryUtils:utils}});
  vm.runInContext(source.slice(source.indexOf('export class TelemetryEvent')).replace('export class','class')+';this.Recorder=TelemetryEvent;',context);
  const controller={sapName:'urlbar',_lastQueryContextWrapper:{queryContext:{}},get input(){reads++;return {data:fallback,smart};}};
  const recorder=new context.Recorder(controller);recorder.recordFromChild=data=>records.push(data);
  const explicit={engagementData:{visibleResults:[{providerName:'UrlbarProviderQuickSuggest'}],searchMode:null},smartbarData:{}};
  recorder.start({type:'input',timeStamp:1},{},'typed');recorder.record({type:'blur',timeStamp:2},{searchString:'typed',searchSource:'urlbar'},explicit);
  assert.equal(reads,0);assert.equal(builds[0].data,explicit.engagementData);assert.equal(builds[0].smartbar,explicit.smartbarData);
  assert.equal(records[0].visibleResults,explicit.engagementData.visibleResults);assert.equal(records[0].disableBuilt.test,true);
  recorder.start({type:'input',timeStamp:3},{},'typed');recorder.record({type:'blur',timeStamp:4},{searchString:'typed',searchSource:'urlbar'});
  assert.equal(reads,2);assert.equal(builds[1].data,fallback);assert.equal(builds[1].smartbar,smart);assert.deepEqual(errors,[]);
});


test('shared engagement classification preserves Firefox result and provider precedence',()=>{
  const f=fixture(),classify=f.sandbox.UrlbarResultPresentation.telemetryTypeFromResult;
  const T=f.sandbox.UrlbarShared.RESULT_TYPE,S=f.sandbox.UrlbarShared.RESULT_SOURCE;
  const cases=[
    [null,'unknown'],[{type:-1},'unknown'],
    ...[['TAB_SWITCH','switchtab'],['KEYWORD','keyword'],['OMNIBOX','extension'],['REMOTE_TAB','remotetab'],['TIP','tip'],['AI_CHAT','ai_chat']].map(([type,name])=>[{type:T[type]},name]),
    [{type:T.SEARCH,payload:{}},'searchengine'],
    [{type:T.SEARCH,providerName:'UrlbarProviderRecentSearches',source:S.HISTORY},'recent_search'],
    [{type:T.SEARCH,source:S.HISTORY},'formhistory'],
    [{type:T.SEARCH,providerName:'UrlbarProviderTabToSearch'},'tabtosearch'],
    [{type:T.SEARCH,providerName:'UrlbarProviderAiChat'},'ai_search_fallback'],
    [{type:T.SEARCH,payload:{suggestion:'q'}},'searchsuggestion'],
    [{type:T.SEARCH,payload:{suggestion:'q',trending:true},isRichSuggestion:true},'trending_rich'],
    [{type:T.URL,autofill:{type:'adaptive_origin'}},'autofill_adaptive_origin'],
    [{type:T.URL,source:S.OTHER_LOCAL,heuristic:true},'visiturl'],
    [{type:T.URL,providerName:'UrlbarProviderQuickSuggest'},'quicksuggest'],
    [{type:T.URL,providerName:'UrlbarProviderClipboard'},'clipboard'],
    [{type:T.URL,source:S.BOOKMARKS},'bookmark'],
    [{type:T.URL,source:S.BOOKMARKS,providerName:'UrlbarProviderInputHistory'},'bookmarkadaptive'],
    [{type:T.URL,source:S.HISTORY,providerName:'UrlbarProviderInputHistory'},'historyadaptive'],
    [{type:T.URL,source:S.HISTORY},'history'],
    [{type:T.DYNAMIC,providerName:'UrlbarProviderTabToSearch'},'tabtosearch'],
    [{type:T.DYNAMIC},'dynamic'],
    ...[['BOOKMARK','bookmarks'],['OPENPAGE','tabs'],['HISTORY','history'],['ACTION','actions']].map(([token,name])=>[
      {type:T.RESTRICT,payload:{keyword:f.sandbox.UrlbarShared.RESTRICT_TOKENS[token]}},'restrict_keyword_'+name]),
    [{type:T.RESTRICT,payload:{keyword:'unknown'}},'unknown'],
  ];
  for(const [result,expected] of cases)assert.equal(classify(result),expected,JSON.stringify(result));
  f.session.close();
});

test('engagement descriptions capture validated exposure and owned editor facts without recording',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed query'));
  const result={id:71,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{engine:'engine',query:'picked query'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const before=f.editor.snapshot,count=f.parent.engagements.length;
  const description=f.editor.describeResultEngagement(1,q.id,result,{searchString:'spoof',searchMode:{engineName:'spoof'}});
  assert.equal(description.details.searchString,'typed query');assert.equal(description.details.searchMode,null);
  assert.equal(description.details.selType,'searchengine');assert.equal(description.details.windowMode,'private');
  assert.equal(description.details.result,result);assert.equal(description.state.engagementData.visibleResults[0],result);
  assert.equal(Object.isFrozen(description.state.engagementData.visibleResults),true);
  assert.equal(f.editor.snapshot,before);assert.equal(f.parent.engagements.length,count);
  f.editor.publishPresentation(1,q.id,[],false);
  assert.throws(()=>f.editor.describeResultEngagement(1,q.id,result),/Stale/);
  assert.equal(description.state.engagementData.visibleResults.length,1);f.editor.close();
});

test('engagement descriptions reject copied results, stale revisions and invalid source facts',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  const result={id:72,type:f.sandbox.UrlbarShared.RESULT_TYPE.URL,source:f.sandbox.UrlbarShared.RESULT_SOURCE.HISTORY,payload:{url:'https://history.test/'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  assert.throws(()=>f.editor.describeResultEngagement(1,q.id,{...result}),/Stale/);
  assert.throws(()=>f.editor.describeResultEngagement(2,q.id,result),/Stale/);
  assert.throws(()=>f.editor.describeResultEngagement(1,q.id,result,{isHandoff:1}),/Invalid/);
  assert.equal(f.editor.describeResultEngagement(1,q.id,result,{isHandoff:true}).details.searchSource,'urlbar_handoff');
  f.editor.cancel();assert.throws(()=>f.editor.describeResultEngagement(1,q.id,result),/Stale/);f.editor.close();
});


test('search recording policy preserves destination routing and explicit payload fields',()=>{
  const f=fixture();
  for(const where of ['current','window','tab','tabshifted']) {
    const p=f.sandbox.UrlbarQueryModel.searchRecording({engineId:'id',query:'  query ',where,
      searchSource:'urlbar',searchActionDetails:{alias:'@e',isOneOff:'override'},isOneOff:false,newtabSessionId:'session'});
    assert.equal(p.method,where.startsWith('tab')?'recordSearchInOpenedTab':'recordSearch');
    assert.equal(p.data.query,'  query ');assert.equal(p.data.engineId,'id');
    assert.equal(p.data.details.alias,'@e');assert.equal(p.data.details.isOneOff,false);
    assert.equal(p.data.details.newtabSessionId,'session');assert.equal(p.data.opensInPrivateWindow,false);
  }
  f.session.close();
});

test('independent search recordings resolve live engines and preserve suggestion and private semantics',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  await f.parent.child.engineStore.init();
  const engine={id:'live-engine',name:'Engine'};f.parent.child.updateEngineStore('changed',engine,-1);
  const result={id:81,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,source:f.sandbox.UrlbarShared.RESULT_SOURCE.HISTORY,
    payload:{engine:'Engine',query:'query',suggestion:' suggestion ',keyword:'@e'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'typed',newtabSessionId:'handoff',isHandoff:true};
  const describe=(event={})=>f.editor.describeResultSearchRecording(1,q.id,result,event,options);
  const before=f.editor.snapshot,p=describe({where:'tab'});
  assert.equal(p.method,'recordSearchInOpenedTab');assert.equal(p.data.engineId,'live-engine');
  assert.equal(p.data.query,' suggestion ');assert.equal(p.data.searchSource,'urlbar_handoff');
  assert.equal(p.data.details.isSuggestion,true);assert.equal(p.data.details.isFormHistory,true);
  assert.equal(p.data.details.isOneOff,false);assert.equal(p.data.details.alias,'@e');
  assert.equal(Object.isFrozen(p.data.details),true);assert.equal(f.editor.snapshot,before);
  result.payload.inPrivateWindow=true;
  const privatePlan=describe({where:'tab'});assert.equal(privatePlan.method,'recordSearch');
  assert.equal(privatePlan.data.opensInPrivateWindow,true);
  const navigation=f.editor.describeResultNavigation(1,q.id,result,{where:'tab'},options);
  assert.equal(navigation.where,'window');assert.equal(navigation.openParams.private,true);
  assert.equal(navigation.revertBeforeLoad,true);
  assert.equal(f.editor.beginNavigationWrite(1,q.id,result,{},options),null);
  f.parent.child.updateEngineStore('removed',engine,-1);assert.equal(describe(),null);
  assert.equal(f.parent.engagements.length,0);f.editor.close();
});

test('search recording descriptions reject stale, unexposed and unsupported results',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'typed'));
  await f.parent.child.engineStore.init();f.parent.child.updateEngineStore('changed',{id:'e',name:'Engine'},-1);
  const result={id:82,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{engine:'Engine',query:'query'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'typed'};
  const describe=(r=result,o=options)=>f.editor.describeResultSearchRecording(1,q.id,r,{},o);
  assert.throws(()=>describe({...result}),/Invalid/);
  assert.throws(()=>describe(result,{...options,newtabSessionId:1}),/Invalid/);
  result.payload.providesSearchMode=true;assert.equal(describe(),null);delete result.payload.providesSearchMode;
  result.type=f.sandbox.UrlbarShared.RESULT_TYPE.URL;result.payload.url='https://example.test/';assert.equal(describe(),null);
  f.editor.publishPresentation(1,q.id,[],false);assert.throws(()=>describe(),/Invalid/);f.editor.close();
});


test('post-search DNS policy preserves heuristic, mode and preference gates',()=>{
  const f=fixture(),m=f.sandbox.UrlbarQueryModel,result={heuristic:true};
  const facts={isAddressbar:true,searchMode:null,originalUntrimmedValue:'  intranet  '};
  f.prefs.dnsResolveSingleWordsAfterSearch=1;f.prefs["browser.fixup.dns_first_for_single_words"]=false;
  assert.equal(m.searchFixupHost(result,facts),'intranet');
  for(const override of [{isAddressbar:false},{searchMode:{source:1}},{originalUntrimmedValue:'two words'},
    {originalUntrimmedValue:'https://example.test/'},{originalUntrimmedValue:''}]) {
    assert.equal(m.searchFixupHost(result,{...facts,...override}),null);
  }
  assert.equal(m.searchFixupHost({heuristic:false},facts),null);
  f.prefs['browser.fixup.dns_first_for_single_words']=true;assert.equal(m.searchFixupHost(result,facts),null);
  f.prefs['browser.fixup.dns_first_for_single_words']=false;
  for(const threshold of [0,-1]) {f.prefs.dnsResolveSingleWordsAfterSearch=threshold;assert.equal(m.searchFixupHost(result,facts),null);}
  f.prefs.dnsResolveSingleWordsAfterSearch=2;assert.equal(m.searchFixupHost(result,facts),'intranet');f.session.close();
});

test('selected-search backend eligibility preserves privacy, history and exact default engine gates',()=>{
  const f=fixture(),m=f.sandbox.UrlbarQueryModel,engine={id:'engine',isAppProvided:true};
  const result={source:f.sandbox.UrlbarShared.RESULT_SOURCE.SEARCH,payload:{}};
  const facts={isAddressbar:true,isPrivate:false,engine,defaultEngine:engine};
  assert.equal(m.searchBackendEligible(result,facts),true);
  for(const override of [{isAddressbar:false},{isPrivate:true},{engine:null},{engine:{...engine,isAppProvided:false}},
    {defaultEngine:{...engine}},{defaultEngine:null}])assert.equal(m.searchBackendEligible(result,{...facts,...override}),false);
  assert.equal(m.searchBackendEligible({...result,source:f.sandbox.UrlbarShared.RESULT_SOURCE.HISTORY},facts),false);
  assert.equal(m.searchBackendEligible({...result,payload:{inPrivateWindow:true}},facts),false);f.session.close();
});

test('independent search effects use pinned privacy, owned mode and live default policy without side effects',async()=>{
  const f=await editorFixture({isPrivate:false}),q=await f.editor.edit({},edit(1,'intranet'));
  const engine={id:'e',name:'Engine',isAppProvided:true};
  f.parent.child.updateEngineStore('changed',engine,-1);f.parent.child.updateEngineStore('default',engine);
  const result={id:83,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,heuristic:true,payload:{engine:'Engine',query:'intranet'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  f.parent.checkKeywordURIFixup=()=>assert.fail('description started DNS');
  f.parent.recordSearch=()=>assert.fail('description recorded search');
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:' intranet ',isPrivate:true,searchMode:{source:1}};
  const describe=()=>f.editor.describeResultSearchRecording(1,q.id,result,{},options);
  f.prefs.dnsResolveSingleWordsAfterSearch=1;f.prefs["browser.fixup.dns_first_for_single_words"]=false;
  const first=describe();assert.equal(first.effects.fixupHost,'intranet');assert.equal(first.effects.backendEligible,true);
  assert.equal(Object.isFrozen(first.effects),true);
  f.prefs['browser.fixup.dns_first_for_single_words']=true;assert.equal(describe().effects.fixupHost,null);
  const other={id:'other',name:'Other',isAppProvided:true};
  f.parent.child.updateEngineStore('changed',other,-1);f.parent.child.updateEngineStore('default',other);
  assert.equal(describe().effects.backendEligible,false);assert.equal(first.effects.backendEligible,true);
  result.payload.inPrivateWindow=true;assert.equal(describe().effects.backendEligible,false);f.editor.close();
});


test('acknowledged navigation is consumed once with live search and owned engagement facts',async()=>{
  const f=await navigationWriteFixture();
  f.parent.child.updateEngineStore('changed',{id:'e',name:'engine'},-1);
  const p=f.begin();assert.equal(f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options),null);
  f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options);
  assert.equal(f.editor.takeAcknowledgedNavigation({},f.event,f.options),null);
  const packet=f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options);
  assert.equal(packet.navigation.userTypedValue,'chosen search');assert.equal(packet.engagement.details.searchString,'typed');
  assert.equal(packet.searchRecording.data.engineId,'e');assert.equal(packet.searchRecording.data.query,'chosen search');
  assert.equal(Object.isFrozen(packet),true);assert.equal(f.parent.engagements.length,0);
  assert.equal(f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options),null);
  assert.equal(f.editor.getAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
});

test('failed navigation consumption cannot revive after engine or fact repair',async()=>{
  for(const failure of ['engine','facts','target','dismissal']) {
    const f=await navigationWriteFixture(),engine={id:'e',name:'engine'};
    f.parent.child.updateEngineStore('changed',engine,-1);
    const p=f.begin();f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options);
    if(failure==='engine')f.parent.child.updateEngineStore('removed',engine,-1);
    if(failure==='dismissal')f.editor.publishPresentation(1,f.q.id,[],false);
    assert.equal(f.editor.takeAcknowledgedNavigation(p.token,failure==='target'?{where:'tab'}:f.event,
      failure==='facts'?{...f.options,isHandoff:1}:f.options),null);
    if(failure==='engine')f.parent.child.updateEngineStore('changed',engine,-1);
    if(failure==='dismissal')f.editor.publishPresentation(1,f.q.id,[f.result],true);
    assert.equal(f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
  }
});

test('navigation consumption rejects synchronous reentry and supports URL preparations without engines',async()=>{
  const f=await navigationWriteFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.URL;
  f.result.payload={url:'https://example.test/'};
  const p=f.begin();f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options);
  const original=f.editor.describeResultNavigation.bind(f.editor);let reentry;
  f.editor.describeResultNavigation=(...args)=>{
    reentry=f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options);return original(...args);
  };
  const packet=f.editor.takeAcknowledgedNavigation(p.token,f.event,f.options);
  assert.equal(reentry,null);assert.equal(packet.searchRecording,null);
  assert.equal(packet.navigation.plan.loadRequest.urlLoad.url,'https://example.test/');f.editor.close();
});


test('terminal result engagement records once with owned text and captured exposure',async()=>{
  const f=await navigationWriteFixture();
  f.editor.startEngagement(1,f.q.id,{type:'input',timeStamp:1});
  const event={type:'keydown',key:'Enter',timeStamp:2};
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,event,{searchString:'spoof'}),true);
  const records=f.parent.engagements.filter(e=>e.kind==='record');assert.equal(records.length,1);
  assert.equal(records[0].args[1].searchString,'typed');assert.equal(records[0].args[1].selType,'searchengine');
  assert.equal(records[0].args[2].engagementData.visibleResults[0],f.result);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,event),false);
  assert.throws(()=>f.editor.publishPresentation(1,f.q.id,[f.result],true),/Invalid/);f.editor.close();
});

test('terminal engagement rejects invalid events, hidden identities and search-mode actions without consuming the clock',async()=>{
  const f=await navigationWriteFixture();f.editor.startEngagement(1,f.q.id,{type:'input',timeStamp:1});
  const event={type:'click',button:0,timeStamp:2};
  for(const bad of [{type:'blur',timeStamp:2},{type:'keydown',key:'Escape',timeStamp:2},
    {type:'click',button:2,timeStamp:2},{...event,timeStamp:NaN}]) {
    assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,bad),/Invalid/);
  }
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,{...f.result},event),/Stale/);
  f.result.payload.providesSearchMode=true;
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,event),/Unsupported/);
  delete f.result.payload.providesSearchMode;
  f.editor.publishPresentation(1,f.q.id,[],false);
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,event),/Stale/);
  f.editor.publishPresentation(1,f.q.id,[f.result],true);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,event),true);f.editor.close();
});

test('terminal engagement blocks reentry and preserves a replacement query begun by providers',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'old'});
  const result={id:90,type:f.sandbox.UrlbarShared.RESULT_TYPE.URL,payload:{url:'https://example.test/'}};
  q.results=[result];f.session.publishPresentation(q.id,[result],true);
  f.session.startEngagement(q.id,{type:'input',timeStamp:1});
  const event={type:'keydown',key:'Enter',timeStamp:2};let next;
  f.parent.engagementEvent.record=(e,details)=>{
    details.isSessionOngoing=false;
    assert.equal(f.session.recordResultEngagement(q.id,result,event,{searchString:'old'}),false);
    next=f.session.query({}, {searchString:'new'});
  };
  assert.equal(f.session.recordResultEngagement(q.id,result,event,{searchString:'old'}),true);
  const replacement=await next;assert.equal(f.session.ownsQuery(replacement.id),true);f.session.close();
});

test('terminal engagement revokes the old query even if recording throws',async()=>{
  const f=await navigationWriteFixture();f.editor.startEngagement(1,f.q.id,{type:'input',timeStamp:1});
  f.parent.engagementEvent.record=()=>{throw Error('recorder failure');};
  const event={type:'keydown',key:'Enter',timeStamp:2};
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,event),/recorder failure/);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,event),false);
  assert.throws(()=>f.editor.publishPresentation(1,f.q.id,[f.result],true),/Invalid/);f.editor.close();
});


test('terminal bounce captures pinned identity and exposure before engagement and survives cancellation',async()=>{
  const f=await editorFixture({browserId:42}),q=await f.editor.edit({},edit(1,'typed'));
  const result={id:100,type:f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH,payload:{query:'typed',engine:'e'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  f.editor.startEngagement(1,q.id,{type:'input',timeStamp:1});
  const calls=[],gate=Promise.withResolvers();let captured;
  f.parent.engagementEvent.startTrackingBounceEvent=(...args)=>{calls.push('bounce');captured=args;return gate.promise;};
  f.parent.engagementEvent.record=()=>calls.push('record');
  f.context.browserId=99;
  const event={type:'keydown',key:'Enter',timeStamp:2};
  assert.equal(f.editor.recordResultEngagement(1,q.id,result,event,{trackBounce:true,browserId:999}),true);
  assert.deepEqual(calls,['bounce','record']);assert.equal(captured[0],42);
  assert.equal(captured[2].searchString,'typed');assert.equal(captured[3].engagementData.visibleResults[0],result);
  assert.equal(f.editor.recordResultEngagement(1,q.id,result,event,{trackBounce:true}),false);
  assert.throws(()=>f.editor.publishPresentation(1,q.id,[result],true),/Invalid/);
  gate.resolve();await gate.promise;assert.equal(captured[3].engagementData.visibleResults.length,1);f.editor.close();
});

test('bounce admission rejects missing identities and invalid flags without consuming engagement',async()=>{
  for(const browserId of [0,-1,1.5,NaN,'42'])assert.throws(()=>fixture({browserId}),/Invalid pinned/);
  const f=await navigationWriteFixture();f.editor.startEngagement(1,f.q.id,{type:'input',timeStamp:1});
  const event={type:'keydown',key:'Enter',timeStamp:2};
  for(const trackBounce of [true,1,'yes'])assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,event,{trackBounce}),/pinned browser/);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,event),true);f.editor.close();
});

test('bounce setup failures do not prevent terminal recording or cause unhandled rejection',async()=>{
  for(const asynchronous of [false,true]) {
    const f=await editorFixture({browserId:42}),q=await f.editor.edit({},edit(1,'typed'));
    const result={id:101,type:f.sandbox.UrlbarShared.RESULT_TYPE.URL,payload:{url:'https://example.test/'}};
    q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
    f.editor.startEngagement(1,q.id,{type:'input',timeStamp:1});
    const errors=[];f.sandbox.console={error:(...args)=>errors.push(args)};
    f.parent.engagementEvent.startTrackingBounceEvent=()=>{if(asynchronous)return Promise.reject(Error('bounce'));throw Error('bounce');};
    assert.equal(f.editor.recordResultEngagement(1,q.id,result,{type:'keydown',key:'Enter',timeStamp:2},{trackBounce:true}),true);
    await new Promise(resolve=>setImmediate(resolve));assert.equal(errors.length,1);
    assert.equal(f.parent.engagements.filter(e=>e.kind==='record').length,1);f.editor.close();
  }
});


async function historyEngagementFixture(isPrivate=false) {
  const f=await editorFixture({browserId:42,isPrivate}),q=await f.editor.edit({},edit(1,'typed'));
  const result={id:102,type:f.sandbox.UrlbarShared.RESULT_TYPE.URL,payload:{url:'https://history.test/'}};
  q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  f.editor.startEngagement(1,q.id,{type:'input',timeStamp:1});
  const calls=[];
  f.parent.addToInputHistory=(...args)=>calls.push(['history',...args]);
  f.parent.handleAutofillReintegration=url=>calls.push(['reintegrate',url]);
  f.parent.engagementEvent.startTrackingBounceEvent=()=>{calls.push(['bounce']);return Promise.resolve();};
  f.parent.engagementEvent.record=()=>calls.push(['record']);
  f.prefs['autoFill.adaptiveHistory.enabled']=true;
  const event={type:'keydown',key:'Enter',timeStamp:2};
  const options={selectedTabIsEmpty:false,originalUntrimmedValue:'typed',recordHistory:true,trackBounce:true};
  return {...f,q,result,calls,event,options};
}

test('history and reintegration execute once before bounce and terminal recording with owned typed input',async()=>{
  const f=await historyEngagementFixture();
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,searchString:'spoof'}),true);
  assert.deepEqual(f.calls,[['history','https://history.test/','typed'],['reintegrate','https://history.test/'],['bounce'],['record']]);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),false);assert.equal(f.calls.length,4);f.editor.close();
});

test('history execution preserves deferred origins and private suppression',async()=>{
  const f=await historyEngagementFixture();f.result.heuristic=true;f.result.autofill={type:'origin'};
  f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options);
  assert.equal(f.calls[0][0],'history');assert.equal(f.calls[0][3].whenReady,true);
  assert.deepEqual(f.calls.map(c=>c[0]),['history','bounce','record']);f.editor.close();
  const g=await historyEngagementFixture(true);g.editor.recordResultEngagement(1,g.q.id,g.result,g.event,g.options);
  assert.deepEqual(g.calls.map(c=>c[0]),['bounce','record']);g.editor.close();
});

test('history admission and failure do not permit duplicate or retargeted effects',async()=>{
  const f=await historyEngagementFixture();
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,recordHistory:1}),/Invalid/);
  assert.equal(f.calls.length,0);
  f.parent.addToInputHistory=()=>{throw Error('history failure');};
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),/history failure/);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),false);assert.equal(f.calls.length,0);f.editor.close();
  const g=await historyEngagementFixture();let replacement;
  g.parent.addToInputHistory=()=>{replacement=g.editor.edit({},edit(2,'replacement'));};
  assert.equal(g.editor.recordResultEngagement(1,g.q.id,g.result,g.event,g.options),false);
  const next=await replacement;assert.equal(g.editor.snapshot.text,'replacement');
  g.editor.publishPresentation(2,next.id,[],false);assert.equal(g.calls.length,0);g.editor.close();
});


test('activation autofill cleanup is explicit, once per result and precedes selection hooks',async()=>{
  const f=await historyEngagementFixture();f.result.autofill={type:'origin'};
  f.parent.onBeforeSelection=()=>f.calls.push(['before']);
  f.parent.clearAutofillBackspaceEntryForUrl=url=>{
    f.calls.push(['cleanup',url]);
    assert.equal(f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),false);
  };
  f.editor.beginResultSelection(1,f.q.id,f.result);assert.deepEqual(f.calls,[['before']]);f.calls.length=0;
  assert.equal(f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),true);
  assert.equal(f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),false);
  f.editor.beginResultSelection(1,f.q.id,f.result);
  assert.deepEqual(f.calls,[['cleanup','https://history.test/'],['before']]);f.editor.close();
});

test('activation cleanup preserves private and disabled policy and rejects unexposed identities',async()=>{
  for(const disabled of [false,true]) {
    const f=await historyEngagementFixture(!disabled);f.result.autofill={type:'origin'};
    if(disabled)f.prefs['autoFill.adaptiveHistory.enabled']=false;
    f.parent.clearAutofillBackspaceEntryForUrl=()=>assert.fail('suppressed cleanup ran');
    assert.throws(()=>f.editor.clearResultAutofillBackspace(1,f.q.id,{...f.result}),/Stale/);
    f.editor.publishPresentation(1,f.q.id,[],false);
    assert.throws(()=>f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),/Stale/);
    f.editor.publishPresentation(1,f.q.id,[f.result],true);
    assert.equal(f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),true);f.editor.close();
  }
});

test('activation cleanup failures cannot retry and replacement queries revoke the old operation',async()=>{
  const f=await historyEngagementFixture();f.result.autofill={type:'origin'};
  f.parent.clearAutofillBackspaceEntryForUrl=()=>{throw Error('cleanup failure');};
  assert.throws(()=>f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),/cleanup failure/);
  assert.equal(f.editor.clearResultAutofillBackspace(1,f.q.id,f.result),false);f.editor.close();
  const g=await historyEngagementFixture();g.result.autofill={type:'origin'};let next;
  g.parent.clearAutofillBackspaceEntryForUrl=()=>{next=g.editor.edit({},edit(2,'replacement'));};
  assert.equal(g.editor.clearResultAutofillBackspace(1,g.q.id,g.result),false);
  const query=await next;g.editor.publishPresentation(2,query.id,[],false);g.editor.close();
});


test('parent search recording uses strict pinned identity without selected-browser fallback',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  recordSearch(searchData, browserId = null) {');
  const method=source.slice(start,source.indexOf('\n  /**',start));
  const Parent=new Function(`return class { ${method} #recordSearchForBrowser(data){this.calls.push(data);} }`)();
  const p=new Parent(),selected={browserId:1},pinned={browserId:2};
  p.calls=[];p.browserWindow={gBrowser:{selectedBrowser:selected}};p.resolveTargetBrowser=()=>pinned;
  assert.equal(p.recordSearch({query:'q'},2),true);assert.equal(p.calls[0].browser,pinned);
  p.resolveTargetBrowser=()=>null;assert.equal(p.recordSearch({},2),false);
  p.resolveTargetBrowser=()=>selected;assert.equal(p.recordSearch({},2),false);assert.equal(p.calls.length,1);
  assert.throws(()=>p.recordSearch({},0),/Invalid/);
  assert.equal(p.recordSearch({}),true);assert.equal(p.calls[1].browser,selected);
});

test('independent current-tab search recording precedes bounce/engagement and uses a mutable payload copy',async()=>{
  const f=await historyEngagementFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH;
  f.result.payload={engine:'Engine',query:'chosen'};f.parent.child.updateEngineStore('changed',{id:'e',name:'Engine'},-1);
  f.parent.recordSearch=(data,id)=>{f.calls.push(['search',id,data.query]);data.details.test=true;return true;};
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,recordSearch:true}),true);
  assert.deepEqual(f.calls,[['search',42,'chosen'],['bounce'],['record']]);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,recordSearch:true}),false);f.editor.close();
});

test('search execution rejects non-current targets before consumption and stops if the pinned target vanishes',async()=>{
  const f=await historyEngagementFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH;
  f.result.payload={engine:'Engine',query:'chosen'};f.parent.child.updateEngineStore('changed',{id:'e',name:'Engine'},-1);
  const options={...f.options,recordSearch:true};
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,{...f.event,where:'tab'},options),false);
  assert.equal(f.calls.length,0);
  f.parent.recordSearch=()=>false;
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,options),false);
  f.parent.recordSearch=()=>assert.fail('consumed search retried');
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,options),false);assert.equal(f.calls.length,0);f.editor.close();
});


test('parent DNS fixup preserves legacy fallback and strictly pins independent targets',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  checkKeywordURIFixup(searchString, browserId, strictTarget = false) {');
  const method=source.slice(start,source.indexOf('\n  /**',start));
  const calls=[],info={host:'intranet'},selected={browserId:1},pinned={browserId:2};
  const lazy={UrlbarUtils:{getURIFixupInfo:(text,priv)=>{calls.push([text,priv]);return info;}}};
  const Parent=new Function('lazy',`return class { ${method} }`)(lazy),p=new Parent();
  p.isPrivate=true;p.browserWindow={gBrowser:{selectedBrowser:selected},gKeywordURIFixup:{check:(b,i)=>calls.push([b,i])}};
  p.resolveTargetBrowser=()=>pinned;
  assert.equal(p.checkKeywordURIFixup('intranet',2,true),true);
  assert.deepEqual(calls,[['intranet',true],[pinned,info]]);calls.length=0;
  for(const target of [null,selected]) {p.resolveTargetBrowser=()=>target;assert.equal(p.checkKeywordURIFixup('intranet',2,true),false);}
  assert.throws(()=>p.checkKeywordURIFixup('intranet',0,true),/Invalid/);assert.equal(calls.length,0);
  p.resolveTargetBrowser=()=>null;assert.equal(p.checkKeywordURIFixup('intranet',2),true);assert.equal(calls[1][0],selected);
  lazy.UrlbarUtils.getURIFixupInfo=()=>null;calls.length=0;p.resolveTargetBrowser=()=>pinned;
  assert.equal(p.checkKeywordURIFixup('intranet',2,true),true);assert.equal(calls.length,0);
});

async function dnsEngagementFixture() {
  const f=await historyEngagementFixture();f.result.type=f.sandbox.UrlbarShared.RESULT_TYPE.SEARCH;
  f.result.heuristic=true;f.result.payload={engine:'Engine',query:'typed'};
  f.parent.child.updateEngineStore('changed',{id:'e',name:'Engine'},-1);
  f.prefs['browser.fixup.dns_first_for_single_words']=false;f.prefs.dnsResolveSingleWordsAfterSearch=1;
  f.parent.recordSearch=()=>{f.calls.push(['search']);return true;};
  f.options={...f.options,recordSearch:true,checkSearchDNS:true};return f;
}

test('independent DNS runs once before search and terminal effects using the pinned target',async()=>{
  const f=await dnsEngagementFixture();
  f.parent.checkKeywordURIFixup=(host,id,strict)=>{f.calls.push(['dns',host,id,strict]);return true;};
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),true);
  assert.deepEqual(f.calls,[['dns','typed',42,true],['search'],['bounce'],['record']]);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),false);f.editor.close();
});

test('DNS suppression preserves search recording and invalid flag combinations do not consume',async()=>{
  const f=await dnsEngagementFixture();f.parent.checkKeywordURIFixup=()=>assert.fail('suppressed DNS');
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,recordSearch:false}),/requires search/);
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,checkSearchDNS:1}),/requires search/);
  f.prefs.dnsResolveSingleWordsAfterSearch=0;
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),true);
  assert.deepEqual(f.calls,[['search'],['bounce'],['record']]);f.editor.close();
});

test('DNS refusal, exceptions and query replacement consume activation and stop later effects',async()=>{
  for(const outcome of ['refuse','throw','replace']) {
    const f=await dnsEngagementFixture();let next;
    f.parent.checkKeywordURIFixup=()=>{
      if(outcome==='throw')throw Error('DNS failure');
      if(outcome==='replace')next=f.editor.edit({},edit(2,'replacement'));
      return outcome!=='refuse';
    };
    const run=()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options);
    if(outcome==='throw')assert.throws(run,/DNS failure/);else assert.equal(run(),false);
    assert.equal(f.calls.length,0);
    if(next) {const q=await next;f.editor.publishPresentation(2,q.id,[],false);}
    else assert.equal(run(),false);
    f.editor.close();
  }
});


test('selected-search browser facts strictly capture the pinned tab instead of the selected tab',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  getSelectedSearchBrowserState(browserId) {');
  const method=source.slice(start,source.indexOf('\n  /**',start));
  const Parent=new Function(`return class { ${method} }`)(),p=new Parent();
  const browser={browserId:42,getAttribute:()=> '7',currentURI:{spec:'https://pinned.test/'}};
  p.resolveTargetBrowser=()=>browser;p.browserWindow={gBrowser:{getTabForBrowser:b=>b===browser?{group:{id:'group'}}:null}};
  const state=p.getSelectedSearchBrowserState(42);
  assert.deepEqual(state,{userContextId:7,tabGroup:'group',currentPage:'https://pinned.test/'});assert.equal(Object.isFrozen(state),true);
  assert.throws(()=>p.getSelectedSearchBrowserState(0),/Invalid/);
  assert.equal(p.getSelectedSearchBrowserState(43),null);p.resolveTargetBrowser=()=>null;assert.equal(p.getSelectedSearchBrowserState(42),null);
});

async function backendEngagementFixture() {
  const f=await dnsEngagementFixture(),engine={id:'e',name:'Engine',isAppProvided:true};
  f.parent.child.updateEngineStore('changed',engine,-1);f.parent.child.updateEngineStore('default',engine);
  f.result.payload={engine:'Engine',query:'typed',suggestion:'  chosen words  '};
  f.parent.getSelectedSearchBrowserState=id=>{assert.equal(id,42);return {userContextId:7,tabGroup:'g',currentPage:'https://pinned.test/'};};
  f.sandbox.backend={isEnabled:true,query:(text,{queryContext})=>{f.calls.push(['backend',text]);f.backendContext=queryContext;return Promise.resolve([]);}};
  f.options={...f.options,checkSearchDNS:false,querySearchBackend:true};return f;
}

test('selected backend receives a fresh trimmed-selection context between search and terminal effects',async()=>{
  const f=await backendEngagementFixture();
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),true);
  assert.deepEqual(f.calls,[['search'],['backend','chosen words'],['bounce'],['record']]);
  assert.notEqual(f.backendContext,f.q);assert.equal(f.backendContext.searchString,'chosen words');
  assert.equal(f.backendContext.tokens[0].value,'chosen words');assert.equal(f.backendContext.userContextId,7);
  assert.equal(f.backendContext.currentPage,'https://pinned.test/');assert.equal(f.backendContext.tabGroup,'g');
  assert.equal(f.backendContext.isPrivate,false);assert.equal(f.backendContext.sapName,'urlbar');assert.equal(f.backendContext.prohibitRemoteResults,false);
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),false);f.editor.close();
});

test('backend disablement, origin, empty, history and default-engine changes suppress execution',async()=>{
  for(const policy of ['disabled','origin','empty','history','default']) {
    const f=await backendEngagementFixture();
    if(policy==='disabled')f.sandbox.backend.isEnabled=false;
    if(policy==='origin')f.result.payload.suggestion='private.test';
    if(policy==='empty')f.result.payload.suggestion='   ';
    if(policy==='history')f.result.source=f.sandbox.UrlbarShared.RESULT_SOURCE.HISTORY;
    if(policy==='default')f.parent.recordSearch=()=>{f.calls.push(['search']);f.parent.child.updateEngineStore('changed',{id:'other',name:'Other'},-1);f.parent.child.updateEngineStore('default',{id:'other',name:'Other'});return true;};
    assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),true);
    assert.deepEqual(f.calls,[['search'],['bounce'],['record']]);f.editor.close();
  }
});

test('backend flag validation, vanished targets and reentry cannot dispatch obsolete selections',async()=>{
  const f=await backendEngagementFixture();
  assert.throws(()=>f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,{...f.options,recordSearch:false}),/requires search/);
  f.parent.getSelectedSearchBrowserState=()=>null;
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),false);assert.deepEqual(f.calls,[['search']]);f.editor.close();
  const g=await backendEngagementFixture();let next;
  g.parent.getSelectedSearchBrowserState=()=>{next=g.editor.edit({},edit(2,'new'));return {};};
  assert.equal(g.editor.recordResultEngagement(1,g.q.id,g.result,g.event,g.options),false);assert.deepEqual(g.calls,[['search']]);
  const q=await next;g.editor.publishPresentation(2,q.id,[],false);g.editor.close();
});

test('shared selected-search helper rechecks ownership after context construction',()=>{
  const f=fixture(),engine={isAppProvided:true},result={payload:{query:'chosen words'}};let current=true;
  f.sandbox.backend={isEnabled:true,query:()=>assert.fail('stale backend dispatch')};
  assert.equal(f.sandbox.UrlbarSelectedSearch(result,{isAddressbar:true,isPrivate:false,engine,defaultEngine:engine},
    ()=>{current=false;return {searchString:'chosen words'};},()=>current),false);f.session.close();
});

test('backend asynchronous rejection is handled without suppressing terminal recording',async()=>{
  const f=await backendEngagementFixture(),errors=[];f.sandbox.console={error:error=>errors.push(error.message)};
  f.sandbox.backend.query=()=>Promise.reject(Error('backend failure'));
  assert.equal(f.editor.recordResultEngagement(1,f.q.id,f.result,f.event,f.options),true);
  await Promise.resolve();assert.deepEqual(errors,['backend failure']);assert.deepEqual(f.calls,[['search'],['bounce'],['record']]);f.editor.close();
});


function parentLoadFixture() {
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  prepareCurrentLoad(loadData) {');
  const methods=source.slice(start,source.indexOf('  /**\n   * Focuses the browser',start));
  const calls=[],browser={browserId:42,lastLocationChange:1,browsingContext:{currentWindowGlobal:{}},focus:()=>calls.push('focus')};
  const lazy={UrlbarUtils:{loadRequestToUrl:request=>({url:request.urlLoad?.url,postData:null})}};
  const Parent=new Function('lazy','Cr',`return class {
    #currentLoad=null;#currentLoadGeneration=0;#destroyed=false;#isAddressbar=true;
    #prepareAddressbarLoad(data){this.prepare?.(data);}
    destroy(){this.#destroyed=true;this.#currentLoad=null;}
    ${methods}
  }`)(lazy,{NS_ERROR_LOAD_SHOWED_ERRORPAGE:99});
  const p=new Parent();p.resolveTargetBrowser=id=>id===42?browser:null;
  p.browserWindow={gBrowser:{selectedBrowser:browser},openTrustedLinkIn:(url,where,params)=>calls.push({url,where,params})};
  const data={where:'current',browserId:42,loadRequest:{urlLoad:{url:'https://chosen.test/'}},params:{avoidBrowserFocus:false},userTypedValue:'chosen'};
  return {p,browser,data,calls,lazy};
}

test('parent current loads copy preparation, supersede tokens and dispatch exactly once',()=>{
  const {p,browser,data,calls}=parentLoadFixture();
  const old=p.prepareCurrentLoad(data),token=p.prepareCurrentLoad(data);
  assert.equal(Object.isFrozen(token),true);assert.equal(p.commitCurrentLoad(old).attempted,false);
  data.loadRequest.urlLoad.url='https://mutated.test/';data.params.avoidBrowserFocus=true;
  assert.deepEqual(p.commitCurrentLoad(token),{attempted:true,reverted:false,browserId:42});
  assert.equal(calls[0],'focus');assert.equal(calls[1].url,'https://chosen.test/');assert.equal(calls[1].params.targetBrowser,browser);
  assert.equal(p.commitCurrentLoad(token).attempted,false);assert.equal(calls.length,2);assert.equal(data.params.targetBrowser,undefined);
});

test('current-load lifetime rejects tab, document, epoch, identity and destroyed changes',()=>{
  for(const change of ['tab','document','epoch','identity','gone','destroy','scope']) {
    const {p,browser,data,calls}=parentLoadFixture(),token=p.prepareCurrentLoad(data);
    if(change==='tab')p.browserWindow.gBrowser.selectedBrowser={};
    if(change==='document')browser.browsingContext.currentWindowGlobal={};
    if(change==='epoch')browser.lastLocationChange++;
    if(change==='identity')browser.browserId=43;
    if(change==='gone')p.resolveTargetBrowser=()=>null;
    if(change==='destroy')p.destroy();
    assert.equal(p.commitCurrentLoad(token,()=>change!=='scope').attempted,false);
    assert.equal(calls.length,0);assert.equal(p.commitCurrentLoad(token).attempted,false);
  }
});

test('current-load guards cover URL resolution, address preparation and focus reentry',()=>{
  for(const phase of ['resolve','prepare','focus','supersede']) {
    const {p,browser,data,calls,lazy}=parentLoadFixture(),token=p.prepareCurrentLoad(data);let next;
    if(phase==='resolve')lazy.UrlbarUtils.loadRequestToUrl=()=>{browser.lastLocationChange++;return {url:'https://chosen.test/'};};
    if(phase==='prepare')p.prepare=()=>{browser.lastLocationChange++;};
    if(phase==='focus')browser.focus=()=>{browser.lastLocationChange++;};
    if(phase==='supersede')browser.focus=()=>{next=p.prepareCurrentLoad(data);};
    assert.equal(p.commitCurrentLoad(token).attempted,false);assert.equal(calls.length,0);
    if(next){browser.focus=()=>{};assert.equal(p.commitCurrentLoad(next).attempted,true);}
  }
});

test('current-load consumption survives thrown callbacks and validation failures',()=>{
  const {p,data,calls}=parentLoadFixture();let token=p.prepareCurrentLoad(data);
  assert.throws(()=>p.commitCurrentLoad(token,()=>{throw Error('scope failure');}),/scope failure/);
  assert.equal(p.commitCurrentLoad(token).attempted,false);
  token=p.prepareCurrentLoad(data);assert.throws(()=>p.commitCurrentLoad(token,null),/Invalid/);
  assert.equal(p.commitCurrentLoad(token).attempted,false);
  token=p.prepareCurrentLoad(data);assert.throws(()=>p.prepareCurrentLoad({...data,where:'tab'}),/Invalid/);
  assert.equal(p.commitCurrentLoad(token).attempted,false);assert.equal(calls.length,0);
});

test('guarded load outcomes distinguish unresolved URLs and attempted errors without changing legacy results',()=>{
  for(const error of [null,1,99]) {
    const {p,data}=parentLoadFixture();
    if(error)p.browserWindow.openTrustedLinkIn=()=>{throw {result:error};};
    const result=p.commitCurrentLoad(p.prepareCurrentLoad(data));
    assert.equal(result.attempted,true);assert.equal(result.reverted,error===1);
    const legacy=p.loadURL(data);assert.equal('attempted' in legacy,false);assert.equal(legacy.reverted,error===1);
  }
  const {p,data,lazy}=parentLoadFixture();lazy.UrlbarUtils.loadRequestToUrl=()=>({url:null});
  assert.deepEqual(p.commitCurrentLoad(p.prepareCurrentLoad(data)),{attempted:false,reverted:true,browserId:42});
});
