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
  const prefs={"keepPanelOpenDuringImeComposition":false,"experimental.hideHeuristic":false,openintab:false,ctrlCanonizesURLs:false,"browser.search.openintab":false};
  class Parent {
    constructor(identity){this.identity=identity;this.started=[];this.engagements=[];
      this.engagementEvent={start:(...args)=>this.engagements.push({kind:'start',args}),
        record:(...args)=>this.engagements.push({kind:'record',args}),discard:()=>this.engagements.push({kind:'discard'})};parents.push(this);}
    setChild(child){this.child=child;}
    initEngineStore(){if(!hold)this.child.updateEngineStore('init',[],null);}
    startQuery(query){query.results??=[];this.started.push(query);return Promise.resolve(query);}
    cancelQuery(){this.cancelled=(this.cancelled??0)+1;}
    recordZeroPrefix(kind){(this.zeroPrefix??=[]).push(kind);}
    captureIndependentQueryAuthority(){return ()=>true;}
    commitIndependentModePreservation(_id,mode,persisted,current){return current()?{applied:true,modeUnchanged:true,stateToken:{},searchMode:mode,persistedSearchTerms:persisted}:null;}
    destroy(){this.destroyed=(this.destroyed??0)+1;}
  }
  let id=0;
  const sandbox=vm.createContext({console,URL,UrlbarQueryLifecycle,UrlbarParentControllerProxy:class {},
    KeyboardEvent:class {constructor(facts={}){Object.assign(this,{keyCode:13},facts);}getModifierState(key){return key==='AltGraph' && !!this.altGraph;}},KeyEvent:{DOM_VK_RETURN:13},
    UrlbarShared:{COMPOSITION:sharedPolicy.UrlbarShared.COMPOSITION,stripPrefixAndTrim:sharedPolicy.UrlbarShared.stripPrefixAndTrim,getLogger:()=>({debug(){}}),isPasteEvent:e=>e.inputType?.startsWith('insertFromPaste') || e.inputType==='insertFromYank',RESULT_TYPE:sharedPolicy.UrlbarShared.RESULT_TYPE,RESULT_SOURCE:sharedPolicy.UrlbarShared.RESULT_SOURCE,canAutofillURL:sharedPolicy.UrlbarShared.canAutofillURL.bind(sharedPolicy.UrlbarShared),NOTIFICATIONS:{
      RESULTS_CLOSE_REQUESTED:'onResultsCloseRequested',QUERY_RESULT_REMOVED:'onQueryResultRemoved',
      QUERY_ARMED:'onQueryArmed',QUERY_STARTED:'onQueryStarted',QUERY_CANCELLED:'onQueryCancelled',
      QUERY_FIRST_RESULT:'onFirstResult',QUERY_RESULTS:'onQueryResults',QUERY_FINISHED:'onQueryFinished'}},
    UrlbarPrefs:{get:key=>prefs[key] ?? 10,getScotchBonnetPref:key=>!!(prefs["scotchBonnet.enableOverride"] || prefs[key])},UrlbarContentUtils:{usesMessagePath:()=>false,getDisplaySpec:spec=>spec,getFixupPrimitives:()=>({}),getPlatform:()=> 'win',whereToOpenLink:e=>e?.where || 'current',willLoadInBackground:(_where,params)=>!params.forceForeground},
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
  for(const file of ['SearchEngineStore','UrlbarChildController','UrlbarQueryModel','UrlbarSelectedSearch','UrlbarSearchModeModel','UrlbarQuerySession','UrlbarAutofillModel','UrlbarResultPresentation','UrlbarInputIdentity','UrlbarCompositionModel','UrlbarEditSession']) {
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
    onUpdate:update=>updates.push(update),onModeStateChanged:options?.onModeStateChanged ?? null,onPresentationRequest:request=>{
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

async function completionFixture(options) {
  const f=await editorFixture(options);
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


async function modeWriteFixture(options={},fixtureOptions={}) {
  const f=await editorFixture(fixtureOptions),query=await f.editor.edit({},edit(1,'@engine'));
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
const observedMode=p=>({...observed(p),searchMode:{...p.searchMode},modeEntry:p.modeEntry,pageProxyValid:p.pageProxyValid});

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


async function historyEngagementFixture(isPrivate=false, fixtureOptions={}) {
  const f=await editorFixture({...fixtureOptions,browserId:42,isPrivate}),q=await f.editor.edit({},edit(1,'typed'));
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
  let stateToken=null,lastCommit=null,latest=null;
  const owner={prepareRecovery:()=>({token:stateToken=latest={}}),isCurrent:(_b,t)=>t===stateToken,
    isCommitted:(_b,t)=>t===lastCommit && t===latest,
    commit:(_b,t)=>{if(t!==stateToken)return false;lastCommit=t;stateToken=null;return true;},
    discard:(_b,t)=>{if(t!==stateToken)return false;stateToken=null;return true;}};
  const lazy={independentUrlbarBrowserState:owner,UrlbarUtils:{isPersistedSearchTermsEnabled:()=>true,
    loadRequestToUrl:request=>({url:request.urlLoad?.url,postData:null})}};
  const modelFixture=fixture();lazy.UrlbarSearchModeModel=modelFixture.sandbox.UrlbarSearchModeModel;
  lazy.UrlbarCompositionModel=modelFixture.sandbox.UrlbarCompositionModel;lazy.UrlbarShared=modelFixture.sandbox.UrlbarShared;modelFixture.session.close();
  lazy.ReaderMode={getOriginalUrlObjectForDisplay:()=>null};
  const Parent=new Function('lazy','Cr',`return class {
    #currentLoad=null;#currentLoadRecovery=null;#currentLoadGeneration=0;#destroyed=false;#isAddressbar=true;#oneOffSearchTabs=new WeakMap();#actor={};
    #prepareAddressbarLoad(data){this.prepare?.(data);}
    destroy(){this.#destroyed=true;this.#currentLoad=null;this.#discardCurrentLoadRecovery();}
    describeCurrentPageRevert(){return this.recoveryDescription??null;}
    ${methods}
  }`)(lazy,{NS_ERROR_LOAD_SHOWED_ERRORPAGE:99});
  const p=new Parent();p.resolveTargetBrowser=id=>id===42?browser:null;
  p.recordSearchMode=mode=>calls.push(['mode-entry',mode]);p.markEngineAsUsed=id=>calls.push(['engine-used',id]);
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


function mockActivationLoad(f) {
  let pending=null;
  f.parent.prepareCurrentLoad=data=>{f.calls.push(['prepare',data.userTypedValue]);f.loadData=data;return pending={};};
  f.parent.discardCurrentLoad=token=>{if(token===pending){pending=null;f.calls.push(['discard']);return true;}return false;};
  f.parent.notifyStartNavigation=result=>{assert.equal(result,f.result);f.calls.push(['notify']);};
  f.parent.commitCurrentLoad=(token,current)=>{
    assert.equal(token,pending);pending=null;
    if(!current())return {attempted:false,reverted:false,browserId:42};
    f.calls.push(['load']);return {attempted:true,reverted:false,browserId:42};
  };
  const proposal=f.editor.beginNavigationWrite(1,f.q.id,f.result,f.event,f.options);
  f.editor.acknowledgeNavigationWrite(proposal.token,navigationObservation(proposal),f.event,f.options);
  return proposal;
}

test('acknowledged URL activation orders all effects, notification and load exactly once',async()=>{
  const f=await historyEngagementFixture(),p=mockActivationLoad(f);
  const outcome=f.editor.commitAcknowledgedNavigation(p.token,f.event,{...f.options,recordHistory:false,trackBounce:false});
  assert.equal(outcome.attempted,true);
  assert.deepEqual(f.calls.map(x=>x[0]),['prepare','history','reintegrate','bounce','record','notify','load']);
  assert.equal(f.loadData.browserId,42);assert.equal(f.loadData.userTypedValue,p.value);
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
});

test('acknowledged search activation includes DNS, search and backend before terminal/load',async()=>{
  const f=await backendEngagementFixture();f.parent.checkKeywordURIFixup=()=>{f.calls.push(['dns']);return true;};
  const p=mockActivationLoad(f);
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options).attempted,true);
  assert.deepEqual(f.calls.map(x=>x[0]),['prepare','dns','search','backend','bounce','record','notify','load']);f.editor.close();
});

test('navigation observers replacing the edit stop dispatch and preserve the replacement query',async()=>{
  const f=await historyEngagementFixture(),p=mockActivationLoad(f);let next;
  f.parent.notifyStartNavigation=()=>{f.calls.push(['notify']);next=f.editor.edit({},edit(2,'new edit'));};
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);
  assert.equal(f.calls.some(x=>x[0]==='load'),false);assert.equal(f.calls.at(-1)[0],'discard');
  const q=await next;f.editor.publishPresentation(2,q.id,[],false);f.editor.close();
});

test('activation revalidates acknowledged result semantics at the final parent guard',async()=>{
  const f=await historyEngagementFixture(),p=mockActivationLoad(f);
  f.parent.commitCurrentLoad=(_token,current)=>{f.result.payload.url='https://changed.test/';return {attempted:current(),reverted:false,browserId:42};};
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options).attempted,false);
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
});

test('unacknowledged or inactive navigation cannot prepare a load',async()=>{
  const f=await historyEngagementFixture();f.parent.prepareCurrentLoad=()=>assert.fail('unready load');
  const p=f.editor.beginNavigationWrite(1,f.q.id,f.result,f.event,f.options);
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);
  f.editor.acknowledgeNavigationWrite(p.token,navigationObservation(p),f.event,f.options);
  f.editor.discardEngagement();
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
});

test('throwing activation effects consume editor readiness and discard parent authorization',async()=>{
  const f=await historyEngagementFixture(),p=mockActivationLoad(f);
  f.parent.engagementEvent.record=()=>{throw Error('record failure');};
  assert.throws(()=>f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),/record failure/);
  assert.equal(f.calls.at(-1)[0],'discard');assert.equal(f.calls.some(x=>x[0]==='load'),false);
  assert.equal(f.editor.commitAcknowledgedNavigation(p.token,f.event,f.options),null);f.editor.close();
});


async function activationSelectionFixture() {
  const f=await historyEngagementFixture();f.result.autofill={type:'origin'};
  f.parent.clearAutofillBackspaceEntryForUrl=()=>f.calls.push(['cleanup']);
  f.parent.onBeforeSelection=()=>f.calls.push(['before']);
  f.parent.onSelection=()=>f.calls.push(['selected']);
  f.beginActivation=()=>f.editor.beginNavigationActivation(1,f.q.id,f.result,f.event,f.options);
  return f;
}

test('staged activation cleans up before provider selection and requires exact observed row',async()=>{
  const f=await activationSelectionFixture(),p=f.beginActivation();
  assert.deepEqual(f.calls,[['cleanup'],['before']]);assert.equal(p.resultId,f.result.id);
  assert.equal(f.editor.getNavigationWrite(p.token,f.event,f.options),null);
  for(const observation of [{queryId:-1,resultId:p.resultId},{queryId:p.queryId,resultId:-1}]) {
    assert.equal(f.editor.acknowledgeNavigationActivation(p.token,observation,f.event,f.options),null);
  }
  const write=f.editor.acknowledgeNavigationActivation(p.token,{queryId:p.queryId,resultId:p.resultId},f.event,f.options);
  assert.equal(write.value,'https://history.test/');assert.deepEqual(f.calls,[['cleanup'],['before'],['selected']]);
  assert.equal(f.editor.getAcknowledgedNavigation(write.token,f.event,f.options),null);
  assert.equal(f.editor.acknowledgeNavigationActivation(p.token,{queryId:p.queryId,resultId:p.resultId},f.event,f.options),null);
  assert.equal(f.editor.acknowledgeNavigationWrite(write.token,navigationObservation(write),f.event,f.options),true);
  assert.ok(f.editor.getAcknowledgedNavigation(write.token,f.event,f.options));f.editor.close();
});

test('activation rejects unsupported destinations and non-activation events before cleanup',async()=>{
  const f=await activationSelectionFixture();
  assert.throws(()=>f.editor.beginNavigationActivation(1,f.q.id,f.result,{type:'mousemove',timeStamp:2},f.options),/Invalid/);
  assert.equal(f.editor.beginNavigationActivation(1,f.q.id,f.result,{...f.event,where:'tab'},f.options),null);
  assert.equal(f.calls.length,0);assert.ok(f.beginActivation());f.editor.close();
});

test('cleanup reentry or failures cannot start or retry a stale selection',async()=>{
  for(const change of ['throw','selection','edit']) {
    const f=await activationSelectionFixture();let next;
    f.parent.clearAutofillBackspaceEntryForUrl=()=>{
      if(change==='throw')throw Error('cleanup failure');
      if(change==='selection')f.editor.beginResultSelection(1,f.q.id,f.result);
      if(change==='edit')next=f.editor.edit({},edit(2,'new'));
    };
    if(change==='throw')assert.throws(f.beginActivation,/cleanup failure/);else assert.equal(f.beginActivation(),null);
    if(next) {const q=await next;f.editor.publishPresentation(2,q.id,[],false);}
    else assert.equal(f.beginActivation(),null);
    f.editor.close();
  }
});

test('activation acknowledgment rejects changed row identity, exposure and opening facts permanently',async()=>{
  for(const change of ['id','hide','target']) {
    const f=await activationSelectionFixture(),p=f.beginActivation();
    if(change==='id')f.result.id++;
    if(change==='hide')f.editor.publishPresentation(1,f.q.id,[],false);
    const event=change==='target'?{...f.event,where:'tab'}:f.event;
    assert.equal(f.editor.getNavigationActivation(p.token,event,f.options),null);
    if(change==='id')f.result.id--;
    if(change==='hide')f.editor.publishPresentation(1,f.q.id,[f.result],true);
    assert.equal(f.editor.getNavigationActivation(p.token,f.event,f.options),null);
    assert.equal(f.calls.some(x=>x[0]==='selected'),false);f.editor.close();
  }
});

test('selection callbacks cannot restore old activation after reselection or hide/reopen',async()=>{
  for(const change of ['selection','exposure','throw']) {
    const f=await activationSelectionFixture(),p=f.beginActivation();
    f.parent.onSelection=()=>{
      if(change==='throw')throw Error('selection failure');
      if(change==='selection'){const newer=f.editor.beginResultSelection(1,f.q.id,f.result);f.parent.onSelection=()=>{};f.editor.acknowledgeResultSelection(newer.token);}
      if(change==='exposure'){f.editor.publishPresentation(1,f.q.id,[],false);f.editor.publishPresentation(1,f.q.id,[f.result],true);}
    };
    const acknowledge=()=>f.editor.acknowledgeNavigationActivation(p.token,{queryId:p.queryId,resultId:p.resultId},f.event,f.options);
    if(change==='throw')assert.throws(acknowledge,/selection failure/);else assert.equal(acknowledge(),null);
    assert.equal(acknowledge(),null);assert.equal(f.editor.getNavigationWrite(p.token,f.event,f.options),null);f.editor.close();
  }
});


test('unchanged exposure preserves staged activation while removal/republication during selection revokes it',async()=>{
  const f=await activationSelectionFixture(),p=f.beginActivation();
  f.editor.publishPresentation(1,f.q.id,[f.result],true);
  assert.equal(f.editor.getNavigationActivation(p.token,f.event,f.options),p);
  f.parent.onSelection=()=>f.editor.publishPresentation(1,f.q.id,[f.result],true);
  assert.ok(f.editor.acknowledgeNavigationActivation(p.token,{queryId:p.queryId,resultId:p.resultId},f.event,f.options));f.editor.close();
  const g=await activationSelectionFixture(),other={id:123};g.q.results.push(other);const a=g.beginActivation();
  g.parent.onSelection=()=>{g.editor.publishPresentation(1,g.q.id,[other],true);g.editor.publishPresentation(1,g.q.id,[g.result,other],true);};
  assert.equal(g.editor.acknowledgeNavigationActivation(a.token,{queryId:a.queryId,resultId:a.resultId},g.event,g.options),null);g.editor.close();
});


test('loaded-URI value preserves blank/origin/extension/session-restore validity and decode fallback',()=>{
  const f=fixture();
  for(const hideInitialPage of [false,true])for(const isBlankPage of [false,true])
    for(const isExtensionUrl of [false,true])for(const dueToSessionRestore of [false,true]) {
      let reads=0;
      const result=f.sandbox.loadedURIValue(()=>{reads++;return 'https://loaded.test/path';},
        {hideInitialPage,isBlankPage,isExtensionUrl,dueToSessionRestore});
      assert.equal(result.value,hideInitialPage?'':'https://loaded.test/path');
      assert.equal(result.valid,!dueToSessionRestore && (!isBlankPage || isExtensionUrl || !hideInitialPage));
      assert.equal(reads,hideInitialPage?0:1);
    }
  const result=f.sandbox.loadedURIValue(()=>{throw Error('exotic URI');},{hideInitialPage:false,isBlankPage:false});
  assert.equal(result.value,'about:blank');assert.equal(result.valid,true);f.session.close();
});

function recoveryValueFixture(methodName='describeCurrentPageRevert') {
  const f=fixture(),source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  '+methodName+'(');
  const method=source.slice(start,source.indexOf('\n  /**',start));
  const uri=spec=>({spec,displaySpec:spec});
  const browser={browserId:42,lastLocationChange:1,userTypedValue:'untouched',currentURI:uri('https://loaded.test/'),
    browsingContext:{currentWindowGlobal:{},sessionHistory:{count:1}}};
  const prefs={trimURLs:false,'formatting.enabled':true,'security.insecure_connection_text.enabled':false};
  const lazy={loadedURIValue:f.sandbox.loadedURIValue,trimDisplayValue:f.sandbox.trimDisplayValue,UrlbarSearchModeModel:f.sandbox.UrlbarSearchModeModel,
    ReaderMode:{getOriginalUrlObjectForDisplay:()=>null},UrlbarPrefs:{get:key=>prefs[key]},
    BrowserUIUtils:{checkEmptyPageOrigin:()=>true,trimURL:s=>s.replace(/^https:\/\/(?:www\.)?/, '').replace(/\/$/, ''),
      getTrimmedURLPrefix:s=>s.startsWith('https://www.')?'https://www.':s.startsWith('https://')?'https://':'',trimURLProtocol:'https://'},ExtensionUtils:{isExtensionUrl:u=>u.spec.startsWith('moz-extension:')}};
  const services={io:{createExposableURI:u=>uri(u.spec.replace('user:pass@',''))}};
  const Parent=new Function('lazy','Services','Ci',`return class { #destroyed=false;#isAddressbar=true;
    destroy(){this.#destroyed=true;} ${method} }`)(lazy,services,{nsIWebProgressListener:{STATE_LOADED_MIXED_ACTIVE_CONTENT:4}}),p=new Parent();
  p.resolveTargetBrowser=()=>browser;p.browserWindow={gBrowser:{selectedBrowser:browser,securityUI:{state:0}},browsingContext:{},windowUtils:{getDirectionFromText:()=>0,DIRECTION_RTL:1},
    isInitialPage:u=>u.spec==='about:blank',isBlankPageURL:spec=>spec==='about:blank'};
  return {...f,p,browser,lazy,services,uri,prefs};
}

test('parent revert description preserves auth/loading/current URI precedence and strips credentials without mutation',()=>{
  const f=recoveryValueFixture();
  f.browser.currentAuthPromptURI=f.uri('https://user:pass@auth.test/path');
  f.browser.browsingContext.nonWebControlledLoadingURI=f.uri('https://loading.test/');f.browser.browsingContext.sessionHistory.count=0;
  assert.equal(f.p.describeCurrentPageRevert(42).value,'https://auth.test/path');
  f.browser.currentAuthPromptURI=null;assert.equal(f.p.describeCurrentPageRevert(42).value,'https://loading.test/');
  f.browser.browsingContext.sessionHistory.count=1;const d=f.p.describeCurrentPageRevert(42);
  assert.equal(d.value,'https://loaded.test/');assert.equal(d.pageProxyValid,true);assert.equal(d.searchMode,null);assert.equal(d.userTypedValue,null);
  assert.equal(Object.isFrozen(d),true);assert.equal(f.browser.userTypedValue,'untouched');f.session.close();
});

test('revert description preserves trusted blank versus content-controlled blank behavior',()=>{
  const f=recoveryValueFixture();f.browser.currentURI=f.uri('about:blank');
  let d=f.p.describeCurrentPageRevert(42);assert.equal(d.value,'');assert.equal(d.pageProxyValid,false);
  f.lazy.BrowserUIUtils.checkEmptyPageOrigin=()=>false;d=f.p.describeCurrentPageRevert(42);
  assert.equal(d.value,'about:blank');assert.equal(d.pageProxyValid,true);f.session.close();
});

test('revert description refuses missing/mismatched tabs, changed lifetime and unsupported PiP',()=>{
  for(const change of ['id','tab','document','epoch','destroy','pip']) {
    const f=recoveryValueFixture();
    if(change==='id')f.browser.browserId=43;
    if(change==='tab')f.p.browserWindow.gBrowser.selectedBrowser={};
    if(change==='destroy')f.p.destroy();
    if(change==='pip')f.p.browserWindow.browsingContext.isDocumentPiP=true;
    if(['document','epoch'].includes(change))f.services.io.createExposableURI=u=>{
      if(change==='document')f.browser.browsingContext.currentWindowGlobal={};else f.browser.lastLocationChange++;
      return u;
    };
    assert.equal(f.p.describeCurrentPageRevert(42),null);f.session.close();
  }
  const f=recoveryValueFixture();assert.throws(()=>f.p.describeCurrentPageRevert(0),/Invalid/);
  f.p.resolveTargetBrowser=()=>null;assert.equal(f.p.describeCurrentPageRevert(42),null);f.session.close();
});


test('only reverting guarded loads create recovery, which revalidates and resets typed state once',()=>{
  const {p,data,browser}=parentLoadFixture();browser.userTypedValue='failed';
  p.recoveryDescription={value:'https://loaded.test/',pageProxyValid:true,searchMode:null,userTypedValue:null,persistedSearchTerms:false};
  p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
  const outcome=p.commitCurrentLoad(p.prepareCurrentLoad(data));assert.equal(outcome.reverted,true);assert.ok(outcome.recovery);
  assert.equal(p.getCurrentLoadRecovery(outcome.recovery.token),outcome.recovery);assert.equal(browser.userTypedValue,'failed');
  assert.equal(p.acknowledgeCurrentLoadRecovery(outcome.recovery.token),true);assert.equal(browser.userTypedValue,null);
  assert.equal(p.acknowledgeCurrentLoadRecovery(outcome.recovery.token),false);
  p.browserWindow.openTrustedLinkIn=()=>{throw {result:99};};
  assert.equal(p.commitCurrentLoad(p.prepareCurrentLoad(data)).recovery,undefined);
});

test('recovery tickets expire on document, value and newer-load changes',()=>{
  for(const change of ['document','value','load','typed']) {
    const {p,data,browser}=parentLoadFixture();browser.userTypedValue='failed';
    p.recoveryDescription={value:'loaded',pageProxyValid:true,searchMode:null,userTypedValue:null};
    p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
    const {recovery}=p.commitCurrentLoad(p.prepareCurrentLoad(data));
    if(change==='document')browser.browsingContext.currentWindowGlobal={};
    if(change==='value')p.recoveryDescription={...p.recoveryDescription,value:'changed'};
    if(change==='load')p.prepareCurrentLoad(data);
    if(change==='typed')browser.userTypedValue='new typed value';
    assert.equal(p.getCurrentLoadRecovery(recovery.token),null);assert.equal(p.acknowledgeCurrentLoadRecovery(recovery.token),false);
    assert.equal(browser.userTypedValue,change==='typed'?'new typed value':'failed');
  }
});

async function editorRecoveryFixture(options={}) {
  const f=await historyEngagementFixture(false,options),write=mockActivationLoad(f);
  const recovery={token:{},value:'https://loaded.test/',pageProxyValid:true,searchMode:null,userTypedValue:null,persistedSearchTerms:false};
  let pending=true;
  f.parent.commitCurrentLoad=()=>({attempted:true,reverted:true,browserId:42,recovery});
  f.parent.getCurrentLoadRecovery=token=>pending && token===recovery.token?recovery:null;
  f.parent.acknowledgeCurrentLoadRecovery=(token,applyEditor,editorCurrent,notify)=>{
    if(!pending || token!==recovery.token)return false;pending=false;f.calls.push(['recover']);
    if(applyEditor && !applyEditor(()=>true))return false;if(editorCurrent && !editorCurrent())return false;
    notify?.(Object.freeze({browserId:42,searchMode:null,persistedSearchTerms:false,userTypedValue:null,persistenceChanged:false}));return true;
  };
  const outcome=f.editor.commitAcknowledgedNavigation(write.token,f.event,f.options);
  return {...f,recovery,outcome};
}
const recoveryObservation=p=>({text:p.value,selectionStart:p.selectionStart,selectionEnd:p.selectionEnd,
  selectionDirection:p.selectionDirection,searchMode:null,userTypedValue:null,pageProxyValid:p.pageProxyValid,persistedSearchTerms:p.persistedSearchTerms,focused:p.focused});

test('failed independent commit proposes untrimmed recovery and requires exact observation once',async()=>{
  const f=await editorRecoveryFixture();assert.equal(f.outcome.recovery,f.recovery);
  const before=f.editor.snapshot,p=f.editor.getNavigationRecovery(f.recovery.token,{focused:true});
  assert.equal(p.selectionEnd,p.value.length);assert.equal(f.editor.snapshot,before);
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,{...recoveryObservation(p),text:'wrong'}),false);
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),true);
  assert.equal(f.editor.snapshot.text,p.value);assert.equal(f.editor.snapshot.selectionEnd,p.value.length);
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),false);f.editor.close();
});

test('unfocused recovery resets caret and rejects wrong mode, proxy and typed-state observations',async()=>{
  const f=await editorRecoveryFixture(),p=f.editor.getNavigationRecovery(f.recovery.token,{focused:false});
  assert.equal(p.selectionEnd,0);
  for(const override of [{searchMode:{}},{pageProxyValid:false},{userTypedValue:'old'},{persistedSearchTerms:true},{selectionStart:1}]) {
    assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,{...recoveryObservation(p),...override}),false);
  }
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),true);f.editor.close();
});

test('new edits, cancellation and lost scope cannot recover over current editor state',async()=>{
  for(const change of ['edit','cancel','scope']) {
    const f=await editorRecoveryFixture();
    if(change==='edit')await f.editor.edit({},edit(2,'new'));
    if(change==='cancel')f.editor.cancel();
    if(change==='scope')f.loseScope();
    const get=()=>f.editor.getNavigationRecovery(f.recovery.token,{focused:false});
    if(change==='scope')assert.throws(get,/scope/);else assert.equal(get(),null);
    assert.equal(f.calls.some(x=>x[0]==='recover'),false);f.editor.close();
  }
});


test('recovery revalidates editor state after parent capture and acknowledgment callbacks',async()=>{
  const f=await editorRecoveryFixture();let next;
  f.parent.getCurrentLoadRecovery=()=>{next=f.editor.edit({},edit(2,'replacement'));return f.recovery;};
  assert.equal(f.editor.getNavigationRecovery(f.recovery.token,{focused:false}),null);await next;
  assert.equal(f.editor.snapshot.text,'replacement');f.editor.close();
  const g=await editorRecoveryFixture(),p=g.editor.getNavigationRecovery(g.recovery.token,{focused:false});
  g.parent.acknowledgeCurrentLoadRecovery=()=>{g.editor.cancel();return true;};
  assert.equal(g.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),false);
  assert.notEqual(g.editor.snapshot.text,p.value);g.editor.close();
  const h=await editorRecoveryFixture();
  h.parent.getCurrentLoadRecovery=()=>{h.loseScope();return h.recovery;};
  assert.equal(h.editor.getNavigationRecovery(h.recovery.token,{focused:false}),null);h.editor.close();
});


test('recovery formatting retains full Reader Mode URL and reports actual prefix removal',()=>{
  const f=recoveryValueFixture();f.prefs.trimURLs=true;
  f.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>f.uri('https://www.reader.test/');
  const d=f.p.describeCurrentPageRevert(42);
  assert.equal(d.value,'reader.test');assert.equal(d.untrimmedValue,'https://www.reader.test/');
  assert.equal(d.protocolIsTrimmed,true);assert.equal(d.wwwIsTrimmed,true);
  assert.equal(f.browser.userTypedValue,'untouched');f.session.close();
});

test('recovery formatting uses RTL and Gecko mixed-content facts with Firefox prefs',()=>{
  const f=recoveryValueFixture();f.prefs.trimURLs=true;
  assert.equal(f.p.describeCurrentPageRevert(42).value,'loaded.test');
  f.p.browserWindow.windowUtils.getDirectionFromText=()=>1;
  let d=f.p.describeCurrentPageRevert(42);assert.equal(d.value,'https://loaded.test/');assert.equal(d.protocolIsTrimmed,false);
  f.p.browserWindow.windowUtils.getDirectionFromText=()=>0;
  f.p.browserWindow.gBrowser.securityUI.state=4;
  d=f.p.describeCurrentPageRevert(42);assert.equal(d.value,'https://loaded.test/');assert.equal(d.protocolIsTrimmed,false);
  f.prefs['formatting.enabled']=false;assert.equal(f.p.describeCurrentPageRevert(42).value,'loaded.test');
  f.prefs['formatting.enabled']=true;f.prefs['security.insecure_connection_text.enabled']=true;
  assert.equal(f.p.describeCurrentPageRevert(42).value,'loaded.test');
  f.prefs.trimURLs=false;assert.equal(f.p.describeCurrentPageRevert(42).value,'https://loaded.test/');f.session.close();
});

test('shared trim policy preserves service short circuit and retained input delegates to it',()=>{
  const f=fixture(),calls=[];
  const opts={trimURLs:true,trimURL:v=>{calls.push('trim');return v.slice(8);},
    isTextDirectionRTL:()=>{calls.push('rtl');return true;},preserveMixedContentProtocol:()=>{throw Error('unexpected mixed read');}};
  assert.equal(f.sandbox.trimDisplayValue('https://host/',opts),'https://host/');assert.deepEqual(calls,['trim','rtl']);
  opts.trimURLs=false;calls.length=0;f.sandbox.trimDisplayValue('https://host/',opts);assert.deepEqual(calls,['rtl']);
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarInputBase.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  _trimValue(val) {'),method=source.slice(start,source.indexOf('\n  /**',start));
  const Input=new Function('trimDisplayValue','UrlbarPrefs','lazy','UrlbarContentUtils','window',
    `return class {#isAddressbar=true; #getValueFormatter(){return {willShowFormattedMixedContentProtocol:()=>false};} ${method}}`)
    (f.sandbox.trimDisplayValue,{get:()=>true},{BrowserUIUtils:{trimURL:v=>v.slice(8)}},{isTextDirectionRTL:()=>false},{});
  assert.equal(new Input()._trimValue('https://host/'),'host/');f.session.close();
});

test('recovery ticket revokes on display metadata changes even when visible text is unchanged',()=>{
  for(const key of ['untrimmedValue','protocolIsTrimmed','wwwIsTrimmed']) {
    const {p,data,browser}=parentLoadFixture();browser.userTypedValue='failed';
    p.recoveryDescription={value:'loaded.test',untrimmedValue:'https://loaded.test/',protocolIsTrimmed:true,wwwIsTrimmed:false,pageProxyValid:true,searchMode:null,userTypedValue:null};
    p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
    const outcome=p.commitCurrentLoad(p.prepareCurrentLoad(data));
    p.recoveryDescription={...p.recoveryDescription,[key]:key==='untrimmedValue'?'http://loaded.test/':!p.recoveryDescription[key]};
    assert.equal(p.getCurrentLoadRecovery(outcome.recovery.token),null);
    assert.equal(p.acknowledgeCurrentLoadRecovery(outcome.recovery.token),false);assert.equal(browser.userTypedValue,'failed');
  }
});


test('parent recovery rejects superseded owned state and changed persistence browser facts',()=>{
  for(const change of ['state','preference','originalURI','destroy']) {
    const {p,data,browser,lazy}=parentLoadFixture();browser.userTypedValue='failed';browser.originalURI={spec:'https://old.test/'};
    p.recoveryDescription={value:'old.test',pageProxyValid:true,searchMode:null,userTypedValue:null};
    p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
    const outcome=p.commitCurrentLoad(p.prepareCurrentLoad(data));assert.equal(outcome.recovery.persistedSearchTerms,false);
    if(change==='state')lazy.independentUrlbarBrowserState.prepareRecovery(browser);
    if(change==='preference')lazy.UrlbarUtils.isPersistedSearchTermsEnabled=()=>false;
    if(change==='originalURI')browser.originalURI={spec:'https://new.test/'};
    if(change==='destroy')p.destroy();
    assert.equal(p.acknowledgeCurrentLoadRecovery(outcome.recovery.token),false,change);
    assert.equal(browser.userTypedValue,'failed');
  }
});

test('parent consumes owned recovery state before typed-value reset and refuses failed state commit',()=>{
  const {p,data,browser,lazy}=parentLoadFixture();browser.userTypedValue='failed';
  p.recoveryDescription={value:'old.test',pageProxyValid:true,searchMode:null,userTypedValue:null};
  p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
  const outcome=p.commitCurrentLoad(p.prepareCurrentLoad(data));
  lazy.independentUrlbarBrowserState.commit=()=>false;
  assert.equal(p.acknowledgeCurrentLoadRecovery(outcome.recovery.token),false);
  assert.equal(browser.userTypedValue,'failed');assert.equal(p.getCurrentLoadRecovery(outcome.recovery.token),null);
});


test('acknowledged result mode commits pinned browser state once without starting a query',async()=>{
  const f=await modeWriteFixture({}, {browserId:42}),p=f.proposal,calls=[];
  f.parent.commitIndependentModeState=(id,mode,observation,current)=>{assert.equal(current(),true);calls.push({id,mode,observation});return {applied:true};};
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}).applied,true);
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);
  assert.equal(calls.length,1);assert.equal(calls[0].id,42);assert.equal(calls[0].mode.engineName,'Engine');
  assert.equal(calls[0].observation.userTypedValue,p.value);assert.equal(calls[0].observation.persistedSearchTerms,false);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('mode-state commit rejects engine, editor and presentation changes after acknowledgment',async()=>{
  for(const change of ['engine','caret','cancel','hidden','removed','selection']) {
    const f=await modeWriteFixture({startQuery:false,modeEntry:{untrimmedValue:'full',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:null}},{browserId:42}),p=f.proposal;let calls=0;
    f.parent.commitIndependentModeState=()=>{calls++;return {applied:true};};
    f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
    if(change==='engine')f.parent.child.updateEngineStore('removed',f.engine,-1);
    if(change==='caret')f.editor.select(1,0,0);
    if(change==='cancel')f.editor.cancel();
    if(change==='hidden'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[f.result],true);}
    if(change==='removed'){f.editor.publishPresentation(1,f.query.id,[],true);f.editor.publishPresentation(1,f.query.id,[f.result],true);}
    if(change==='selection')f.editor.beginResultSelection(1,f.query.id,f.result);
    assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null,change);assert.equal(calls,0);f.editor.close();
  }
});

test('provider reentry and failure cannot leave an owned-mode commit dispatchable',async()=>{
  for(const action of ['cancel','throw']) {
    const f=await modeWriteFixture({}, {browserId:42}),p=f.proposal;
    f.parent.onSelection=()=>{if(action==='throw')throw Error('provider failure');f.editor.cancel();};
    f.parent.commitIndependentModeState=()=>{throw Error('unexpected state commit');};
    if(action==='throw')assert.throws(()=>f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),/provider failure/);
    else f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
    assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);f.editor.close();
  }
});

test('mode-state live guard rejects removal and re-exposure during commit',async()=>{
  const f=await modeWriteFixture({startQuery:false,modeEntry:{untrimmedValue:'full',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:null}},{browserId:42}),p=f.proposal;
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  f.parent.commitIndependentModeState=(_id,_mode,_obs,current)=>{
    assert.equal(current(),true);f.editor.publishPresentation(1,f.query.id,[],true);
    f.editor.publishPresentation(1,f.query.id,[f.result],true);assert.equal(current(),false);return null;
  };
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);f.editor.close();
});

test('parent mode commit validates browser scope, typed state and persistence observation',()=>{
  for(const change of ['none','document','typed','scope','persistence']) {
    const {p,browser,lazy}=parentLoadFixture();browser.userTypedValue='old';let committed=0,discarded=0,current=true;
    lazy.independentUrlbarBrowserState={prepareMode:()=>{
      if(change==='document')browser.browsingContext.currentWindowGlobal={};
      if(change==='typed')browser.userTypedValue='newer';if(change==='scope')current=false;
      return {token:{},effects:{persistedSearchTerms:change==='persistence'}};
    },commit:()=>{committed++;return true;},isCommitted:()=>true,discard:()=>{discarded++;}};
    const result=p.commitIndependentModeState(42,{engineName:'Engine'},{userTypedValue:'chosen',persistedSearchTerms:false,engineId:'engine'},()=>current);
    assert.equal(result?.applied??false,change==='none');assert.equal(committed,change==='none'?1:0);assert.equal(discarded,1);
    assert.equal(browser.userTypedValue,change==='none'?'chosen':change==='typed'?'newer':'old');
  }
});

test('parent mode commit rejects missing targets and preserves typed text for preview exits',()=>{
  const {p,browser,lazy}=parentLoadFixture();browser.userTypedValue='retained';let effects;
  lazy.independentUrlbarBrowserState={prepareMode:(_b,mode)=>({token:{},effects:effects={searchMode:mode,persistedSearchTerms:false}}),commit:()=>true,discard:()=>{}};
  assert.equal(p.commitIndependentModeState(43,null,{persistedSearchTerms:false},()=>true),null);
  assert.equal(p.commitIndependentModeState(42,null,{persistedSearchTerms:false},()=>true).applied,true);
  assert.equal(browser.userTypedValue,'retained');assert.equal(effects.searchMode,null);
  assert.throws(()=>p.commitIndependentModeState(42,null,{},()=>true),/Invalid/);
});


test('parent mode effects run after typed-state commit and suppress later work on reentry',()=>{
  for(const phase of ['none','typed','record','owner']) {
    const {p,browser,lazy}=parentLoadFixture(),events=[];let typed='old',current=true,owned=true;
    Object.defineProperty(browser,'userTypedValue',{get:()=>typed,set:value=>{typed=value;events.push('typed');if(phase==='typed')current=false;}});
    lazy.independentUrlbarBrowserState={prepareMode:()=>({token:{},effects:{persistedSearchTerms:false,recordModeEntry:true}}),
      commit:()=>{events.push('commit');return true;},isCommitted:()=>owned,discard:()=>events.push('discard')};
    p.recordSearchMode=()=>{events.push('record');if(phase==='record')current=false;if(phase==='owner')owned=false;};
    p.markEngineAsUsed=id=>{assert.equal(id,'engine');events.push('engine');};
    const result=p.commitIndependentModeState(42,{engineName:'Engine'},
      {userTypedValue:'chosen',persistedSearchTerms:false,engineId:'engine'},()=>current);
    assert.equal(result.applied,true);assert.equal(result.modeEntryNotified,phase!=='typed');
    assert.equal(result.engineUsageNotified,phase==='none');
    assert.deepEqual(events,phase==='typed'?['commit','typed','discard']:phase==='none'?
      ['commit','typed','record','engine','discard']:['commit','typed','record','discard']);
  }
});

test('engine previews and unchanged modes still notify usage while local modes have no engine effect',()=>{
  for(const engineMode of [false,true]) {
    const {p,browser,lazy}=parentLoadFixture(),events=[];browser.userTypedValue='old';
    lazy.independentUrlbarBrowserState={prepareMode:()=>({token:{},effects:{persistedSearchTerms:false,recordModeEntry:false}}),
      commit:()=>true,isCommitted:()=>true,discard:()=>{}};
    p.recordSearchMode=()=>{throw Error('unexpected mode entry');};p.markEngineAsUsed=id=>events.push(id);
    const result=p.commitIndependentModeState(42,engineMode?{engineName:'Engine',isPreview:true}:{source:1},
      {persistedSearchTerms:false,engineId:engineMode?'engine':null,modeEntry:{untrimmedValue:'old',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'old'}},()=>true);
    assert.equal(result.modeEntryNotified,false);assert.equal(result.engineUsageNotified,engineMode);
    assert.deepEqual(events,engineMode?['engine']:[]);assert.equal(browser.userTypedValue,'old');
  }
});

test('mode-state live guard pins engine id even when a replacement has the same name and mode facts',async()=>{
  const f=await modeWriteFixture({}, {browserId:42}),p=f.proposal;
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  f.parent.commitIndependentModeState=(_id,_mode,observation,current)=>{
    assert.equal(observation.engineId,'engine');assert.equal(current(),true);
    f.parent.child.updateEngineStore('changed',{...f.engine,id:'replacement'},0);
    assert.equal(current(),false);return null;
  };
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);f.editor.close();
});

test('throwing mode effects cannot make an editor state commit retryable',async()=>{
  const f=await modeWriteFixture({}, {browserId:42}),p=f.proposal;let calls=0;
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  f.parent.commitIndependentModeState=()=>{calls++;throw Error('engine notification failed');};
  assert.throws(()=>f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),/notification failed/);
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);
  assert.equal(calls,1);f.editor.close();
});


test('preview commit requires captured full-value facts and exact mode/proxy acknowledgment',async()=>{
  const missing=await modeWriteFixture({startQuery:false},{browserId:42});
  missing.editor.acknowledgeResultModeApplication(missing.proposal.token,observedMode(missing.proposal));
  missing.parent.commitIndependentModeState=()=>{throw Error('missing full value accepted');};
  assert.equal(missing.editor.commitAcknowledgedModeState(missing.proposal.token,{persistedSearchTerms:false}),null);missing.editor.close();
  const facts={untrimmedValue:'https://www.full.test/',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'typed'};
  const f=await modeWriteFixture({startQuery:false,modeEntry:facts},{browserId:42}),p=f.proposal;
  facts.untrimmedValue='mutated';assert.equal(p.modeEntry.untrimmedValue,'https://www.full.test/');assert.equal(Object.isFrozen(p.modeEntry),true);
  for(const wrong of [{modeEntry:undefined},{modeEntry:{...p.modeEntry,untrimmedValue:'changed'}},{pageProxyValid:true}])
    assert.equal(f.editor.acknowledgeResultModeApplication(p.token,{...observedMode(p),...wrong}),false);
  assert.equal(f.editor.acknowledgeResultModeApplication(p.token,observedMode(p)),true);
  f.parent.commitIndependentModeState=(_id,_mode,observation,current)=>{
    assert.equal(current(),true);assert.equal(observation.modeEntry,p.modeEntry);assert.equal(observation.userTypedValue,undefined);return {applied:true};
  };
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}).applied,true);
  assert.equal(f.editor.beginResultModeExit(1,f.query.id).value,'@engine','preview must preserve the query typed basis');f.editor.close();
});

test('parent preview typed write preserves full text and Firefox page/autofill/Reader Mode policy',()=>{
  for(const [proxy,autofill,typed,expected] of [[false,false,'typed','https://www.full.test/'],
    [true,false,'typed',''],[true,true,'reader',''],[false,true,'typed','typed'],
    [false,true,'reader','https://reader.test/'],[false,true,'','https://www.full.test/'],[false,true,null,'https://www.full.test/']]) {
    const {p,browser,lazy}=parentLoadFixture();browser.userTypedValue=typed;
    lazy.ReaderMode.getOriginalUrlObjectForDisplay=value=>value==='reader'?{displaySpec:'https://reader.test/'}:null;
    lazy.independentUrlbarBrowserState={prepareMode:()=>({token:{},effects:{persistedSearchTerms:false,recordModeEntry:false}}),commit:()=>true,isCommitted:()=>true,discard:()=>{}};
    const result=p.commitIndependentModeState(42,{engineName:'Engine',isPreview:true},{engineId:'engine',persistedSearchTerms:false,
      modeEntry:{untrimmedValue:'https://www.full.test/',pageProxyValid:proxy,hasAutofillPlaceholder:autofill,userTypedValue:typed}},()=>true);
    assert.equal(result.applied,true);assert.equal(browser.userTypedValue,expected);
  }
});

test('preview typed-state capture rejects changed browser text and normalization reentry',()=>{
  for(const change of ['typed','document']) {
    const {p,browser,lazy}=parentLoadFixture();browser.userTypedValue=change==='typed'?'newer':'reader';let prepared=0;
    lazy.independentUrlbarBrowserState.prepareMode=()=>{prepared++;throw Error('stale preparation');};
    lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>{browser.browsingContext.currentWindowGlobal={};return {displaySpec:'old'};};
    const result=p.commitIndependentModeState(42,{engineName:'Engine',isPreview:true},{engineId:'engine',persistedSearchTerms:false,
      modeEntry:{untrimmedValue:'full',pageProxyValid:false,hasAutofillPlaceholder:true,userTypedValue:'reader'}},()=>true);
    assert.equal(result,null);assert.equal(prepared,0);assert.equal(browser.userTypedValue,change==='typed'?'newer':'reader');
  }
});

test('pre-mode facts reject malformed snapshots before provider selection',async()=>{
  const f=await modeWriteFixture();const before=f.calls.length;
  for(const modeEntry of [null,{}, {untrimmedValue:3},
    {untrimmedValue:'full',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:undefined}]) {
    assert.throws(()=>f.editor.beginResultModeApplication(1,f.query.id,f.result,{modeEntry}),/Invalid pre-mode/);
  }
  assert.equal(f.calls.length,before);f.editor.close();
});


test('committed mode notification is separate, immutable, correlated and delivered only once',async()=>{
  const notifications=[];
  const f=await modeWriteFixture({}, {browserId:42,onModeStateChanged:n=>notifications.push(n)}),p=f.proposal;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    assert.equal(current(),true);notify(Object.freeze({browserId:id,searchMode:Object.freeze({...mode}),
      userTypedValue:facts.userTypedValue,persistedSearchTerms:false,persistenceChanged:false}));return {applied:true,modeChangeNotified:true};
  };
  assert.equal(notifications.length,0);f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  assert.equal(notifications.length,0);const acknowledged=f.editor.snapshot;
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}).modeChangeNotified,true);
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);
  assert.equal(notifications.length,1);const n=notifications[0];
  assert.equal(n.kind,'mode-state-changed');assert.equal(n.reason,'result-mode');assert.equal(n.token,p.token);
  assert.equal(n.queryId,f.query.id);assert.equal(n.edit,acknowledged);assert.equal(n.browserId,42);
  assert.equal(n.userTypedValue,p.value);assert.equal(n.searchMode.engineName,'Engine');
  assert.equal(Object.isFrozen(n),true);assert.equal(Object.isFrozen(n.searchMode),true);assert.equal(Object.isFrozen(n.edit),true);
  assert.equal(f.updates.length,0);assert.equal(f.parent.started.length,1);f.editor.close();
});

test('mode notification exceptions are reported without retrying or undoing the committed operation',async()=>{
  let delivered=0;const f=await modeWriteFixture({}, {browserId:42,onModeStateChanged:()=>{delivered++;throw Error('consumer failed');}}),p=f.proposal,errors=[];
  f.sandbox.console={error:e=>errors.push(e.message)};
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    notify(Object.freeze({browserId:id,searchMode:Object.freeze({...mode}),userTypedValue:facts.userTypedValue,persistedSearchTerms:false}));return {applied:true};
  };
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}).applied,true);
  assert.deepEqual(errors,['consumer failed']);assert.equal(delivered,1);
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}),null);
  assert.equal(f.editor.snapshot.text,p.value);f.editor.close();
});

test('reentrant mode consumer revokes old follow-up query without rewriting the delivered snapshot',async()=>{
  let f,delivered;
  f=await modeWriteFixture({}, {browserId:42,onModeStateChanged:n=>{delivered=n;f.editor.cancel();}});const p=f.proposal;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    assert.equal(current(),true);notify(Object.freeze({browserId:id,searchMode:Object.freeze({...mode}),userTypedValue:facts.userTypedValue,persistedSearchTerms:false}));return {applied:true};
  };
  f.editor.acknowledgeResultModeApplication(p.token,observedMode(p));const before=f.editor.snapshot;
  assert.equal(f.editor.commitAcknowledgedModeState(p.token,{persistedSearchTerms:false}).applied,true);
  assert.equal(delivered.edit,before);await assert.rejects(f.editor.requeryAfterModeApplication(p.token,{searchMode:p.searchMode}),/Stale/);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('parent notifies committed mode only after effects and suppresses superseded delivery',()=>{
  for(const phase of ['none','typed','record','engine','owner']) {
    const {p,browser,lazy}=parentLoadFixture(),events=[];let typed='old',current=true,owned=true;
    Object.defineProperty(browser,'userTypedValue',{get:()=>typed,set:v=>{typed=v;events.push('typed');if(phase==='typed')current=false;}});
    lazy.independentUrlbarBrowserState={prepareMode:()=>({token:{},effects:{searchMode:Object.freeze({engineName:'Engine'}),persistedSearchTerms:false,recordModeEntry:true}}),
      commit:()=>{events.push('commit');return true;},isCommitted:()=>owned,discard:()=>events.push('discard')};
    p.recordSearchMode=()=>{events.push('record');if(phase==='record')current=false;};
    p.markEngineAsUsed=()=>{events.push('engine');if(phase==='engine')current=false;if(phase==='owner')owned=false;};
    const outcome=p.commitIndependentModeState(42,{engineName:'Engine'},{userTypedValue:'chosen',persistedSearchTerms:false,engineId:'engine'},()=>current,n=>{
      events.push('notify');assert.equal(n.browserId,42);assert.equal(n.userTypedValue,'chosen');
      assert.equal(Object.isFrozen(n),true);assert.equal(Object.isFrozen(n.searchMode),true);
    });
    assert.equal(outcome.applied,true);assert.equal(outcome.modeChangeNotified,phase==='none');
    assert.equal(events.includes('notify'),phase==='none');
    if(phase==='none')assert.deepEqual(events,['commit','typed','record','engine','notify','discard']);
  }
});

test('mode observer is optional and invalid callbacks fail before state application',async()=>{
  const f=fixture();assert.throws(()=>new f.sandbox.UrlbarEditSession({context:f.context,isCurrent:()=>true,onUpdate:()=>{},onModeStateChanged:{}}),/Invalid mode-state listener/);f.session.close();
  const {p,browser,lazy}=parentLoadFixture();browser.userTypedValue='old';let calls=0;
  lazy.independentUrlbarBrowserState={prepareMode:()=>({token:{},effects:{searchMode:null,persistedSearchTerms:false}}),commit:()=>{calls++;return true;},discard:()=>{}};
  assert.throws(()=>p.commitIndependentModeState(42,null,{persistedSearchTerms:false},()=>true,{}),/Invalid independent/);
  assert.equal(calls,0);
  assert.equal(p.commitIndependentModeState(42,null,{persistedSearchTerms:false},()=>true).modeChangeNotified,false);assert.equal(calls,1);
});


test('recovery delivers one frozen notification correlated with the recovered editor',async()=>{
  const events=[],f=await editorRecoveryFixture({onModeStateChanged:n=>events.push(n)});
  const before=f.editor.snapshot,p=f.editor.getNavigationRecovery(f.recovery.token,{focused:false});
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,{...recoveryObservation(p),text:'wrong'}),false);
  assert.equal(events.length,0);
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),true);
  assert.equal(events.length,1);const n=events[0];
  assert.equal(n.kind,'mode-state-changed');assert.equal(n.reason,'navigation-recovery');
  assert.equal(n.token,p.token);assert.equal(n.queryId,f.q.id);assert.equal(n.browserId,42);
  assert.equal(n.edit,f.editor.snapshot);assert.notEqual(n.edit,before);assert.equal(n.edit.text,p.value);
  assert.equal(n.searchMode,null);assert.equal(n.userTypedValue,null);assert.equal(n.persistedSearchTerms,false);
  assert.equal(Object.isFrozen(n),true);assert.equal(Object.isFrozen(n.edit),true);
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),false);
  assert.equal(events.length,1);f.editor.close();
});

test('recovery notification errors and consumer reentry cannot repeat the committed recovery',async()=>{
  for(const action of ['throw','cancel','edit']) {
    let f,next,delivered;
    f=await editorRecoveryFixture({onModeStateChanged:n=>{
      delivered=n;
      if(action==='throw')throw Error('consumer failure');
      if(action==='cancel')f.editor.cancel();
      if(action==='edit')next=f.editor.edit({},edit(2,'new consumer edit'));
    }});
    const p=f.editor.getNavigationRecovery(f.recovery.token,{focused:false});
    assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),true,action);
    if(next)await next;
    assert.equal(delivered.edit.text,p.value);
    if(action==='edit')assert.equal(f.editor.snapshot.text,'new consumer edit');
    assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),false);f.editor.close();
  }
});

test('recovery checks editor again after browser validation reenters the session',async()=>{
  const events=[],f=await editorRecoveryFixture({onModeStateChanged:n=>events.push(n)});
  const p=f.editor.getNavigationRecovery(f.recovery.token,{focused:false});let next;
  f.parent.acknowledgeCurrentLoadRecovery=(_token,apply)=>apply(()=>{
    next=f.editor.edit({},edit(2,'newer edit'));return true;
  });
  assert.equal(f.editor.acknowledgeNavigationRecovery(p.token,recoveryObservation(p)),false);
  await next;assert.equal(f.editor.snapshot.text,'newer edit');assert.equal(events.length,0);f.editor.close();
});

function notifyingRecoveryFixture() {
  const f=parentLoadFixture();f.browser.userTypedValue='failed';
  f.p.recoveryDescription={value:'old.test',pageProxyValid:true,searchMode:null,userTypedValue:null};
  f.p.browserWindow.openTrustedLinkIn=()=>{throw {result:1};};
  f.recovery=f.p.commitCurrentLoad(f.p.prepareCurrentLoad(f.data)).recovery;return f;
}

test('parent recovery applies editor after browser commit and before notification',()=>{
  const {p,browser,lazy,recovery}=notifyingRecoveryFixture(),calls=[];
  const commit=lazy.independentUrlbarBrowserState.commit;
  lazy.independentUrlbarBrowserState.commit=(...args)=>{calls.push('commit');return commit(...args);};
  assert.equal(p.acknowledgeCurrentLoadRecovery(recovery.token,current=>{
    assert.equal(browser.userTypedValue,null);assert.equal(current(),true);calls.push('editor');return true;
  },()=>true,n=>{calls.push('notify');assert.equal(n.searchMode,null);assert.equal(Object.isFrozen(n),true);}),true);
  assert.deepEqual(calls,['commit','editor','notify']);
});

test('parent recovery suppresses stale notifications after continuation reentry',()=>{
  for(const change of ['load','document','typed','state','display','originalURI','preference','destroy','editor']) {
    const {p,browser,lazy,recovery,data}=notifyingRecoveryFixture();let notified=false;
    assert.equal(p.acknowledgeCurrentLoadRecovery(recovery.token,()=>{
      if(change==='load')p.prepareCurrentLoad(data);
      if(change==='document')browser.browsingContext.currentWindowGlobal={};
      if(change==='typed')browser.userTypedValue='newer';
      if(change==='state')lazy.independentUrlbarBrowserState.prepareRecovery(browser);
      if(change==='display')p.recoveryDescription={...p.recoveryDescription,value:'newer'};
      if(change==='originalURI')browser.originalURI={spec:'https://newer.test/'};
      if(change==='preference')lazy.UrlbarUtils.isPersistedSearchTermsEnabled=()=>false;
      if(change==='destroy')p.destroy();
      return true;
    },()=>change!=='editor',()=>{notified=true;}),false,change);
    assert.equal(notified,false,change);
  }
});

test('malformed recovery continuations fail before consuming browser state',()=>{
  const {p,browser,recovery}=notifyingRecoveryFixture();
  for(const args of [[1],[null,1],[null,()=>true,1]]) {
    assert.throws(()=>p.acknowledgeCurrentLoadRecovery(recovery.token,...args),/Invalid recovery/);
    assert.equal(browser.userTypedValue,'failed');
  }
  assert.equal(p.acknowledgeCurrentLoadRecovery(recovery.token),true);
});


async function ownedExplicitExitFixture(options={}) {
  const f=await editorFixture({browserId:42,...options});
  await f.editor.edit({searchMode:{source:1,isPreview:false}},edit(1,'unchanged'));
  const proposal=f.editor.beginSearchModeExit(1);
  return {...f,proposal};
}

test('explicit exit commits acknowledged browser state once without result rows or text writes',async()=>{
  const notices=[],f=await ownedExplicitExitFixture({onModeStateChanged:n=>notices.push(n)}),p=f.proposal,calls=[];
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    calls.push({id,mode,facts});assert.equal(current(),true);
    notify(Object.freeze({browserId:id,searchMode:null,userTypedValue:'browser text',persistedSearchTerms:false}));
    return {applied:true};
  };
  assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),null);
  assert.equal(f.editor.acknowledgeSearchModeExit(p.token,observedExit(p)),true);
  assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}).applied,true);
  assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),null);
  assert.equal(calls.length,1);assert.equal(calls[0].id,42);assert.equal(calls[0].mode,null);
  assert.equal(calls[0].facts.userTypedValue,undefined);assert.equal(f.parent.started.length,1);
  assert.equal(notices.length,1);assert.equal(notices[0].reason,'explicit-mode-exit');assert.equal(notices[0].queryId,null);
  assert.equal(notices[0].edit,p.edit);assert.equal(notices[0].token,p.token);assert.equal(Object.isFrozen(notices[0]),true);
  assert.equal(f.editor.snapshot,p.edit);
  await f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false});
  assert.equal(f.parent.started.length,2);f.editor.close();
});

test('failed explicit exit commit revokes requery and malformed observation does not consume it',async()=>{
  for(const failure of ['reject','throw']) {
    const f=await ownedExplicitExitFixture(),p=f.proposal;
    f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
    assert.throws(()=>f.editor.commitAcknowledgedSearchModeExit(p.token,{}),/persisted-search/);
    f.parent.commitIndependentModeState=()=>{if(failure==='throw')throw Error('commit failed');return null;};
    if(failure==='throw')assert.throws(()=>f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),/commit failed/);
    else assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),null);
    assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),null);
    await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false}),/Stale/);f.editor.close();
  }
});

test('explicit exit state commit is revoked by new editor work and requires a pinned browser',async()=>{
  for(const revoke of [f=>f.editor.cancel(),f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new'))]) {
    const f=await ownedExplicitExitFixture(),p=f.proposal;let calls=0;
    f.parent.commitIndependentModeState=()=>{calls++;return {applied:true};};
    f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));await revoke(f);
    assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),null);assert.equal(calls,0);f.editor.close();
  }
  const f=await ownedExplicitExitFixture({browserId:undefined}),p=f.proposal;
  f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
  assert.throws(()=>f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}),/pinned browser/);f.editor.close();
});

test('explicit exit notification reentry preserves completed commit but revokes follow-up',async()=>{
  let f;
  f=await ownedExplicitExitFixture({onModeStateChanged:()=>f.editor.cancel()});const p=f.proposal;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    assert.equal(current(),true);notify({browserId:id,searchMode:null});assert.equal(current(),false);return {applied:true};
  };
  f.editor.acknowledgeSearchModeExit(p.token,observedExit(p));
  assert.equal(f.editor.commitAcknowledgedSearchModeExit(p.token,{persistedSearchTerms:false}).applied,true);
  await assert.rejects(f.editor.requeryAfterSearchModeExit(p.token,{searchMode:null},{pageProxyValid:false}),/Stale/);f.editor.close();
});


const commandEntry=()=>({untrimmedValue:'previous full value',pageProxyValid:false,
  hasAutofillPlaceholder:false,userTypedValue:'previous typed value'});
const observedOwnedCommand=p=>({...observedCommand(p),modeEntry:p.modeEntry,pageProxyValid:p.pageProxyValid});
async function ownedCommandFixture(value='^ new words',options={},fixtureOptions={}) {
  const f=await editorFixture({browserId:42,...fixtureOptions});await f.editor.edit({},edit(1,'previous'));
  f.parent.child.updateEngineStore('changed',{id:'engine',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true},-1);
  const proposal=await f.editor.beginSearchCommand(1,value,{modeEntry:commandEntry(),...options});
  return {...f,proposal};
}

test('command mode commit requires captured entry and invalidated proxy observation',async()=>{
  const entry=commandEntry(),f=await ownedCommandFixture('^ words',{modeEntry:entry}),p=f.proposal;
  entry.untrimmedValue='mutated';assert.equal(p.modeEntry.untrimmedValue,'previous full value');
  for(const wrong of [{modeEntry:undefined},{pageProxyValid:true},{modeEntry:{...p.modeEntry,userTypedValue:'wrong'}}]) {
    assert.equal(f.editor.acknowledgeSearchCommand(p.token,{...observedOwnedCommand(p),...wrong}),false);
  }
  assert.equal(f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p)),true);f.editor.close();
  const g=await ownedCommandFixture('^ words',{modeEntry:undefined}),q=g.proposal;
  g.editor.acknowledgeSearchCommand(q.token,observedOwnedCommand(q));
  assert.equal(g.editor.commitAcknowledgedSearchCommandMode(q.token,{persistedSearchTerms:false}),null);
  assert.equal(g.editor.getSearchCommandRequery(q.token),null);g.editor.close();
});

test('local and engine command commits use initial preview policy and frozen editor notifications once',async()=>{
  for(const value of ['^ words','@e words']) {
    const events=[],f=await ownedCommandFixture(value,value.startsWith('@')?{engineId:'engine'}:{},
      {onModeStateChanged:n=>events.push(n)}),p=f.proposal,calls=[];
    f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
      assert.equal(current(),true);calls.push({id,mode,facts});
      notify({browserId:id,searchMode:mode,userTypedValue:'previous full value',persistedSearchTerms:false});return {applied:true};
    };
    assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}),null);
    f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
    assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}).applied,true);
    assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}),null);
    assert.equal(calls.length,1);assert.equal(calls[0].mode.isPreview,true);
    assert.equal(calls[0].facts.userTypedValue,undefined);assert.equal(calls[0].facts.modeEntry,p.modeEntry);
    assert.equal(calls[0].facts.engineId,value.startsWith('@')?'engine':null);
    assert.equal(events.length,1);assert.equal(events[0].reason,'search-command');assert.equal(events[0].queryId,null);
    assert.equal(events[0].edit,f.editor.snapshot);assert.equal(events[0].token,p.token);assert.equal(Object.isFrozen(events[0]),true);
    assert.equal(f.parent.started.length,1);assert.ok(f.editor.getSearchCommandRequery(p.token));f.editor.close();
  }
});

test('ordinary commands skip mode effects while unsupported restrictions commit clearing without text writes',async()=>{
  for(const value of ['ordinary text','#']) {
    const f=await ownedCommandFixture(value),p=f.proposal;let calls=0;
    f.parent.commitIndependentModeState=(_id,mode,facts)=>{
      calls++;assert.equal(mode,null);assert.equal(facts.userTypedValue,undefined);return {applied:true};
    };
    f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
    const result=f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});
    assert.equal(result.applied,true);assert.equal(calls,value==='#'?1:0);
    if(value!=='#')assert.equal(result.modeUnchanged,true);
    assert.ok(f.editor.getSearchCommandRequery(p.token));f.editor.close();
  }
});

test('command state commits reject same-name engine replacement and consumed failure cannot requery',async()=>{
  for(const change of ['replace','reject','throw']) {
    const f=await ownedCommandFixture('@e words',{engineId:'engine'}),p=f.proposal;let calls=0;
    f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
    if(change==='replace'){
      f.parent.child.updateEngineStore('removed',{id:'engine'});
      f.parent.child.updateEngineStore('changed',{id:'new',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true},-1);
    }
    f.parent.commitIndependentModeState=()=>{calls++;if(change==='throw')throw Error('failure');return null;};
    assert.throws(()=>f.editor.commitAcknowledgedSearchCommandMode(p.token,{}),/persisted-search/);
    if(change==='throw')assert.throws(()=>f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}),/failure/);
    else assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}),null);
    assert.equal(calls,change==='replace'?0:1);assert.equal(f.editor.getSearchCommandRequery(p.token),null);f.editor.close();
  }
});

test('new editor work and notification reentry revoke command follow-up without repeating effects',async()=>{
  for(const revoke of [f=>f.editor.cancel(),f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new'))]) {
    const f=await ownedCommandFixture(),p=f.proposal;
    f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));await revoke(f);
    assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}),null);f.editor.close();
  }
  let f;f=await ownedCommandFixture('^ words',{}, {onModeStateChanged:()=>f.editor.cancel()});const p=f.proposal;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{notify({browserId:id,searchMode:mode});assert.equal(current(),false);return {applied:true};};
  f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}).applied,true);
  assert.equal(f.editor.getSearchCommandRequery(p.token),null);f.editor.close();
});


const observedConfirmation=p=>({searchMode:p.searchMode,text:p.edit.text,selectionStart:p.edit.selectionStart,
  selectionEnd:p.edit.selectionEnd,selectionDirection:p.edit.selectionDirection,modeEntry:p.modeEntry,
  pageProxyValid:false,persistedSearchTerms:false});
async function commandConfirmationFixture(value='^ words',options={},fixtureOptions={}) {
  const f=await ownedCommandFixture(value,options,fixtureOptions),p=f.proposal;
  f.initialToken=Object.freeze({});
  f.parent.commitIndependentModeState=()=>({applied:true,stateToken:f.initialToken});
  f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});
  f.inputToken=Object.freeze({});
  f.parent.commitIndependentInputState=()=>({applied:true,current:true,stateToken:f.inputToken});
  f.editor.commitAcknowledgedSearchCommandInput(p.token,{...observedCommand(p),userTypedValue:'previous',persistedSearchTerms:false});
  const confirmation=f.editor.beginSearchCommandConfirmation(p.token,{...commandEntry(),untrimmedValue:f.editor.snapshot.text});
  return {...f,confirmation};
}

test('command confirmation requires initial commit and exact unchanged editor and entry observation',async()=>{
  const f=await ownedCommandFixture(),p=f.proposal;
  f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  assert.equal(f.editor.beginSearchCommandConfirmation(p.token,commandEntry()),null);f.editor.close();
  const g=await commandConfirmationFixture(),c=g.confirmation;let called=0;
  g.parent.commitIndependentModeState=()=>{called++;return {applied:true};};
  assert.equal(c.searchMode.isPreview,false);assert.equal(c.commandToken,g.proposal.token);assert.equal(Object.isFrozen(c.modeEntry),true);
  for(const wrong of [{searchMode:g.proposal.searchMode},{text:'wrong'},{selectionStart:0},{pageProxyValid:true},
    {modeEntry:{...c.modeEntry,userTypedValue:'new'}}]) {
    assert.equal(g.editor.commitSearchCommandConfirmation(c.token,{...observedConfirmation(c),...wrong}),null);
  }
  assert.equal(called,0);assert.equal(g.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)).applied,true);
  assert.equal(g.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)),null);assert.equal(called,1);g.editor.close();
});

test('confirmed command commit preserves preview query context and emits confirmed editor correlation',async()=>{
  const events=[],f=await commandConfirmationFixture('@e words',{engineId:'engine'},
    {onModeStateChanged:n=>events.push(n)}),c=f.confirmation,p=f.proposal;
  await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:c.searchMode}),/confirmation has not committed/);
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    assert.equal(current(),true);assert.equal(mode.isPreview,false);assert.equal(facts.expectedStateToken,f.inputToken);
    assert.equal(facts.engineId,'engine');assert.equal(facts.modeEntry,c.modeEntry);
    notify({browserId:id,searchMode:mode,userTypedValue:c.edit.text});return {applied:true,stateToken:{},userTypedValue:c.edit.text};
  };
  assert.equal(f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)).applied,true);
  assert.equal(events.length,1);assert.equal(events[0].reason,'search-command-confirmation');
  assert.equal(events[0].token,c.token);assert.equal(events[0].commandToken,p.token);assert.equal(events[0].edit,c.edit);
  const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:c.searchMode});
  assert.equal(q.searchMode.isPreview,true);assert.equal(f.editor.snapshot,c.edit);
  assert.equal(f.editor.beginSearchModeExit(1).previousSearchMode.isPreview,false);f.editor.close();
});

test('confirmation failure and engine changes consume follow-up authority',async()=>{
  for(const action of ['reject','throw','engine']) {
    const f=await commandConfirmationFixture('@e words',{engineId:'engine'}),c=f.confirmation;let called=0;
    f.parent.commitIndependentModeState=()=>{called++;if(action==='throw')throw Error('failed confirmation');return null;};
    if(action==='engine')f.parent.child.updateEngineStore('changed',{id:'replacement',name:'Engine',aliases:['@e'],isGeneralPurposeEngine:true},0);
    if(action==='throw')assert.throws(()=>f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)),/failed confirmation/);
    else assert.equal(f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)),null);
    assert.equal(called,action==='engine'?0:1);
    await assert.rejects(f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:c.searchMode}),/Stale/);f.editor.close();
  }
});

test('new confirmation proposals and editor work revoke older confirmation tokens',async()=>{
  const f=await commandConfirmationFixture(),old=f.confirmation;
  const next=f.editor.beginSearchCommandConfirmation(f.proposal.token,commandEntry());
  assert.equal(f.editor.commitSearchCommandConfirmation(old.token,observedConfirmation(old)),null);
  assert.ok(next);f.editor.cancel();assert.equal(f.editor.commitSearchCommandConfirmation(next.token,observedConfirmation(next)),null);f.editor.close();
  const g=await commandConfirmationFixture(),c=g.confirmation;g.editor.select(1,0,0);
  assert.equal(g.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)),null);g.editor.close();
});

test('confirmation consumer reentry does not undo state but revokes the old requery',async()=>{
  let f;f=await commandConfirmationFixture('^ words',{}, {onModeStateChanged:()=>f.editor.cancel()});const c=f.confirmation;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{notify({browserId:id,searchMode:mode});return {applied:true};};
  assert.equal(f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)).applied,true);
  await assert.rejects(f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:c.searchMode}),/Stale/);f.editor.close();
});

test('parent continuation mode commit requires the exact preceding owned commit receipt',()=>{
  for(const valid of [true,false]) {
    const {p,browser,lazy}=parentLoadFixture(),previous={},next={};let prepared=0;
    lazy.independentUrlbarBrowserState={isCommitted:(_b,t)=>valid&&t===previous,
      prepareMode:()=>{prepared++;return {token:next,effects:{persistedSearchTerms:false}};},commit:()=>true,discard:()=>{}};
    const outcome=p.commitIndependentModeState(42,null,{persistedSearchTerms:false,expectedStateToken:previous},()=>true);
    assert.equal(prepared,valid?1:0);assert.equal(outcome?.applied??false,valid);
    if(valid)assert.equal(outcome.stateToken,next);
  }
});


async function commandInputFixture(value='^ words') {
  const f=await ownedCommandFixture(value),p=f.proposal;f.stateToken={};
  f.parent.commitIndependentModeState=()=>({applied:true,stateToken:f.stateToken});
  f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});
  return f;
}
const observedCommandInput=p=>({...observedCommand(p),userTypedValue:'previous browser text',persistedSearchTerms:false});

test('command input requires exact observation and initial receipt, commits once without querying',async()=>{
  const f=await commandInputFixture(),p=f.proposal,calls=[],next={};
  f.parent.child.userSelectionBehavior='arrow';
  f.parent.commitIndependentInputState=(id,value,facts,current)=>{
    calls.push(value);assert.equal(id,42);assert.equal(current(),true);assert.equal(facts.expectedStateToken,f.stateToken);
    assert.equal(facts.userTypedValue,'previous browser text');return {applied:true,current:true,stateToken:next};
  };
  assert.equal(f.editor.beginSearchCommandConfirmation(p.token,commandEntry()),null);
  for(const wrong of [{text:'wrong'},{selectionStart:0},{searchMode:null},{selectionDirection:'backward'}]) {
    assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,{...observedCommandInput(p),...wrong}),null);
  }
  assert.equal(calls.length,0);
  assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)).applied,true);
  assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)),null);
  assert.equal(f.parent.child.userSelectionBehavior,'none');assert.equal(f.parent.started.length,1);assert.deepEqual(calls,['words']);
  const confirmation=f.editor.beginSearchCommandConfirmation(p.token,commandEntry());assert.ok(confirmation);
  f.parent.commitIndependentModeState=(_id,_mode,facts)=>{assert.equal(facts.expectedStateToken,next);return {applied:true};};
  f.editor.commitSearchCommandConfirmation(confirmation.token,observedConfirmation(confirmation));f.editor.close();
});

test('clearing command cannot query before input commit and then uses its unchanged editor',async()=>{
  const f=await commandInputFixture('#'),p=f.proposal,snapshot=f.editor.snapshot;
  await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:null}),/input has not committed/);
  f.parent.commitIndependentInputState=()=>({applied:true,current:true,stateToken:{}});
  f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
  const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:null});
  assert.equal(q.searchString,'# ');assert.equal(f.editor.snapshot,snapshot);f.editor.close();
});

test('failed or superseded input application revokes follow-up without repeating browser effects',async()=>{
  for(const action of ['reject','throw','stale','reentry']) {
    const f=await commandInputFixture(),p=f.proposal;let calls=0;
    f.parent.commitIndependentInputState=()=>{calls++;if(action==='throw')throw Error('input failed');
      if(action==='reject')return null;if(action==='reentry')f.editor.cancel();return {applied:true,current:action!=='stale',stateToken:{}};};
    assert.throws(()=>f.editor.commitAcknowledgedSearchCommandInput(p.token,{}),/Invalid/);assert.equal(calls,0);
    if(action==='throw')assert.throws(()=>f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)),/input failed/);
    else f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
    assert.equal(calls,1);assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)),null);
    assert.equal(f.editor.getSearchCommandRequery(p.token),null);f.editor.close();
  }
});

test('new editor work revokes pending command input and ordinary preservation grants input receipts',async()=>{
  for(const revoke of [f=>f.editor.cancel(),f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new'))]) {
    const f=await commandInputFixture(),p=f.proposal;await revoke(f);
    assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)),null);f.editor.close();
  }
  const f=await ownedCommandFixture('ordinary'),p=f.proposal;f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  assert.equal(f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false}).modeUnchanged,true);
  f.parent.commitIndependentInputState=()=>({applied:true,current:true,stateToken:{}});
  assert.equal(f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p)).applied,true);f.editor.close();
});

test('parent input commit validates receipt, persistence and lifetime before browser text mutation',()=>{
  for(const change of ['none','old-text','receipt','document','scope','persistence']) {
    const {p,browser,lazy}=parentLoadFixture(),prior={},next={};let prepared=0,committed=0,current=true;
    browser.userTypedValue='old';let latest=prior;
    lazy.independentUrlbarBrowserState={isCommitted:(_b,t)=>t===latest,
      prepareInput:(_b,value)=>{prepared++;latest=next;if(change==='document')browser.browsingContext.currentWindowGlobal={};
        if(change==='scope')current=false;return {token:next,effects:{userTypedValue:value,persistenceChanged:true,persistedSearchTerms:change==='persistence'}};},
      commit:()=>{committed++;return true;},discard:()=>{}};
    if(change==='receipt')latest={};
    const result=p.commitIndependentInputState(42,'new',{expectedStateToken:prior,persistedSearchTerms:false,
      userTypedValue:change==='old-text'?'different':'old'},()=>current);
    assert.equal(result?.applied??false,change==='none',change);assert.equal(committed,change==='none'?1:0);
    assert.equal(browser.userTypedValue,change==='none'?'new':'old');assert.equal(prepared,['old-text','receipt'].includes(change)?0:1);
    if(result){assert.equal(result.current,true);assert.equal(result.stateToken,next);assert.equal(Object.isFrozen(result),true);}
  }
});

test('parent input commits state before browser setters and reports reentrant supersession',()=>{
  const {p,browser,lazy}=parentLoadFixture(),prior={},next={},events=[];let text='old',latest=prior;
  lazy.independentUrlbarBrowserState={isCommitted:(_b,t)=>t===latest,
    prepareInput:()=>({token:next,effects:{userTypedValue:'new',persistedSearchTerms:false}}),
    commit:()=>{events.push('state');latest=next;return true;},discard:()=>{}};
  Object.defineProperty(browser,'userTypedValue',{get:()=>text,set:value=>{events.push('text');text=value;latest={};}});
  const result=p.commitIndependentInputState(42,'new',{userTypedValue:'old',expectedStateToken:prior,persistedSearchTerms:false},()=>true);
  assert.equal(result.applied,true);assert.equal(result.current,false);assert.deepEqual(events,['state','text']);
});


test('dispatch authority is evaluated after engine readiness and never enters provider query data',async()=>{
  for(const allowed of [true,false]) {
    const f=fixture({hold:true}),events=[];let ready=false,calls=0;
    const pending=f.session.query({}, {searchString:'guarded'},u=>events.push(u.kind),()=>{calls++;assert.equal(ready,true);return allowed;});
    assert.equal(calls,0);ready=true;f.parent.child.updateEngineStore('init',[],null);
    if(allowed){const q=await pending;assert.equal(Object.hasOwn(q,'authorize'),false);assert.equal(f.parent.started.length,1);}
    else {await assert.rejects(pending,/dispatch authority/);assert.equal(f.parent.started.length,0);}
    assert.equal(calls,1);f.session.close();
  }
});

test('dispatch guard cancellation and replacement cannot dispatch the obsolete context',async()=>{
  for(const action of ['cancel','replace','throw']) {
    const f=fixture();let next;
    const pending=f.session.query({}, {searchString:'old'},null,()=>{
      if(action==='throw')throw Error('guard failed');
      if(action==='cancel')f.session.cancel();
      if(action==='replace')next=f.session.query({}, {searchString:'new'});
      return true;
    });
    await assert.rejects(pending,action==='throw'?/guard failed/:/superseded|cancelled/);
    if(next)await next;
    assert.deepEqual(f.parent.started.map(q=>q.searchString),action==='replace'?['new']:[]);
    await f.session.query({}, {searchString:'later'});assert.equal(f.parent.started.at(-1).searchString,'later');f.session.close();
  }
});

test('invalid dispatch guards fail before replacing an owned query',async()=>{
  const f=fixture(),q=await f.session.query({}, {searchString:'keep'});
  await assert.rejects(f.session.query({}, {searchString:'bad'},null,1),/Invalid query dispatch guard/);
  assert.equal(f.session.ownsQuery(q.id),true);f.session.close();
});

test('parent dispatch authority pins receipt, typed text, selected browser and document lifetime',()=>{
  for(const change of ['receipt','text','document','epoch','selected','destroy']) {
    const {p,browser,lazy}=parentLoadFixture(),token={};let owned=true;browser.userTypedValue='command';
    lazy.independentUrlbarBrowserState.isCommitted=(_b,t)=>owned&&t===token;
    const guard=p.captureIndependentQueryAuthority(42,token,'command');assert.equal(guard(),true);
    if(change==='receipt')owned=false;if(change==='text')browser.userTypedValue='new';
    if(change==='document')browser.browsingContext.currentWindowGlobal={};if(change==='epoch')browser.lastLocationChange++;
    if(change==='selected')p.browserWindow.gBrowser.selectedBrowser={};if(change==='destroy')p.destroy();
    assert.equal(guard(),false,change);assert.equal(p.captureIndependentQueryAuthority(42,token,'command')===null,change!=='epoch'&&change!=='document');
  }
});

test('committed command dispatch uses the final input receipt and can reject while engines are held',async()=>{
  const f=await commandInputFixture('#'),p=f.proposal,inputToken={};let allowed=true,checked=0;
  f.parent.commitIndependentInputState=()=>({applied:true,current:true,stateToken:inputToken});
  f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
  f.parent.captureIndependentQueryAuthority=(id,token,text)=>{
    assert.equal(id,42);assert.equal(token,inputToken);assert.equal(text,'# ');return ()=>{checked++;return allowed;};
  };
  const store=f.parent.child.engineStore,gate=Promise.withResolvers();
  Object.defineProperty(store,'initialized',{value:false});store.init=()=>gate.promise;
  const query=f.editor.requeryAfterSearchCommand(p.token,{searchMode:null});
  assert.equal(checked,0);allowed=false;gate.resolve();await assert.rejects(query,/dispatch authority/);
  assert.equal(checked,1);assert.equal(f.parent.started.length,1);assert.equal(f.editor.getSearchCommandRequery(p.token),null);f.editor.close();
});

test('confirmed command passes confirmation receipt and final browser text to dispatch capture',async()=>{
  const f=await commandConfirmationFixture(),c=f.confirmation,confirmedToken={};
  f.parent.commitIndependentModeState=()=>({applied:true,stateToken:confirmedToken,userTypedValue:'actual mode text'});
  f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c));
  f.parent.captureIndependentQueryAuthority=(id,token,text)=>{
    assert.equal(token,confirmedToken);assert.equal(text,'actual mode text');return null;
  };
  await assert.rejects(f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:c.searchMode}),/browser state superseded/);
  assert.equal(f.editor.getSearchCommandRequery(f.proposal.token),null);f.editor.close();
});


test('ordinary confirmed and preview commands preserve mode without notifications and apply input once',async()=>{
  for(const preview of [false,true]) {
    const events=[],f=await editorFixture({browserId:42,onModeStateChanged:n=>events.push(n)});
    const mode={source:1,isPreview:preview,entry:'searchbutton'};
    await f.editor.edit({searchMode:mode},edit(1,'previous'));
    const p=await f.editor.beginSearchCommand(1,'ordinary words');
    f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    const preserve=f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});
    assert.equal(preserve.modeUnchanged,true);assert.ok(preserve.stateToken);assert.equal(events.length,0);
    const inputToken={};f.parent.commitIndependentInputState=(_id,text,facts)=>{
      assert.equal(facts.expectedStateToken,preserve.stateToken);assert.equal(text,'ordinary words');return {applied:true,current:true,stateToken:inputToken};
    };
    f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
    const request=f.editor.getSearchCommandRequery(p.token);
    if(preview){
      const c=f.editor.beginSearchCommandConfirmation(p.token,commandEntry());assert.ok(c);
      f.parent.commitIndependentModeState=(id,confirmed,facts,current,notify)=>{
        assert.equal(facts.expectedStateToken,inputToken);assert.equal(confirmed.isPreview,false);
        notify({browserId:id,searchMode:confirmed});return {applied:true,stateToken:{},userTypedValue:'ordinary words'};
      };
      assert.equal(f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c)).applied,true);assert.equal(events.length,1);
    }else assert.equal(f.editor.beginSearchCommandConfirmation(p.token,commandEntry()),null);
    const q=await f.editor.requeryAfterSearchCommand(p.token,{searchMode:request.searchMode});
    assert.equal(q.searchMode.isPreview,preview);assert.equal(q.searchString,'ordinary words');f.editor.close();
  }
});

test('ordinary engine preservation pins identity for input, confirmation and dispatch',async()=>{
  for(const phase of ['input','confirmation','dispatch']) {
    const f=await editorFixture({browserId:42}),engine={id:'e',name:'Engine',isGeneralPurposeEngine:true,aliases:[]};
    f.parent.child.updateEngineStore('changed',engine,-1);
    await f.editor.edit({searchMode:{engineName:'Engine',isPreview:true,entry:'searchbutton',isGeneralPurposeEngine:true,source:3}},edit(1,'old'));
    const p=await f.editor.beginSearchCommand(1,'new words');f.editor.acknowledgeSearchCommand(p.token,observedCommand(p));
    const preservation=f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});assert.equal(preservation.engineId,'e');
    const replace=()=>f.parent.child.updateEngineStore('changed',{...engine,id:'replacement'},0);
    let inputs=0;f.parent.commitIndependentInputState=()=>{inputs++;return {applied:true,current:true,stateToken:{}};};
    if(phase==='input')replace();
    const input=f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
    if(phase==='input'){assert.equal(input,null);assert.equal(inputs,0);f.editor.close();continue;}
    const c=f.editor.beginSearchCommandConfirmation(p.token,commandEntry());let confirmations=0;
    f.parent.commitIndependentModeState=()=>{confirmations++;return {applied:true,stateToken:{},userTypedValue:'new words'};};
    if(phase==='confirmation')replace();
    const confirmed=f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c));
    if(phase==='confirmation'){assert.equal(confirmed,null);assert.equal(confirmations,0);}
    else {replace();await assert.rejects(f.editor.requeryAfterSearchCommand(p.token,{searchMode:c.searchMode}),/dispatch authority/);}
    f.editor.close();
  }
});

test('parent preservation grants receipt only for matching owned mode, persistence and browser lifetime',()=>{
  for(const change of ['none','mode','persistence','typed','document']) {
    const {p,browser,lazy}=parentLoadFixture(),token={};browser.userTypedValue='unchanged';let committed=0;
    lazy.independentUrlbarBrowserState={preparePreservedMode:()=>{
      if(change==='mode')return null;if(change==='typed')browser.userTypedValue='new';
      if(change==='document')browser.browsingContext.currentWindowGlobal={};
      return {token,effects:{searchMode:null,persistedSearchTerms:change==='persistence'}};
    },commit:()=>{committed++;return true;},discard:()=>{}};
    const result=p.commitIndependentModePreservation(42,null,false,()=>true);
    assert.equal(result?.applied??false,change==='none');assert.equal(committed,change==='none'?1:0);
    if(result){assert.equal(result.modeUnchanged,true);assert.equal(result.stateToken,token);assert.equal(Object.isFrozen(result),true);}
    assert.equal(browser.userTypedValue,change==='typed'?'new':'unchanged');
  }
});


test('query start effects run once before engine-held queries settle or dispatch',async()=>{
  for(const allowed of [true,false]) {
    const f=fixture({hold:true}),events=[];
    const pending=f.session.query({}, {searchString:'held'},null,()=>allowed,q=>events.push(q.searchString));
    assert.deepEqual(events,['held']);assert.equal(f.parent.started.length,0);
    f.parent.child.updateEngineStore('init',[],null);
    if(allowed)await pending;else await assert.rejects(pending,/dispatch authority/);
    assert.deepEqual(events,['held']);assert.equal(f.parent.started.length,allowed?1:0);f.session.close();
  }
});

test('denied or synchronously superseded query starts do not run stale effects',async()=>{
  const f=fixture();await f.parent.child.engineStore.init();let effects=0;
  await assert.rejects(f.session.query({}, {searchString:'denied'},null,()=>false,()=>effects++),/dispatch authority/);
  assert.equal(effects,0);let next;
  f.parent.startQuery=q=>{f.parent.started.push(q);if(q.searchString==='old')next=f.session.query({}, {searchString:'new'});return Promise.resolve(q);};
  await assert.rejects(f.session.query({}, {searchString:'old'},null,null,()=>effects++),/superseded/);
  await next;assert.equal(effects,0);f.session.close();
});

test('query start effect errors preserve the running promise and cancellation does not repeat them',async()=>{
  const f=fixture({hold:true}),errors=[];f.sandbox.console={error:e=>errors.push(e)};let calls=0;
  const pending=f.session.query({}, {searchString:'held'},null,null,()=>{calls++;throw Error('counter failure');});
  assert.equal(calls,1);assert.equal(errors.length,1);f.session.cancel();f.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(pending,/cancelled/);assert.equal(calls,1);
  const q=await f.session.query({}, {searchString:'new'});
  await assert.rejects(f.session.query({}, {searchString:'bad'},null,null,1),/Invalid query start/);
  assert.equal(f.session.ownsQuery(q.id),true);f.session.close();
});

async function pickedMenuFixture(key,isConfigEngine=false) {
  const f=await editorFixture({browserId:42});await f.editor.edit({},edit(1,'words'));
  f.parent.child.updateEngineStore('changed',{id:'engine',name:'Engine',aliases:[],isGeneralPurposeEngine:true,isConfigEngine},-1);
  const p=await f.editor.beginSearchModeMenuCommand(1,key,{pageProxyValid:false,modeEntry:commandEntry()});
  f.editor.acknowledgeSearchCommand(p.token,observedOwnedCommand(p));
  f.parent.commitIndependentModeState=()=>({applied:true,stateToken:{},userTypedValue:'words'});
  f.parent.commitIndependentInputState=()=>({applied:true,current:true,stateToken:{}});
  f.editor.commitAcknowledgedSearchCommandMode(p.token,{persistedSearchTerms:false});
  f.editor.commitAcknowledgedSearchCommandInput(p.token,observedCommandInput(p));
  const c=f.editor.beginSearchCommandConfirmation(p.token,commandEntry());
  f.editor.commitSearchCommandConfirmation(c.token,observedConfirmation(c));
  return {...f,proposal:p,confirmation:c};
}

test('committed menu queries record shared local, built-in and add-on classifications once',async()=>{
  for(const [key,builtin,expected] of [['local:^',false,'local_search'],['engine:engine',true,'builtin_search'],['engine:engine',false,'addon_search']]) {
    const f=await pickedMenuFixture(key,builtin),picked=[];
    f.parent.recordSearchModeMenuPicked=kind=>{picked.push(kind);return true;};
    await f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:f.confirmation.searchMode});
    assert.deepEqual(picked,[expected]);
    await assert.rejects(f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:f.confirmation.searchMode}),/Stale/);
    assert.equal(picked.length,1);f.editor.close();
  }
});

test('rejected menu dispatch cannot record a pick',async()=>{
  const f=await pickedMenuFixture('local:^');let picked=0;
  f.parent.captureIndependentQueryAuthority=()=>()=>false;
  f.parent.recordSearchModeMenuPicked=()=>{picked++;};
  await assert.rejects(f.editor.requeryAfterSearchCommand(f.proposal.token,{searchMode:f.confirmation.searchMode}),/dispatch authority/);
  assert.equal(picked,0);f.editor.close();
});

test('menu picked backend validates query categories and matches address-bar SAP gating',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarParentController.sys.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  recordSearchModeMenuPicked(kind) {');
  const method=source.slice(start,source.indexOf('  /**',start));
  const calls=[],errors=[];let throws=false;
  const picked=new Proxy({}, {get:(_t,kind)=>({add:n=>{if(throws)throw Error('Glean failed');calls.push([kind,n]);}})});
  const Parent=new Function('Glean','console',`return class {#destroyed=false;#isAddressbar;constructor(addressbar){this.#isAddressbar=addressbar;}destroy(){this.#destroyed=true;}${method}}`)
    ({urlbarUnifiedsearchbutton:{picked}},{error:e=>errors.push(e)});
  const p=new Parent(true);
  for(const kind of ['local_search','builtin_search','addon_search'])assert.equal(p.recordSearchModeMenuPicked(kind),true);
  assert.deepEqual(calls,[['local_search',1],['builtin_search',1],['addon_search',1]]);
  assert.throws(()=>p.recordSearchModeMenuPicked('settings'),/Invalid/);
  assert.equal(new Parent(false).recordSearchModeMenuPicked('local_search'),false);
  throws=true;assert.equal(p.recordSearchModeMenuPicked('local_search'),false);assert.equal(errors.length,1);
  p.destroy();assert.equal(p.recordSearchModeMenuPicked('local_search'),false);
});


function observedInputBackend(f) {
  const commits=[],captures=[];
  f.parent.commitIndependentEditState=(id,text,mode,prior,current)=>{
    if(!current())return null;
    const outcome={applied:true,current:true,stateToken:{},userTypedValue:text,persistedSearchTerms:false,persistenceChanged:true};
    commits.push({id,text,mode,prior,outcome});return outcome;
  };
  f.parent.captureIndependentQueryAuthority=(id,token,text)=>{captures.push({id,token,text});return ()=>true;};
  return {commits,captures};
}

test('observed typing commits input before query and exposes immutable browser effects',async()=>{
  const f=await editorFixture({browserId:42}),{commits,captures}=observedInputBackend(f);
  const q=await f.editor.edit({},edit(1,'new words'),{userTypedValue:null});
  assert.equal(q.searchString,'new words');assert.equal(commits.length,1);assert.equal(commits[0].prior,null);
  assert.equal(captures[0].token,commits[0].outcome.stateToken);assert.equal(captures[0].text,'new words');
  assert.equal(f.editor.inputState.revision,1);assert.equal(f.editor.inputState.persistenceChanged,true);
  assert.equal(Object.isFrozen(f.editor.inputState),true);assert.equal(f.parent.child.userSelectionBehavior,'none');
  await assert.rejects(f.editor.edit({},edit(1,'repeat'),{userTypedValue:'new words'}),/stale/);
  assert.equal(commits.length,1);f.editor.close();
});

test('composition updates browser input without dispatch and its committed edit starts one query',async()=>{
  const f=await editorFixture({browserId:42}),{commits}=observedInputBackend(f);
  const mode={source:1,isPreview:true};
  assert.equal(await f.editor.edit({searchMode:mode},{...edit(1,'composing'),composing:true},{userTypedValue:'prior'}),null);
  assert.equal(f.parent.started.length,0);assert.equal(f.editor.inputState.userTypedValue,'composing');
  const q=await f.editor.edit({searchMode:mode},edit(2,'committed'),{userTypedValue:'composing'});
  assert.equal(commits.length,2);assert.deepEqual({...commits[1].mode},mode);
  assert.equal(q.searchMode.isPreview,true);assert.equal(f.parent.started.length,1);f.editor.close();
});

test('invalid browser observations preserve the current edit and denied commits cannot dispatch',async()=>{
  const f=await editorFixture({browserId:42}),q=await f.editor.edit({},edit(1,'old'));
  await assert.rejects(f.editor.edit({},edit(2,'new'),{}),/Invalid observed/);
  assert.equal(f.editor.snapshot.revision,1);assert.equal(f.editor.inputState,null);
  f.parent.commitIndependentEditState=()=>null;
  await assert.rejects(f.editor.edit({},edit(2,'new'),{userTypedValue:null}),/commit is stale/);
  assert.equal(f.parent.started.length,1);assert.equal(f.parent.started[0],q);f.editor.close();
});

test('reentrant observed input commits cannot overwrite or cancel a newer editor query',async()=>{
  const f=await editorFixture({browserId:42});let newer;
  f.parent.commitIndependentEditState=()=>{
    newer=f.editor.edit({},edit(2,'newer'));
    return {applied:true,current:true,stateToken:{},userTypedValue:'old'};
  };
  await assert.rejects(f.editor.edit({},edit(1,'old'),{userTypedValue:null}),/commit is stale/);
  await newer;assert.equal(f.editor.snapshot.text,'newer');assert.equal(f.editor.inputState,null);
  assert.equal(f.parent.started.length,1);assert.equal(f.parent.started[0].searchString,'newer');f.editor.close();
});

test('observed input dispatch checks committed browser authority after engine readiness',async()=>{
  const f=fixture({hold:true,browserId:42});f.session.close();
  const editor=new f.sandbox.UrlbarEditSession({context:f.context,isCurrent:()=>true,onUpdate:()=>{}});
  f.parent=f.parents.at(-1);observedInputBackend(f);let allowed=true;
  f.parent.captureIndependentQueryAuthority=()=>()=>allowed;
  const pending=editor.edit({},edit(1,'waiting'),{userTypedValue:null});
  assert.equal(editor.inputState.userTypedValue,'waiting');allowed=false;
  f.parent.child.updateEngineStore('init',[],null);
  await assert.rejects(pending,/dispatch authority/);assert.equal(f.parent.started.length,0);editor.close();
});

test('parent observed edit validates browser scope and consumes state before typed setter reentry',()=>{
  for(const change of ['none','prior','mode','document','setter']) {
    const {p,browser,lazy}=parentLoadFixture(),token={};let committed=false,text='prior';
    Object.defineProperty(browser,'userTypedValue',{get:()=>text,set:value=>{text=value;if(change==='setter')browser.lastLocationChange++;},configurable:true});
    lazy.independentUrlbarBrowserState={prepareEditInput:()=>{
      if(change==='mode')return null;if(change==='document')browser.browsingContext.currentWindowGlobal={};
      return {token,effects:{userTypedValue:'new',persistedSearchTerms:false,persistenceChanged:true}};
    },commit:()=>{committed=true;return true;},isCommitted:()=>committed,discard:()=>{}};
    const result=p.commitIndependentEditState(42,'new',null,change==='prior'?'wrong':'prior',()=>true);
    assert.equal(!!result,['none','setter'].includes(change));
    assert.equal(committed,['none','setter'].includes(change));
    if(result)assert.equal(result.current,change==='none');
  }
});


test('cancel callback replacement cannot attach an older observed input to the new revision',async()=>{
  const f=await editorFixture({browserId:42});await f.editor.edit({},edit(1,'initial'));
  const {commits}=observedInputBackend(f);let newer,replaced=false;
  f.parent.cancelQuery=()=>{if(!replaced){replaced=true;newer=f.editor.edit({},edit(3,'newest'));}};
  await assert.rejects(f.editor.edit({},edit(2,'obsolete'),{userTypedValue:null}),/superseded/);
  await newer;assert.equal(commits.length,0);assert.equal(f.editor.snapshot.revision,3);
  assert.equal(f.parent.started.at(-1).searchString,'newest');f.editor.close();
});


test('independent identity describes typed and composing snapshots but never stale revisions',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,' exact '));
  const d=f.editor.describeInputIdentity(1,{pageProxyValid:true,lastValidURL:'other'});
  assert.equal(d.revision,1);assert.equal(d.userTypedValue,' exact ');assert.equal(d.invalidatePageProxy,true);
  assert.equal(d.userSelectionBehavior,'none');assert.equal(Object.isFrozen(d),true);
  assert.equal(f.editor.describeInputIdentity(0,{}),null);
  await f.editor.edit({}, {...edit(2,''),composing:true});
  const c=f.editor.describeInputIdentity(2,{pageProxyValid:false});
  assert.equal(c.userTyping,false);assert.equal(c.pageProxyValid,false);
  assert.equal(f.editor.describeInputIdentity(1,{}),null);f.editor.close();
});

test('completion and selection changes revoke observed-input identity descriptions',async()=>{
  const f=await completionFixture();
  assert.ok(f.editor.describeInputIdentity(1,{pageProxyValid:false}));
  assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,{text:f.proposal.value,
    selectionStart:f.proposal.selectionStart,selectionEnd:f.proposal.selectionEnd,selectionDirection:'backward'}),true);
  assert.equal(f.editor.describeInputIdentity(1,{pageProxyValid:false}),null);
  await f.editor.edit({},edit(2,'next'));f.editor.select(2,0,0);
  assert.equal(f.editor.describeInputIdentity(2,{pageProxyValid:false}),null);f.editor.close();
});


test('explicit composition input uses shared query and autofill gates for every transition',async()=>{
  for(const keep of [false,true]) for(const name of ['NONE','COMPOSING','COMMIT','CANCELED']) for(const closedPopup of [false,true]) {
    const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=keep;
    const composition={state:f.sandbox.UrlbarShared.COMPOSITION[name],closedPopup};
    const q=await f.editor.edit({}, {...edit(1,'words'),composing:name==='COMPOSING',composition});
    const allowed=keep || (name!=='COMPOSING' && (name!=='CANCELED' || closedPopup));
    assert.equal(!!q,allowed,`${keep}/${name}/${closedPopup}`);
    if(q)assert.equal(q.allowAutofill,!(keep && name==='COMPOSING'));
    assert.equal(Object.isFrozen(f.editor.snapshot.composition),true);f.editor.close();
  }
});

test('invalid composition observations cannot cancel or replace a valid edit',async()=>{
  const f=await editorFixture(),q=await f.editor.edit({},edit(1,'current'));
  for(const composition of [{state:0,closedPopup:false},{state:1,closedPopup:1},{state:2,closedPopup:false}]) {
    await assert.rejects(f.editor.edit({}, {...edit(2,'invalid'),composition}),/Invalid composition/);
    assert.equal(f.editor.snapshot.revision,1);assert.equal(f.parent.started.length,1);assert.equal(f.parent.started[0],q);
  }
  f.editor.close();
});

test('explicit canceled input still commits browser text when shared policy suppresses its query',async()=>{
  const f=await editorFixture({browserId:42}),{commits}=observedInputBackend(f);
  const composition={state:f.sandbox.UrlbarShared.COMPOSITION.CANCELED,closedPopup:false};
  assert.equal(await f.editor.edit({}, {...edit(1,'canceled edit'),composition},{userTypedValue:'old'}),null);
  assert.equal(commits.length,1);assert.equal(f.editor.inputState.userTypedValue,'canceled edit');
  assert.equal(f.parent.started.length,0);f.editor.close();
});

test('keep-open composing queries never propose placeholder or provider autofill',async()=>{
  const f=await completionFixture();f.editor.acknowledgeCompletion(f.proposal.token,{text:f.proposal.value,
    selectionStart:f.proposal.selectionStart,selectionEnd:f.proposal.selectionEnd,selectionDirection:'backward'});
  f.prefs.keepPanelOpenDuringImeComposition=true;
  const q=await f.editor.edit({}, {...edit(2,'exa'),composing:true,
    composition:{state:f.sandbox.UrlbarShared.COMPOSITION.COMPOSING,closedPopup:false}});
  assert.equal(q.allowAutofill,false);assert.equal(f.updates.filter(u=>u.edit.revision===2 && u.completion).length,0);
  const result={autofill:{value:'example.test/',selectionStart:3,selectionEnd:13},payload:{url:'https://example.test/'}};
  q.results=[result];f.parent.child.notify('onFirstResult',q);
  assert.equal(f.updates.at(-1).completion,null);
  assert.throws(()=>f.editor.beginResultSelection(2,q.id,result),/Stale/);f.editor.close();
});

test('composition facts and preference are captured before asynchronous query completion',async()=>{
  const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  const composition={state:f.sandbox.UrlbarShared.COMPOSITION.COMPOSING,closedPopup:false};
  const pending=f.editor.edit({}, {...edit(1,'words'),composing:true,composition});
  composition.state=f.sandbox.UrlbarShared.COMPOSITION.NONE;composition.closedPopup=true;
  f.prefs.keepPanelOpenDuringImeComposition=false;
  const q=await pending;assert.equal(q.allowAutofill,false);
  assert.equal(f.editor.snapshot.composition.state,f.sandbox.UrlbarShared.COMPOSITION.COMPOSING);
  assert.equal(f.editor.snapshot.composition.closedPopup,false);f.editor.close();
});

test('legacy composing observations retain suppression even with keep-panel-open preference',async()=>{
  const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  assert.equal(await f.editor.edit({}, {...edit(1,'words'),composing:true}),null);
  assert.equal(f.parent.started.length,0);f.editor.close();
});


async function composingExposureFixture() {
  const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  const composition={state:f.sandbox.UrlbarShared.COMPOSITION.COMPOSING,closedPopup:false};
  const q=await f.editor.edit({}, {...edit(1,'composing exposure'),composing:true,composition});
  const result={id:901,payload:{}};q.results=[result];return {...f,q,result,composition};
}

test('keep-open composition exposes only observed current rows and can withdraw them',async()=>{
  const f=await composingExposureFixture();
  f.editor.publishPresentation(1,f.q.id,[f.result],true);
  assert.equal(f.parent.child.visibleResults[0],f.result);assert.equal(f.parent.child.resultsOpen,true);
  f.editor.publishPresentation(1,f.q.id,[],false);
  assert.equal(f.parent.child.visibleResults.length,0);assert.equal(f.parent.child.resultsOpen,false);f.editor.close();
});

test('composition exposure rejects copied, duplicate, stale and superseded rows',async()=>{
  const f=await composingExposureFixture();
  for(const rows of [[{...f.result}],[f.result,f.result]])assert.throws(()=>f.editor.publishPresentation(1,f.q.id,rows,true),/Invalid/);
  const newer=await f.editor.edit({}, {...edit(2,'new composition'),composing:true,composition:f.composition});
  assert.throws(()=>f.editor.publishPresentation(1,f.q.id,[f.result],true),/Stale/);
  assert.throws(()=>f.editor.publishPresentation(2,f.q.id,[f.result],true),/Stale/);
  assert.throws(()=>f.editor.publishPresentation(2,newer.id,[f.result],true),/Invalid/);f.editor.close();
});

test('legacy and disabled composing edits cannot inherit prior exposure permission',async()=>{
  for(const legacy of [true,false]) {
    const f=await composingExposureFixture();f.editor.publishPresentation(1,f.q.id,[f.result],true);
    f.prefs.keepPanelOpenDuringImeComposition=false;
    await f.editor.edit({}, {...edit(2,'blocked'),composing:true,...(legacy?{}:{composition:f.composition})});
    assert.equal(f.parent.child.visibleResults.length,0);
    assert.throws(()=>f.editor.publishPresentation(2,f.q.id,[f.result],true),/Stale/);f.editor.close();
  }
});

test('composing caret observations retain read-only exposure but never grant result actions',async()=>{
  const f=await composingExposureFixture();f.editor.select(1,0,0);
  f.editor.publishPresentation(1,f.q.id,[f.result],true);
  assert.throws(()=>f.editor.beginResultSelection(1,f.q.id,f.result),/Stale/);
  assert.throws(()=>f.editor.freezeResults(1,f.q.id),/Stale/);
  assert.throws(()=>f.editor.describeResultNavigation(1,f.q.id,f.result,{}),/Stale/);
  assert.equal(f.editor.getCompletion({}),null);f.editor.close();
});

test('current composition removals revoke exposure and reach the presenter once',async()=>{
  const f=await composingExposureFixture();f.editor.publishPresentation(1,f.q.id,[f.result],true);
  f.parent.child.notify('onQueryResultRemoved',f.result.id,undefined,f.q.id);
  assert.equal(f.parent.child.visibleResults.length,0);assert.equal(f.requests.length,1);
  assert.equal(f.requests[0].kind,'remove-result');assert.equal(f.requests[0].edit,f.editor.snapshot);
  assert.equal(Object.isFrozen(f.requests[0]),true);
  f.parent.child.notify('onQueryResultRemoved',f.result.id,undefined,f.q.id);
  assert.equal(f.requests.length,1);assert.throws(()=>f.editor.publishPresentation(1,f.q.id,[f.result],true),/Invalid/);
  f.editor.close();
});

test('cancel, close and scope loss revoke composition exposure',async()=>{
  for(const action of ['cancel','close','scope']) {
    const f=await composingExposureFixture();f.editor.publishPresentation(1,f.q.id,[f.result],true);
    if(action==='scope')f.loseScope();else f.editor[action]();
    assert.throws(()=>f.editor.publishPresentation(1,f.q.id,[f.result],true));
    assert.equal(f.parent.child.visibleResults.length,0);f.editor.close();
  }
});


test('provider close revokes composing exposure immediately and preserves preview mode and text',async()=>{
  const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  const mode={source:1,isPreview:true};
  const q=await f.editor.edit({searchMode:mode},{...edit(1,'composing'),composing:true,
    composition:{state:f.sandbox.UrlbarShared.COMPOSITION.COMPOSING,closedPopup:false}});
  const result={id:42,payload:{}};q.results=[result];f.editor.publishPresentation(1,q.id,[result],true);
  const observed=f.editor.snapshot;
  f.parent.child.notify('onResultsCloseRequested',q.id,true);
  assert.equal(f.parent.child.visibleResults.length,0);assert.equal(f.parent.child.resultsOpen,false);
  const close=f.requests.at(-1);assert.equal(close.kind,'close');assert.equal(close.edit,observed);
  assert.equal('searchMode' in close,false);assert.equal('userTypedValue' in close,false);
  assert.equal(f.editor.snapshot,observed);assert.equal(f.editor.getCloseRequest(close.token),close);
  assert.equal(f.editor.acknowledgeClose(close.token,{open:false,searchMode:mode,userTypedValue:'composing'}),true);
  assert.equal(f.editor.acknowledgeClose(close.token,{open:false}),false);
  assert.throws(()=>f.editor.publishPresentation(1,q.id,[result],true),/Stale/);f.editor.close();
});


test('independent composition start descriptions match owned mode, text and live preference',async()=>{
  const f=await editorFixture(),mode={source:1,isPreview:true};
  await f.editor.edit({searchMode:mode},edit(1,''));const observed=f.editor.snapshot,starts=f.parent.started.length;
  const p=f.editor.describeCompositionStart(1,{viewIsOpen:true});
  assert.equal(p.closePopup,true);assert.equal(p.confirmSearchMode,true);assert.equal(p.clearUserTypedValue,true);
  assert.equal(p.edit,observed);assert.equal(Object.isFrozen(p),true);
  assert.equal(f.editor.snapshot,observed);assert.equal(f.parent.started.length,starts);
  f.prefs.keepPanelOpenDuringImeComposition=true;
  const kept=f.editor.describeCompositionStart(1,{viewIsOpen:true});
  assert.equal(kept.closePopup,false);assert.equal(kept.confirmSearchMode,false);assert.equal(kept.clearUserTypedValue,false);
  assert.equal(p.closePopup,true);f.editor.close();
});

test('independent composition start descriptions reject stale, composing and invalid observations',async()=>{
  const f=await editorFixture();assert.equal(f.editor.describeCompositionStart(0,{viewIsOpen:true}),null);
  await f.editor.edit({},edit(1,'words'));
  assert.equal(f.editor.describeCompositionStart(0,{viewIsOpen:true}),null);
  assert.throws(()=>f.editor.describeCompositionStart(1,{}),/Invalid/);
  const closed=f.editor.describeCompositionStart(1,{viewIsOpen:false});assert.equal(closed.closePopup,false);
  await f.editor.edit({}, {...edit(2,'composing'),composing:true});
  assert.equal(f.editor.describeCompositionStart(2,{viewIsOpen:true}),null);f.editor.close();
});


const observedCompositionStart=p=>({text:p.edit.text,selectionStart:p.edit.selectionStart,
  selectionEnd:p.edit.selectionEnd,selectionDirection:p.edit.selectionDirection,composing:true,
  searchMode:p.searchMode,userTypedValue:p.userTypedValue,open:p.open,oneOffSelected:p.clearOneOffSelection?false:undefined});

test('composition start requests confirm preview and empty typed reset only after exact host acknowledgment',async()=>{
  const f=await editorFixture(),mode={source:1,isPreview:true};
  const q=await f.editor.edit({searchMode:mode},edit(1,''));q.results=[{id:1,payload:{}}];
  f.editor.publishPresentation(1,q.id,q.results,true);
  const request=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'previous',
    modeEntry:{untrimmedValue:'',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'previous'}});
  assert.equal(request.searchMode.isPreview,false);assert.equal(request.userTypedValue,'');assert.equal(request.open,false);
  assert.equal(f.editor.snapshot.composing,true);assert.equal(f.parent.child.visibleResults.length,1);
  const observed=observedCompositionStart(request);
  for(const wrong of [{open:true},{text:'wrong'},{selectionStart:1},{composing:false},{searchMode:mode},{userTypedValue:'previous'},{userTypedValue:null},{oneOffSelected:true},{oneOffSelected:undefined}]) {
    assert.equal(f.editor.acknowledgeCompositionStart(request.token,{...observed,...wrong}),false);
    assert.equal(f.editor.getCompositionStart(request.token),request);
  }
  assert.equal(f.editor.acknowledgeCompositionStart(request.token,observed),true);
  assert.equal(f.parent.child.visibleResults.length,0);assert.equal(f.parent.child.resultsOpen,false);
  assert.equal(f.editor.acknowledgeCompositionStart(request.token,observed),false);f.editor.close();
});

test('keep-open composition start retains query exposure while revoking pending and late autofill writes',async()=>{
  const f=await completionFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  const request=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'exa'});
  assert.equal(request.closePopup,false);assert.equal(request.confirmSearchMode,false);
  assert.equal(f.editor.getCompletion(f.proposal.token),null);
  f.parent.child.notify('onFirstResult',f.query);
  assert.equal(f.updates.at(-1).completion,null);
  f.editor.publishPresentation(1,f.query.id,[f.result],true);
  assert.equal(f.editor.acknowledgeCompositionStart(request.token,observedCompositionStart(request)),true);
  assert.equal(f.parent.child.visibleResults[0],f.result);
  assert.throws(()=>f.editor.beginResultSelection(1,f.query.id,f.result),/Stale/);f.editor.close();
});

test('composition start acknowledgment expires on preference change without later resurrection',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'text'));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:null});
  f.prefs.keepPanelOpenDuringImeComposition=true;assert.equal(f.editor.getCompositionStart(p.token),null);
  f.prefs.keepPanelOpenDuringImeComposition=false;assert.equal(f.editor.getCompositionStart(p.token),null);f.editor.close();
});

test('composition start acknowledgment cannot survive edit, caret, cancellation or scope changes',async()=>{
  for(const action of ['edit','caret','cancel','scope']) {
    const f=await editorFixture();await f.editor.edit({},edit(1,'text'));
    const p=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:'text'});
    if(action==='edit')await f.editor.edit({},edit(2,'new'));
    else if(action==='caret')f.editor.select(1,0,0);
    else if(action==='scope')f.loseScope();else f.editor.cancel();
    if(action==='scope')assert.throws(()=>f.editor.getCompositionStart(p.token),/scope/);
    else assert.equal(f.editor.getCompositionStart(p.token),null);f.editor.close();
  }
});

test('composition close acknowledgment consumes before cancellation callback replacement',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'text'));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'text'});let newer,reentered=false;
  f.parent.cancelQuery=()=>{if(!reentered){reentered=true;newer=f.editor.edit({},edit(2,'new'));}};
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),false);
  await newer;assert.equal(f.editor.snapshot.text,'new');assert.equal(f.editor.getCompositionStart(p.token),null);f.editor.close();
});

test('invalid and repeated composition starts cannot replace an admitted request',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'text'));const before=f.editor.snapshot;
  assert.throws(()=>f.editor.beginCompositionStart(1,{viewIsOpen:true}),/Invalid composition typed/);
  assert.equal(f.editor.snapshot,before);
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:null});
  assert.equal(f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:null}),null);
  assert.equal(f.editor.getCompositionStart(p.token),p);assert.equal(Object.isFrozen(p),true);f.editor.close();
});


test('keep-open composition can start after query cancellation without inventing exposure authority',async()=>{
  const f=await editorFixture();const q=await f.editor.edit({}, {...edit(1,'text'),
    composition:{state:f.sandbox.UrlbarShared.COMPOSITION.NONE,closedPopup:false}});f.editor.cancel();
  f.prefs.keepPanelOpenDuringImeComposition=true;
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:'text'});
  assert.ok(p);assert.equal(p.open,false);assert.equal('composition' in p.edit,false);
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
  assert.throws(()=>f.editor.publishPresentation(1,q.id,[],true),/Stale/);f.editor.close();
});


test('shared confirmation policy invokes mode and one-off effects only for preview modes',()=>{
  const f=fixture(),M=f.sandbox.UrlbarSearchModeModel;
  for(const mode of [null,{source:1,isPreview:false},{source:1,isPreview:true}]) {
    const p=M.confirmationPolicy(mode);assert.equal(p.applyMode,!!mode?.isPreview);
    assert.equal(p.clearOneOffSelection,p.applyMode);assert.equal(Object.isFrozen(p),true);
    if(mode){assert.equal(p.searchMode.isPreview,false);assert.equal(Object.isFrozen(p.searchMode),true);}
    else assert.equal(p.searchMode,null);
  }
  f.session.close();
});

test('both retained confirmation bindings preserve setter then one-off order and skip confirmed modes',()=>{
  const f=fixture();
  for(const name of ['UrlbarInputBase','SmartbarInput']) for(const preview of [false,true]) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${name}.mjs`,import.meta.url),'utf8');
    const start=source.indexOf('  confirmSearchMode() {'),method=source.slice(start,source.indexOf('\n  }',start)+4);
    const Input=new Function('UrlbarSearchModeModel',`return class {${method}}`)(f.sandbox.UrlbarSearchModeModel);
    const input=new Input(),events=[];let mode={source:1,isPreview:preview};
    Object.defineProperty(input,'searchMode',{get:()=>mode,set:value=>{events.push('setter');mode=value;}});
    const buttons={set selectedButton(value){assert.equal(value,null);events.push('one-off');}};
    Object.defineProperty(input,'view',{get(){if(!preview)throw Error('confirmed mode read view');return {oneOffSearchButtons:buttons};}});
    input.confirmSearchMode();assert.deepEqual(events,preview?['setter','one-off']:[]);assert.equal(mode.isPreview,false);
  }
  f.session.close();
});

test('composition preview requests require stable pre-mode facts before changing editor',async()=>{
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'text'));
  const before=f.editor.snapshot,options={viewIsOpen:true,userTypedValue:'prior'};
  assert.throws(()=>f.editor.beginCompositionStart(1,options),/Missing composition/);
  for(const facts of [{untrimmedValue:'other'},{userTypedValue:'changed'}]) {
    assert.equal(f.editor.beginCompositionStart(1,{...options,modeEntry:{untrimmedValue:'text',pageProxyValid:false,
      hasAutofillPlaceholder:false,userTypedValue:'prior',...facts}}),null);assert.equal(f.editor.snapshot,before);
  }
  const p=f.editor.beginCompositionStart(1,{...options,modeEntry:{untrimmedValue:'text',pageProxyValid:false,
    hasAutofillPlaceholder:false,userTypedValue:'prior'}});
  assert.equal(p.userTypedValue,'text');assert.equal(p.applyMode,true);assert.equal(p.clearOneOffSelection,true);
  assert.equal(Object.isFrozen(p.modeEntry),true);f.editor.close();
});

test('already-confirmed empty composition clears typed value without requesting another setter or one-off reset',async()=>{
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1,isPreview:false}},edit(1,''));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'prior'});
  assert.equal(p.confirmSearchMode,true);assert.equal(p.clearUserTypedValue,true);
  assert.equal(p.applyMode,false);assert.equal(p.clearOneOffSelection,false);assert.equal(p.userTypedValue,null);
  assert.equal('modeEntry' in p,false);assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);f.editor.close();
});


function compositionParentFixture(preview=true,text='',prior='old') {
  const f=parentLoadFixture(),writes=[];let typed=prior,mode={source:1,isPreview:preview},pending,last,latest;
  Object.defineProperty(f.browser,'userTypedValue',{get:()=>typed,set:value=>{writes.push(value);typed=value;f.onWrite?.(value);},configurable:true});
  const prepare=(next,effects)=>{const token=latest={};pending={token,next};return {token,effects};};
  const owner={preparePreservedMode:(_b,observed)=>{
    if(!f.lazy.UrlbarShared.deepEqual(observed,mode))return null;
    const prepared=prepare(mode,{searchMode:mode,persistedSearchTerms:false});f.onPrepare?.();return prepared;
  },prepareMode:(_b,next)=>prepare(next,{searchMode:next,persistedSearchTerms:false,persistenceChanged:false,recordModeEntry:!!mode?.isPreview && !next?.isPreview}),
  commit:(_b,token)=>{if(pending?.token!==token)return false;mode=pending.next;last=token;pending=null;return true;},
  discard:(_b,token)=>{if(pending?.token===token)pending=null;},isCommitted:(_b,token)=>last===token && latest===token};
  f.lazy.independentUrlbarBrowserState=owner;
  const facts={previousMode:{...mode},searchMode:{...mode,isPreview:false},text,priorTypedValue:prior,
    applyMode:preview,clearUserTypedValue:text==='',modeEntry:preview?{untrimmedValue:text,userTypedValue:prior,pageProxyValid:false,hasAutofillPlaceholder:false}:undefined,
    engineId:null,keepPanelOpen:false,viewIsOpen:true,previousPersistedSearchTerms:false,persistedSearchTerms:false};
  return Object.assign(f,{writes,owner,facts,getMode:()=>mode});
}

test('composition browser commit orders null reset before preview confirmation and reports final text',()=>{
  const f=compositionParentFixture(),notifications=[];
  const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true,state=>notifications.push(state));
  assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(result.typedResetApplied,true);
  assert.deepEqual(f.writes,[null,'']);assert.equal(f.browser.userTypedValue,'');assert.equal(f.getMode().isPreview,false);
  assert.equal(result.modeEntryNotified,true);assert.equal(notifications.length,1);assert.equal(notifications[0].userTypedValue,'');
});

test('confirmed and keep-open composition commits preserve mode without repeated setter effects',()=>{
  for(const keep of [false,true]) {
    const f=compositionParentFixture(false);f.facts.keepPanelOpen=keep;
    if(keep)f.facts.clearUserTypedValue=false;
    const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true,()=>assert.fail('preservation notification'));
    assert.equal(result.applied,true);assert.equal(result.modeUnchanged,true);assert.equal(result.current,true);
    assert.deepEqual(f.writes,keep?[]:[null]);assert.equal(f.calls.length,0);
  }
});

test('composition commit rejects stale browser, owned mode and prior persistence before writes',()=>{
  for(const change of ['typed','mode','persistence','document','policy']) {
    const f=compositionParentFixture();
    if(change==='typed')f.facts.priorTypedValue='wrong';
    if(change==='mode')f.facts.previousMode={source:2,isPreview:true};
    if(change==='persistence')f.facts.previousPersistedSearchTerms=true;
    if(change==='document')f.onPrepare=()=>{f.browser.browsingContext.currentWindowGlobal={};};
    if(change==='policy')f.facts.clearUserTypedValue=false;
    assert.equal(f.p.commitIndependentCompositionStart(42,f.facts,()=>true),null);
    assert.deepEqual(f.writes,[]);assert.equal(f.calls.length,0);
  }
});

test('composition null-reset reentry reports an applied reset without confirming a stale mode',()=>{
  for(const change of ['document','receipt']) {
    const f=compositionParentFixture();f.onWrite=()=>{
      if(change==='document')f.browser.lastLocationChange++;
      else {const p=f.owner.prepareMode(f.browser,{source:2,isPreview:true});f.owner.discard(f.browser,p.token);}
    };
    const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true);
    assert.equal(result.applied,false);assert.equal(result.typedResetApplied,true);assert.equal(result.current,false);
    assert.deepEqual(f.writes,[null]);assert.equal(f.getMode().isPreview,true);assert.equal(f.calls.length,0);
  }
});

test('failed post-mode persistence observation cannot hide an already applied composition reset',()=>{
  const f=compositionParentFixture();f.facts.persistedSearchTerms=true;
  const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true);
  assert.equal(result.applied,false);assert.equal(result.typedResetApplied,true);
  assert.deepEqual(f.writes,[null]);assert.equal(f.getMode().isPreview,true);assert.equal(f.calls.length,0);
});

test('acknowledged composition browser commit is one-use, correlated and never starts another query',async()=>{
  const notifications=[],f=await editorFixture({browserId:42,onModeStateChanged:n=>notifications.push(n)});
  await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,''));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'old',
    modeEntry:{untrimmedValue:'',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'old'}});
  const observation={previousPersistedSearchTerms:false,persistedSearchTerms:false};let calls=0;
  f.parent.commitIndependentCompositionStart=(id,facts,current,notify)=>{
    assert.equal(id,42);assert.equal(facts.priorTypedValue,'old');assert.equal(facts.modeEntry.userTypedValue,'old');
    assert.equal(current(),true);calls++;notify({browserId:id,userTypedValue:'',searchMode:facts.searchMode});
    return {applied:true,current:true,typedResetApplied:true};
  };
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,observation),null);
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,observation).applied,true);
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,observation),null);
  assert.equal(calls,1);assert.equal(f.parent.started.length,1);assert.equal(notifications.length,1);
  assert.equal(notifications[0].reason,'composition-start');assert.equal(notifications[0].token,p.token);
  assert.equal(Object.isFrozen(notifications[0]),true);f.editor.close();
});

test('composition engine replacement revokes both pending acknowledgment and consumed commit',async()=>{
  for(const phase of ['ack','commit']) {
    const f=await editorFixture({browserId:42}),engine={id:'e',name:'Engine',isGeneralPurposeEngine:true,aliases:[]};
    f.parent.child.updateEngineStore('changed',engine,-1);
    await f.editor.edit({searchMode:{engineName:'Engine',isPreview:true,source:3,isGeneralPurposeEngine:true}},edit(1,'text'));
    const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:'text',
      modeEntry:{untrimmedValue:'text',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'text'}});
    if(phase==='commit')assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
    f.parent.child.updateEngineStore('changed',{...engine,id:'replacement'},0);
    let calls=0;f.parent.commitIndependentCompositionStart=()=>{calls++;return {applied:true};};
    if(phase==='ack')assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),false);
    assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,{previousPersistedSearchTerms:false,persistedSearchTerms:false}),null);
    assert.equal(calls,0);f.editor.close();
  }
});

test('composition browser commit cannot reuse ready state after caret or preference changes',async()=>{
  for(const change of ['caret','preference']) {
    const f=await editorFixture({browserId:42});await f.editor.edit({},edit(1,'text'));
    const p=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:null});
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
    if(change==='caret')f.editor.select(1,0,0);else f.prefs.keepPanelOpenDuringImeComposition=true;
    f.parent.commitIndependentCompositionStart=()=>assert.fail('stale browser commit');
    const observation={previousPersistedSearchTerms:false,persistedSearchTerms:false};
    assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,observation),null);
    assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,observation),null);f.editor.close();
  }
});


function observedCompositionEnd(p) {
  return {...p.edit,composing:false,selectionCleared:true,resultForCurrentValue:null};
}

test('composition end acknowledgment pins text and caret, requests empty reopen, and never queries',async()=>{
  const f=await editorFixture();await f.editor.edit({}, {...edit(1,'text'),composing:true});
  const before=f.editor.snapshot;
  const p=f.editor.beginCompositionEnd(1,{hasData:false,hadText:false,closedPopup:true});
  assert.equal(p.reopen,true);assert.equal(p.resetSearchState,false);assert.equal(p.clearSelection,true);
  assert.equal(p.nextInput.state,f.sandbox.UrlbarShared.COMPOSITION.NONE);assert.equal(p.nextInput.closedPopup,false);
  assert.equal(f.editor.snapshot,before);assert.equal(Object.isFrozen(p),true);
  for(const wrong of [{text:'other'},{selectionStart:0},{composing:true},{selectionCleared:false},{resultForCurrentValue:{}}]) {
    assert.equal(f.editor.acknowledgeCompositionEnd(p.token,{...observedCompositionEnd(p),...wrong}),false);
    assert.equal(f.editor.snapshot,before);assert.equal(f.editor.getCompositionEnd(p.token),p);
  }
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),true);
  assert.equal(f.editor.snapshot.composing,false);assert.equal('composition' in f.editor.snapshot,false);
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),false);
  assert.equal(f.parent.started.length,0);f.editor.close();
});

test('composition end preserves keep-open query and does not require selection-clear effects',async()=>{
  const f=await editorFixture();f.prefs.keepPanelOpenDuringImeComposition=true;
  const S=f.sandbox.UrlbarShared.COMPOSITION;
  const query=await f.editor.edit({}, {...edit(1,'text'),composing:true,composition:{state:S.COMPOSING,closedPopup:false}});
  const p=f.editor.beginCompositionEnd(1,{hasData:true,hadText:true,closedPopup:false});
  assert.equal(p.clearSelection,false);assert.equal(p.clearResultForCurrentValue,false);assert.equal(p.reopen,false);
  assert.equal(p.nextInput.state,S.COMMIT);
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,{...p.edit,composing:false}),true);
  f.editor.publishPresentation(1,query.id,[],true);
  assert.equal(f.parent.started.length,1);assert.equal('composition' in f.editor.snapshot,false);
  // The following real input, rather than the end event, consumes COMMIT.
  await f.editor.edit({}, {...edit(2,'text'),composition:p.nextInput});
  assert.equal(f.parent.started.length,2);f.editor.close();
});

test('composition end requests cannot survive caret, input, cancellation or observed preference changes',async()=>{
  for(const change of ['caret','input','cancel','preference']) {
    const f=await editorFixture();await f.editor.edit({}, {...edit(1,'text'),composing:true});
    const p=f.editor.beginCompositionEnd(1,{hasData:false,hadText:true,closedPopup:true});
    if(change==='caret')f.editor.select(1,0,0);
    if(change==='input')await f.editor.edit({},edit(2,'new'));
    if(change==='cancel')f.editor.cancel();
    if(change==='preference')f.prefs.keepPanelOpenDuringImeComposition=true;
    assert.equal(f.editor.getCompositionEnd(p.token),null);
    if(change==='preference')f.prefs.keepPanelOpenDuringImeComposition=false;
    assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),false);f.editor.close();
  }
});

test('end preparation rejects invalid history and outstanding start effects without mutating observation',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'text'));
  assert.equal(f.editor.beginCompositionEnd(1,{hasData:true,hadText:true,closedPopup:false}),null);
  const start=f.editor.beginCompositionStart(1,{viewIsOpen:false,userTypedValue:null});
  assert.equal(f.editor.beginCompositionEnd(1,{hasData:true,hadText:true,closedPopup:false}),null);
  assert.equal(f.editor.acknowledgeCompositionStart(start.token,observedCompositionStart(start)),true);
  assert.equal(f.editor.beginCompositionEnd(1,{hasData:true,hadText:true,closedPopup:false}),null);
  await f.editor.edit({}, {...edit(2,'text'),composing:true});const before=f.editor.snapshot;
  assert.throws(()=>f.editor.beginCompositionEnd(2,{hasData:true,hadText:null,closedPopup:false}),/Invalid composition end/);
  assert.equal(f.editor.snapshot,before);f.editor.close();
});

test('a newer composition end preparation revokes its predecessor without ending composition',async()=>{
  const f=await editorFixture();await f.editor.edit({}, {...edit(1,''),composing:true});
  const a=f.editor.beginCompositionEnd(1,{hasData:false,hadText:true,closedPopup:true});
  const b=f.editor.beginCompositionEnd(1,{hasData:true,hadText:true,closedPopup:true});
  assert.equal(a.reopen,false);assert.equal(f.editor.getCompositionEnd(a.token),null);
  assert.equal(f.editor.acknowledgeCompositionEnd(a.token,observedCompositionEnd(a)),false);
  assert.equal(f.editor.snapshot.composing,true);
  assert.equal(f.editor.acknowledgeCompositionEnd(b.token,observedCompositionEnd(b)),true);f.editor.close();
});


async function compositionReopenFixture(mode=null, text='retained', engine=null) {
  const f=await editorFixture({browserId:42});
  if(engine)f.parent.child.updateEngineStore('changed',engine,-1);
  await f.editor.edit({searchMode:mode}, {...edit(1,text),composing:true});
  const proposal=f.editor.beginCompositionEnd(1,{hasData:false,hadText:false,closedPopup:true});
  assert.equal(f.editor.acknowledgeCompositionEnd(proposal.token,observedCompositionEnd(proposal)),true);
  const receipt={},events=[];let preserves=0;
  f.parent.commitIndependentModePreservation=(id,observed,persisted,current)=>{
    assert.equal(id,42);assert.equal(JSON.stringify(observed),JSON.stringify(mode));assert.equal(persisted,false);
    assert.equal(current(),true);preserves++;return {applied:true,stateToken:receipt};
  };
  f.parent.captureIndependentQueryAuthority=(id,token,value)=>{
    assert.equal(id,42);assert.equal(token,receipt);assert.equal(value,null);return ()=>true;
  };
  f.parent.child.engagementEvent.start=(event,query,text)=>events.push({event,query,text});
  return {...f,proposal,events,preserves:()=>preserves,
    observation:{pageProxyValid:false,userTypedValue:null,persistedSearchTerms:false,
      event:{type:'compositionend',data:'',timeStamp:12}}};
}

test('empty composition reopens once without input, mode reset or typed-value writes',async()=>{
  const mode={source:1,isPreview:false},f=await compositionReopenFixture(mode);
  const before=f.editor.snapshot;
  const query=await f.editor.reopenAfterCompositionEnd(f.proposal.token,{searchMode:mode},f.observation);
  assert.equal(query.searchString,'retained');assert.equal(query.allowAutofill,true);
  assert.equal(f.editor.snapshot,before);assert.equal(f.editor.inputState,null);assert.equal(f.preserves(),1);
  assert.equal(f.events.length,1);assert.equal(f.events[0].event.type,'compositionend');
  assert.equal(Object.isFrozen(f.events[0].event),true);assert.equal(f.events[0].query,query);
  assert.equal(f.parent.started.length,1);
  assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{searchMode:mode},f.observation),null);
  f.editor.close();
});

test('valid-page composition reopening queries zero-prefix without changing the displayed address',async()=>{
  const f=await compositionReopenFixture(null,'https://example.test/');
  const query=await f.editor.reopenAfterCompositionEnd(f.proposal.token,{}, {...f.observation,pageProxyValid:true});
  assert.equal(query.searchString,'');assert.equal(f.editor.snapshot.text,'https://example.test/');
  assert.equal(f.events[0].text,'');f.editor.close();
});

test('reopen rejects malformed facts before consumption and mismatched or preview mode after consumption',async()=>{
  const f=await compositionReopenFixture();
  for(const change of [{pageProxyValid:null},{userTypedValue:4},{persistedSearchTerms:null},
    {event:{type:'input',timeStamp:12,data:''}},{event:{type:'compositionend',timeStamp:12,data:'commit'}}]) {
    await assert.rejects(f.editor.reopenAfterCompositionEnd(f.proposal.token,{}, {...f.observation,...change}),/Invalid composition reopen/);
  }
  assert.equal(f.preserves(),0);
  assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{searchMode:{source:1}},f.observation),null);
  assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation),null);f.editor.close();
  const preview=await compositionReopenFixture({source:1,isPreview:true});
  assert.equal(await preview.editor.reopenAfterCompositionEnd(preview.proposal.token,{searchMode:{source:1,isPreview:true}},preview.observation),null);
  assert.equal(preview.preserves(),0);preview.editor.close();
});

test('caret, input, preference and cancellation revoke acknowledged empty reopen',async()=>{
  for(const change of ['caret','input','preference','cancel']) {
    const f=await compositionReopenFixture();
    if(change==='caret')f.editor.select(1,0,0);
    if(change==='input')await f.editor.edit({},edit(2,'new'));
    if(change==='preference')f.prefs.keepPanelOpenDuringImeComposition=true;
    if(change==='cancel')f.editor.cancel();
    assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation),null);
    assert.equal(f.preserves(),0);f.editor.close();
  }
});

test('reopen browser-state failure consumes authority without an engagement update or query',async()=>{
  for(const phase of ['preserve','capture']) {
    const f=await compositionReopenFixture();
    if(phase==='preserve')f.parent.commitIndependentModePreservation=()=>null;
    else f.parent.captureIndependentQueryAuthority=()=>null;
    assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation),null);
    assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation),null);
    assert.equal(f.events.length,0);assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('engine-held reopen rechecks browser authority and cancellation immediately before dispatch',async()=>{
  for(const change of ['browser','caret','cancel']) {
    const f=await compositionReopenFixture();let allowed=true;
    f.parent.captureIndependentQueryAuthority=()=>()=>allowed;
    const store=f.parent.child.engineStore,gate=Promise.withResolvers();
    Object.defineProperty(store,'initialized',{value:false});store.init=()=>gate.promise;
    const pending=f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation);
    assert.equal(f.events.length,1);assert.equal(f.parent.started.length,0);
    if(change==='browser')allowed=false;if(change==='caret')f.editor.select(1,0,0);if(change==='cancel')f.editor.cancel();
    gate.resolve();await assert.rejects(pending,/dispatch authority|superseded|cancelled/i);
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('engagement reentry cannot dispatch a superseded composition reopen',async()=>{
  const f=await compositionReopenFixture();
  f.parent.child.engagementEvent.start=()=>f.editor.cancel();
  await assert.rejects(f.editor.reopenAfterCompositionEnd(f.proposal.token,{},f.observation),/superseded|cancelled/i);
  assert.equal(f.parent.started.length,0);f.editor.close();
});


test('engine replacement revokes captured composition reopen identity before and during dispatch',async()=>{
  for(const held of [false,true]) {
    const engine={id:'engine',name:'Engine',isGeneralPurposeEngine:true,aliases:[]};
    const mode={engineName:'Engine',isGeneralPurposeEngine:true,source:3,isPreview:false};
    const f=await compositionReopenFixture(mode,'retained',engine);
    const replace=()=>f.parent.child.updateEngineStore('changed',{...engine,id:'replacement'},0);
    if(!held) {
      replace();assert.equal(await f.editor.reopenAfterCompositionEnd(f.proposal.token,{searchMode:mode},f.observation),null);
      assert.equal(f.preserves(),0);
    } else {
      const store=f.parent.child.engineStore,gate=Promise.withResolvers();
      Object.defineProperty(store,'initialized',{value:false,configurable:true});store.init=()=>gate.promise;
      const pending=f.editor.reopenAfterCompositionEnd(f.proposal.token,{searchMode:mode},f.observation);
      Object.defineProperty(store,'initialized',{value:true});replace();gate.resolve();await assert.rejects(pending,/dispatch authority/);
    }
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});


test('valid-page preview confirmation proposes an exact empty editor write without applying it',async()=>{
  const f=await editorFixture();await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'https://example.test/'));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:null,
    modeEntry:{untrimmedValue:'https://example.test/',userTypedValue:null,pageProxyValid:true,hasAutofillPlaceholder:false}});
  assert.equal(p.editorWrite.text,'');assert.equal(p.editorWrite.selectionStart,0);assert.equal(p.editorWrite.selectionEnd,0);
  assert.equal(p.editorWrite.pageProxyValid,false);assert.equal(Object.isFrozen(p.editorWrite),true);
  assert.equal(p.clearUserTypedValue,false);assert.equal(p.userTypedValue,'');
  assert.equal(f.editor.snapshot.text,'https://example.test/');assert.equal(f.editor.snapshot.composing,true);
  const observation={...observedCompositionStart(p),...p.editorWrite};
  for(const wrong of [{text:p.edit.text},{selectionEnd:1},{selectionDirection:'backward'},{pageProxyValid:true}]) {
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observation,...wrong}),false);
    assert.equal(f.editor.snapshot.text,p.edit.text);
  }
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),true);
  assert.equal(f.editor.snapshot.text,'');assert.equal(f.editor.snapshot.revision,1);
  assert.equal(f.editor.snapshot.selectionRevision,1);assert.equal(f.editor.snapshot.composing,true);
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),false);f.editor.close();
});

test('valid-page browser confirmation uses original mode-entry facts and correlates the acknowledged edit',async()=>{
  const notifications=[],f=await editorFixture({browserId:42,onModeStateChanged:n=>notifications.push(n)});
  await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'page'));
  const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:null,
    modeEntry:{untrimmedValue:'page',userTypedValue:null,pageProxyValid:true,hasAutofillPlaceholder:false}});
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observedCompositionStart(p),...p.editorWrite}),true);
  const acknowledged=f.editor.snapshot;
  f.parent.commitIndependentCompositionStart=(id,facts,current,notify)=>{
    assert.equal(facts.text,'page');assert.equal(facts.modeEntry.untrimmedValue,'page');assert.equal(facts.modeEntry.pageProxyValid,true);
    assert.equal(current(),true);notify({browserId:id,userTypedValue:'',searchMode:facts.searchMode});return {applied:true,current:true};
  };
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,{previousPersistedSearchTerms:false,persistedSearchTerms:false}).applied,true);
  assert.equal(notifications[0].edit,acknowledged);assert.equal(notifications[0].edit.text,'');
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('valid-page parent confirmation clears browser text without an unnecessary null reset',()=>{
  const f=compositionParentFixture(true,'https://example.test/',null);f.facts.modeEntry.pageProxyValid=true;
  const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true);
  assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(result.typedResetApplied,false);
  assert.deepEqual(f.writes,['']);assert.equal(f.getMode().isPreview,false);assert.equal(f.browser.userTypedValue,'');
});

test('valid-page write acknowledgment rejects stale caret and a newer edit revokes its browser commit',async()=>{
  for(const phase of ['before','after']) {
    const f=await editorFixture({browserId:42});await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'page'));
    const p=f.editor.beginCompositionStart(1,{viewIsOpen:true,userTypedValue:null,
      modeEntry:{untrimmedValue:'page',userTypedValue:null,pageProxyValid:true,hasAutofillPlaceholder:false}});
    const observation={...observedCompositionStart(p),...p.editorWrite};
    if(phase==='before') {
      f.editor.select(1,0,0);assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),false);
    } else {
      assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),true);await f.editor.edit({},edit(2,'new'));
    }
    f.parent.commitIndependentCompositionStart=()=>assert.fail('stale composition write');
    assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,{previousPersistedSearchTerms:false,persistedSearchTerms:false}),null);f.editor.close();
  }
});


function compositionRestorationFixture(value='https://www.restored.test/') {
  const f=recoveryValueFixture('describeIndependentCompositionRestoration');f.browser.userTypedValue=value;
  f.facts={untrimmedValue:'autofilled',userTypedValue:value,pageProxyValid:false,hasAutofillPlaceholder:true};return f;
}

test('composition restoration uses Firefox normalization and reports full versus displayed value without writes',()=>{
  const f=compositionRestorationFixture();
  for(const trim of [false,true]) {
    f.prefs.trimURLs=trim;const d=f.p.describeIndependentCompositionRestoration(42,f.facts);
    assert.equal(d.value,trim?'restored.test':'https://www.restored.test/');assert.equal(d.untrimmedValue,'https://www.restored.test/');
    assert.equal(d.protocolIsTrimmed,trim);assert.equal(d.wwwIsTrimmed,trim);assert.equal(d.valueIsTyped,false);
    assert.equal(d.pageProxyValid,false);assert.equal(Object.isFrozen(d),true);assert.equal(f.browser.userTypedValue,f.facts.userTypedValue);
  }
  f.session.close();
});

test('composition restoration unwraps Reader Mode and preserves RTL display without asserting security',()=>{
  const f=compositionRestorationFixture('about:reader?url=https%3A%2F%2Fwww.restored.test%2F');
  f.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>({displaySpec:'https://www.restored.test/'});f.prefs.trimURLs=true;
  assert.equal(f.p.describeIndependentCompositionRestoration(42,f.facts).value,'restored.test');
  f.p.browserWindow.windowUtils.getDirectionFromText=()=>1;
  const d=f.p.describeIndependentCompositionRestoration(42,f.facts);assert.equal(d.value,'https://www.restored.test/');
  assert.equal(d.protocolIsTrimmed,false);assert.equal(d.wwwIsTrimmed,false);assert.equal(d.pageProxyValid,false);f.session.close();
});

test('composition restoration rejects stale browser facts and formatting reentry',()=>{
  for(const change of ['typed','selected','document','epoch','destroy']) {
    const f=compositionRestorationFixture();
    if(change==='typed')f.browser.userTypedValue='changed';
    if(change==='selected')f.p.browserWindow.gBrowser.selectedBrowser={};
    if(change==='destroy')f.p.destroy();
    if(change==='document' || change==='epoch')f.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>{
      if(change==='document')f.browser.browsingContext.currentWindowGlobal={};else f.browser.lastLocationChange++;return null;
    };
    assert.equal(f.p.describeIndependentCompositionRestoration(42,f.facts),null);f.session.close();
  }
});

test('composition restoration rejects malformed facts and skips absent, empty or page-clearing cases',()=>{
  const f=compositionRestorationFixture();
  for(const facts of [{...f.facts,userTypedValue:null},{...f.facts,userTypedValue:''},
    {...f.facts,hasAutofillPlaceholder:false},{...f.facts,pageProxyValid:true}]) {
    assert.equal(f.p.describeIndependentCompositionRestoration(42,facts),null);
  }
  assert.throws(()=>f.p.describeIndependentCompositionRestoration(42,{...f.facts,pageProxyValid:1}),/Invalid composition restoration/);
  assert.throws(()=>f.p.describeIndependentCompositionRestoration(0,f.facts),/Invalid composition restoration/);f.session.close();
});

test('editor restoration description is immutable, revocable and does not begin composition or query',async()=>{
  const f=await editorFixture({browserId:42});await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,'autofilled'));
  const before=f.editor.snapshot,facts={viewIsOpen:true,userTypedValue:'typed',modeEntry:{untrimmedValue:'autofilled',
    userTypedValue:'typed',pageProxyValid:false,hasAutofillPlaceholder:true}};
  let calls=0;f.parent.describeIndependentCompositionRestoration=(id,entry)=>{
    assert.equal(id,42);assert.equal(entry.userTypedValue,'typed');calls++;return Object.freeze({value:'typed',untrimmedValue:'typed'});
  };
  const d=f.editor.describeCompositionRestoration(1,facts);assert.equal(d.value,'typed');assert.equal(d.edit,before);
  assert.equal(Object.isFrozen(d),true);assert.equal(Object.isFrozen(d.modeEntry),true);assert.equal(f.editor.snapshot,before);
  assert.equal(f.parent.started.length,1);assert.equal('token' in d,false,'description grants no write token');
  f.prefs.keepPanelOpenDuringImeComposition=true;assert.equal(f.editor.describeCompositionRestoration(1,facts),null);assert.equal(calls,1);
  f.prefs.keepPanelOpenDuringImeComposition=false;f.parent.describeIndependentCompositionRestoration=()=>{f.editor.select(1,0,0);return d;};
  assert.equal(f.editor.describeCompositionRestoration(1,facts),null);f.editor.close();
});


function restorationCommitFixture(typed='https://www.restored.test/',text='autofilled') {
  const f=compositionParentFixture(true,text,typed),format=compositionRestorationFixture(typed);
  Object.assign(f.lazy,{UrlbarPrefs:format.lazy.UrlbarPrefs,BrowserUIUtils:format.lazy.BrowserUIUtils,
    ReaderMode:format.lazy.ReaderMode,trimDisplayValue:format.lazy.trimDisplayValue});
  f.p.browserWindow.windowUtils=format.p.browserWindow.windowUtils;
  f.facts.modeEntry.hasAutofillPlaceholder=true;format.session.close();
  return Object.assign(f,{prefs:format.prefs});
}

async function restorationEditorFixture(typed='https://www.restored.test/',text='autofilled') {
  const notifications=[],f=await editorFixture({browserId:42,onModeStateChanged:n=>notifications.push(n)});
  const browser=restorationCommitFixture(typed,text);browser.prefs.trimURLs=true;
  f.parent.describeIndependentCompositionRestoration=browser.p.describeIndependentCompositionRestoration.bind(browser.p);
  f.parent.commitIndependentCompositionStart=browser.p.commitIndependentCompositionStart.bind(browser.p);
  await f.editor.edit({searchMode:{source:1,isPreview:true}},edit(1,text));
  const facts={viewIsOpen:true,userTypedValue:typed,modeEntry:{...browser.facts.modeEntry}};
  return Object.assign(f,{browser,notifications,facts});
}
const compositionPersistence={previousPersistedSearchTerms:false,persistedSearchTerms:false};

test('restoration write requires exact display, full value, caret and metadata before a one-use browser commit',async()=>{
  const f=await restorationEditorFixture();f.editor.select(1,1,3,'backward');
  const p=f.editor.beginCompositionStart(1,f.facts),observation={...observedCompositionStart(p),...p.editorWrite};
  assert.equal(p.editorWrite.text,'restored.test');assert.equal(p.editorWrite.selectionStart,13);
  assert.equal(p.editorWrite.selectionEnd,13);assert.equal(p.editorWrite.selectionDirection,'none');
  assert.equal(Object.isFrozen(p.restoration.formatPolicy),true);assert.equal(f.browser.writes.length,0);
  assert.equal(f.editor.snapshot.text,'autofilled');
  for(const wrong of [{text:p.restoration.untrimmedValue},{untrimmedValue:p.restoration.value},
    {protocolIsTrimmed:false},{wwwIsTrimmed:false},{valueIsTyped:true},{pageProxyValid:true},
    {selectionEnd:0},{selectionDirection:'backward'},{userTypedValue:'restored.test'}]) {
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observation,...wrong}),false);
  }
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),true);
  const acknowledged=f.editor.snapshot;assert.equal(acknowledged.text,'restored.test');
  assert.equal(f.browser.writes.length,0,'host acknowledgment must not write browser state');
  const outcome=f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence);
  assert.equal(outcome.applied,true);assert.equal(outcome.current,true);assert.equal(outcome.typedResetApplied,false);
  assert.deepEqual(f.browser.writes,['https://www.restored.test/']);assert.equal(f.browser.getMode().isPreview,false);
  assert.equal(f.notifications.length,1);assert.equal(f.notifications[0].edit,acknowledged);
  assert.equal(f.notifications[0].userTypedValue,'https://www.restored.test/');
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observation),false);
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence),null);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('same-display restoration preserves the observed caret and direction',async()=>{
  const f=await restorationEditorFixture('autofilled');f.editor.select(1,1,3,'backward');
  const p=f.editor.beginCompositionStart(1,f.facts);
  assert.equal(p.editorWrite.selectionStart,1);assert.equal(p.editorWrite.selectionEnd,3);
  assert.equal(p.editorWrite.selectionDirection,'backward');
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observedCompositionStart(p),...p.editorWrite}),true);
  assert.equal(f.editor.snapshot.selectionStart,1);assert.equal(f.editor.snapshot.selectionDirection,'backward');f.editor.close();
});

test('restoration revalidation revokes writes after formatting, browser or edit changes',async()=>{
  for(const change of ['trim','typed','caret','formatReentry','preferenceReentry']) {
    const f=await restorationEditorFixture(),p=f.editor.beginCompositionStart(1,f.facts);
    if(change==='trim')f.browser.prefs.trimURLs=false;
    if(change==='typed')f.browser.browser.userTypedValue='newer';
    if(change==='caret')f.editor.select(1,0,0);
    if(change==='formatReentry')f.browser.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>{f.editor.select(1,0,0);return null;};
    if(change==='preferenceReentry')f.browser.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>{f.prefs.keepPanelOpenDuringImeComposition=true;return null;};
    assert.equal(f.editor.getCompositionStart(p.token),null);
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observedCompositionStart(p),...p.editorWrite}),false);
    assert.equal(f.editor.snapshot.text,'autofilled');f.editor.close();
  }
});

test('restoration formatting reentry cannot begin composition against a changed keep-open policy',async()=>{
  const f=await restorationEditorFixture(),before=f.editor.snapshot;
  f.browser.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>{f.prefs.keepPanelOpenDuringImeComposition=true;return null;};
  assert.equal(f.editor.beginCompositionStart(1,f.facts),null);assert.equal(f.editor.snapshot,before);f.editor.close();
});

test('restoration browser commit rechecks formatting and browser lifetime after host acknowledgment',async()=>{
  for(const change of ['trim','www','protocol','typed','document','prepareReentry']) {
    const f=await restorationEditorFixture(),p=f.editor.beginCompositionStart(1,f.facts);
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observedCompositionStart(p),...p.editorWrite}),true);
    if(change==='trim')f.browser.prefs.trimURLs=false;
    if(change==='www')f.browser.prefs.trimWww=true;
    if(change==='protocol')f.browser.lazy.BrowserUIUtils.trimURLProtocol='http://';
    if(change==='typed')f.browser.browser.userTypedValue='newer';
    if(change==='document')f.browser.onPrepare=()=>{f.browser.browser.browsingContext.currentWindowGlobal={};};
    if(change==='prepareReentry')f.browser.onPrepare=()=>{f.browser.prefs.trimURLs=false;};
    const writes=f.browser.writes.length;
    assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence),null);
    assert.equal(f.browser.writes.length,writes);assert.equal(f.notifications.length,0);f.editor.close();
  }
});

test('Reader Mode restoration commits the captured full value and exposes setter reentry without retry',()=>{
  for(const reenter of [false,true]) {
    const f=restorationCommitFixture('about:reader?url=https%3A%2F%2Fwww.restored.test%2F');f.prefs.trimURLs=true;
    f.lazy.ReaderMode.getOriginalUrlObjectForDisplay=()=>({displaySpec:'https://www.restored.test/'});
    f.facts.restoration=f.p.describeIndependentCompositionRestoration(42,f.facts.modeEntry);
    if(reenter)f.onWrite=()=>{f.browser.lastLocationChange++;};
    const result=f.p.commitIndependentCompositionStart(42,f.facts,()=>true);
    assert.equal(result.applied,true);assert.equal(result.current,!reenter);
    assert.deepEqual(f.writes,['https://www.restored.test/']);assert.equal(result.typedResetApplied,false);
  }
});


test('valid-page placeholder confirmation requests only the final empty display and exact cleared metadata',async()=>{
  for(const typed of [null,'','https://www.restored.test/','about:reader?url=https%3A%2F%2Frestored.test']) {
    const f=await restorationEditorFixture(typed,'page');f.facts.modeEntry.pageProxyValid=true;
    f.parent.describeIndependentCompositionRestoration=()=>assert.fail('page clear does not need restoration formatting');
    const p=f.editor.beginCompositionStart(1,f.facts),observed={...observedCompositionStart(p),...p.editorWrite};
    assert.equal(p.editorWrite.text,'');assert.equal(p.editorWrite.untrimmedValue,'');
    assert.equal(p.editorWrite.protocolIsTrimmed,false);assert.equal(p.editorWrite.wwwIsTrimmed,false);
    assert.equal(p.editorWrite.valueIsTyped,false);assert.equal('restoration' in p,false);
    assert.equal(f.editor.snapshot.text,'page');assert.equal(f.browser.writes.length,0);
    for(const wrong of [{untrimmedValue:'page'},{protocolIsTrimmed:true},{wwwIsTrimmed:true},
      {valueIsTyped:true},{untrimmedValue:undefined},{protocolIsTrimmed:undefined},{valueIsTyped:undefined}]) {
      assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observed,...wrong}),false);
    }
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,observed),true);
    const result=f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence);
    assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(result.typedResetApplied,false);
    assert.equal(f.editor.snapshot.text,'');assert.deepEqual(f.browser.writes,['']);
    assert.equal(f.notifications[0].edit,f.editor.snapshot);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('placeholder with empty typed state preserves nonempty editor text without requesting restoration',async()=>{
  for(const typed of [null,'']) {
    const f=await restorationEditorFixture(typed,'retained');
    f.parent.describeIndependentCompositionRestoration=()=>assert.fail('empty typed value cannot restore autofill');
    const p=f.editor.beginCompositionStart(1,f.facts);
    assert.equal(p.editorWrite,undefined);assert.equal(p.restoration,undefined);assert.equal(p.userTypedValue,'retained');
    assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
    const result=f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence);
    assert.equal(result.applied,true);assert.equal(result.typedResetApplied,false);
    assert.equal(f.editor.snapshot.text,'retained');assert.deepEqual(f.browser.writes,['retained']);f.editor.close();
  }
});

test('empty composition text resets prior typed state before confirming a placeholder mode',async()=>{
  const f=await restorationEditorFixture('prior typed','');
  f.parent.describeIndependentCompositionRestoration=()=>assert.fail('intermediate null reset suppresses restoration');
  const p=f.editor.beginCompositionStart(1,f.facts);
  assert.equal(p.clearUserTypedValue,true);assert.equal(p.editorWrite,undefined);assert.equal(p.userTypedValue,'');
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
  const result=f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence);
  assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(result.typedResetApplied,true);
  assert.deepEqual(f.browser.writes,[null,'']);assert.equal(f.editor.snapshot.text,'');f.editor.close();
});

test('non-restoring composition commits reject an injected restoration descriptor',()=>{
  for(const valid of [false,true]) {
    const f=compositionParentFixture(true,'page',null);f.facts.modeEntry.hasAutofillPlaceholder=true;
    f.facts.modeEntry.pageProxyValid=valid;f.facts.restoration={value:'injected',untrimmedValue:'injected'};
    assert.equal(f.p.commitIndependentCompositionStart(42,f.facts,()=>true),null);assert.deepEqual(f.writes,[]);
  }
});

test('page-clear acknowledgment cannot overwrite an edit installed by query cancellation',async()=>{
  const f=await restorationEditorFixture('prior','page');f.facts.modeEntry.pageProxyValid=true;
  const p=f.editor.beginCompositionStart(1,f.facts);let newer,reentered=false;
  f.parent.cancelQuery=()=>{if(!reentered){reentered=true;newer=f.editor.edit({},edit(2,'newer'));}};
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,{...observedCompositionStart(p),...p.editorWrite}),false);
  await newer;assert.equal(f.editor.snapshot.text,'newer');assert.equal(f.browser.writes.length,0);
  assert.equal(f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence),null);f.editor.close();
});


async function ownedCompositionFixture({keep=false,open=true}={}) {
  const f=await restorationEditorFixture('prior','text');f.prefs.keepPanelOpenDuringImeComposition=keep;
  f.facts.viewIsOpen=open;f.facts.modeEntry.hasAutofillPlaceholder=false;
  const start=f.editor.beginCompositionStart(1,{...f.facts,trackHistory:true});
  assert.equal(f.editor.acknowledgeCompositionStart(start.token,observedCompositionStart(start)),true);
  assert.equal(f.editor.commitAcknowledgedCompositionStart(start.token,compositionPersistence).current,true);
  return Object.assign(f,{start,state:{searchMode:f.browser.getMode()}});
}

test('owned composition end derives popup history and empty reopen only after acknowledged start commit',async()=>{
  for(const open of [false,true]) {
    const f=await ownedCompositionFixture({open});const p=f.editor.beginCompositionEnd(1,{hasData:false});
    assert.equal(p.reopen,open);assert.equal(p.nextInput.closedPopup,false);
    assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),true);
    assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),false);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('owned composing inputs remember real input data and consume cancellation on exactly one following input',async()=>{
  const f=await ownedCompositionFixture({open:false});
  assert.equal(await f.editor.edit(f.state,{...edit(2,'composed'),composing:true,compositionHasData:true}),null);
  const p=f.editor.beginCompositionEnd(2,{hasData:false});assert.equal(p.reopen,false);
  assert.equal(p.nextInput.state,f.sandbox.UrlbarShared.COMPOSITION.CANCELED);
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),true);
  assert.equal(await f.editor.edit(f.state,{...edit(3,'composed'),compositionHasData:false}),null);
  assert.equal(f.editor.snapshot.composition.state,f.sandbox.UrlbarShared.COMPOSITION.CANCELED);
  assert.ok(await f.editor.edit(f.state,{...edit(4,'composed'),compositionHasData:false}));
  assert.equal(f.editor.snapshot.composition.state,f.sandbox.UrlbarShared.COMPOSITION.NONE);
  assert.equal(f.parent.started.length,2);f.editor.close();
});

test('owned keep-open composition queries disable autofill and retain commit until the following input',async()=>{
  const f=await ownedCompositionFixture({keep:true});
  const q=await f.editor.edit(f.state,{...edit(2,'composed'),composing:true,compositionHasData:true});
  assert.equal(q.allowAutofill,false);const p=f.editor.beginCompositionEnd(2,{hasData:true});
  assert.equal(p.clearSelection,false);assert.equal(p.reopen,false);
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,{...p.edit,composing:false}),true);
  const committed=await f.editor.edit(f.state,{...edit(3,'composed'),compositionHasData:false});
  assert.equal(committed.allowAutofill,true);assert.equal(f.editor.snapshot.composition.state,f.sandbox.UrlbarShared.COMPOSITION.COMMIT);
  f.editor.close();
});

test('owned event history refuses injected history and malformed input without consuming valid history',async()=>{
  const f=await ownedCompositionFixture(),before=f.editor.snapshot;
  assert.throws(()=>f.editor.beginCompositionEnd(1,{hasData:false,hadText:false,closedPopup:true}),/cannot be overridden/);
  for(const change of [{revision:1},{selectionEnd:99},{compositionHasData:'yes'},{composing:false},
    {composition:{state:f.sandbox.UrlbarShared.COMPOSITION.COMPOSING,closedPopup:false}}]) {
    await assert.rejects(f.editor.edit(f.state,{...edit(2,'text'),composing:true,compositionHasData:true,...change}));
    assert.equal(f.editor.snapshot,before);
  }
  const p=f.editor.beginCompositionEnd(1,{hasData:false});assert.equal(p.reopen,true);
  assert.equal(f.editor.acknowledgeCompositionEnd(p.token,{...observedCompositionEnd(p),selectionCleared:false}),false);
  assert.equal(f.editor.getCompositionEnd(p.token),p);assert.equal(f.editor.acknowledgeCompositionEnd(p.token,observedCompositionEnd(p)),true);
  f.editor.close();
});

test('cancel and independently supplied input revoke owned composition history',async()=>{
  for(const change of ['cancel','input']) {
    const f=await ownedCompositionFixture();
    if(change==='cancel')f.editor.cancel();else await f.editor.edit(f.state,{...edit(2,'new'),composing:true});
    await assert.rejects(f.editor.edit(f.state,{...edit(3,'text'),composing:true,compositionHasData:true}),/owned composition/);
    f.editor.close();
  }
});

test('owned history is not published when start browser commit becomes stale during notification',async()=>{
  const f=await restorationEditorFixture('prior','text');f.facts.modeEntry.hasAutofillPlaceholder=false;
  const p=f.editor.beginCompositionStart(1,{...f.facts,trackHistory:true});
  assert.equal(f.editor.acknowledgeCompositionStart(p.token,observedCompositionStart(p)),true);
  f.parent.commitIndependentCompositionStart=()=>{f.editor.select(1,0,0);return {applied:true,current:true};};
  f.editor.commitAcknowledgedCompositionStart(p.token,compositionPersistence);
  await assert.rejects(f.editor.edit(f.state??{}, {...edit(2,'new'),composing:true,compositionHasData:true}),/owned composition/);
  f.editor.close();
});

test('query cancellation replacement cannot retain the superseded owned input history',async()=>{
  const f=await ownedCompositionFixture();let reentered=false,newer;
  f.parent.cancelQuery=()=>{if(!reentered){reentered=true;newer=f.editor.edit(f.state,edit(3,'newest'));}};
  await assert.rejects(f.editor.edit(f.state,{...edit(2,'old'),composing:true,compositionHasData:true}),/superseded/);
  await newer;assert.equal(f.editor.snapshot.text,'newest');
  await assert.rejects(f.editor.edit(f.state,{...edit(4,'new'),composing:true,compositionHasData:true}),/owned composition/);
  f.editor.close();
});


const observedInputIdentity=p=>({...p.edit,...p});

test('input identity acknowledgment requires exact host metadata and publishes a one-use immutable receipt',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
  const before=f.editor.snapshot,started=f.parent.started.length;
  const p=f.editor.beginInputIdentity(1,{pageProxyValid:true,lastValidURL:'loaded'});
  assert.equal(p.invalidatePageProxy,true);assert.equal(p.pageProxyValid,false);assert.equal(p.valueIsTyped,true);
  assert.equal('userTypedValue' in p,false);assert.equal('userSelectionBehavior' in p,false);
  assert.equal(Object.isFrozen(p),true);assert.equal(f.editor.inputIdentityState,null);
  const observation=observedInputIdentity(p);
  for(const wrong of [{text:'other'},{selectionStart:0},{selectionDirection:'backward'},{composing:true},
    {valueIsTyped:false},{untrimmedValue:'loaded'},{protocolIsTrimmed:true},{wwwIsTrimmed:true},
    {resultForCurrentValue:{}},{userTyping:false},{actionType:'switchtab'},{pageProxyValid:true},{lastValidURL:'changed'},
    {valueIsTyped:undefined}]) {
    assert.equal(f.editor.acknowledgeInputIdentity(p.token,{...observation,...wrong}),false);
    assert.equal(f.editor.getInputIdentity(p.token),p);assert.equal(f.editor.inputIdentityState,null);
  }
  assert.equal(f.editor.acknowledgeInputIdentity(p.token,observation),true);
  assert.equal(f.editor.inputIdentityState,p);assert.equal(f.editor.acknowledgeInputIdentity(p.token,observation),false);
  observation.untrimmedValue='mutated';assert.equal(f.editor.inputIdentityState.untrimmedValue,'typed');
  assert.equal(f.editor.snapshot,before);assert.equal(f.parent.started.length,started);f.editor.close();
});

test('input identity preserves a matching valid display and never promotes an invalid proxy',async()=>{
  for(const [text,valid,lastValid,expected] of [['shown',true,'shown',true],['shown',false,'shown',false],['',true,'',true],['',true,undefined,false]]) {
    const f=await editorFixture();await f.editor.edit({},edit(1,text));
    const p=f.editor.beginInputIdentity(1,{pageProxyValid:valid,lastValidURL:lastValid});
    assert.equal(p.pageProxyValid,expected);assert.equal(p.userTyping,!!text);
    assert.equal(f.editor.acknowledgeInputIdentity(p.token,observedInputIdentity(p)),true);f.editor.close();
  }
});

test('composition input identity can acknowledge typed metadata without querying',async()=>{
  const f=await editorFixture();await f.editor.edit({}, {...edit(1,'composing'),composing:true});
  const p=f.editor.beginInputIdentity(1,{pageProxyValid:true,lastValidURL:'loaded'});
  assert.equal(p.edit.composing,true);assert.equal(f.editor.acknowledgeInputIdentity(p.token,observedInputIdentity(p)),true);
  assert.equal(f.parent.started.length,0);assert.equal(f.editor.inputIdentityState.untrimmedValue,'composing');f.editor.close();
});

test('caret, edit, cancellation and scope changes revoke pending and acknowledged input identity',async()=>{
  for(const acknowledged of [false,true]) for(const change of ['caret','edit','cancel','scope']) {
    const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
    const p=f.editor.beginInputIdentity(1,{pageProxyValid:false});
    if(acknowledged)assert.equal(f.editor.acknowledgeInputIdentity(p.token,observedInputIdentity(p)),true);
    if(change==='caret')f.editor.select(1,0,0);
    if(change==='edit')await f.editor.edit({},edit(2,'new'));
    if(change==='cancel')f.editor.cancel();
    if(change==='scope')f.loseScope();
    assert.equal(f.editor.inputIdentityState,null);
    if(change==='scope')assert.throws(()=>f.editor.getInputIdentity(p.token),/closed/);
    else assert.equal(f.editor.getInputIdentity(p.token),null);
    f.editor.close();
  }
});

test('replacement requests and observation getter reentry cannot acknowledge an older identity',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));const facts={pageProxyValid:false};
  const first=f.editor.beginInputIdentity(1,facts),second=f.editor.beginInputIdentity(1,facts);
  assert.equal(f.editor.acknowledgeInputIdentity(first.token,observedInputIdentity(first)),false);
  let replacement;const observation=observedInputIdentity(second);
  Object.defineProperty(observation,'untrimmedValue',{get(){replacement=f.editor.beginInputIdentity(1,facts);return 'typed';}});
  assert.equal(f.editor.acknowledgeInputIdentity(second.token,observation),false);
  assert.equal(f.editor.getInputIdentity(replacement.token),replacement);assert.equal(f.editor.inputIdentityState,null);
  assert.equal(f.editor.acknowledgeInputIdentity(replacement.token,observedInputIdentity(replacement)),true);f.editor.close();
});

test('invalid facts and fact getter reentry cannot authorize identity for a replaced edit',async()=>{
  const f=await editorFixture();await f.editor.edit({},edit(1,'typed'));
  const p=f.editor.beginInputIdentity(1,{pageProxyValid:false});
  assert.throws(()=>f.editor.beginInputIdentity(1,{pageProxyValid:false,lastValidURL:{}}),/Invalid last valid/);
  assert.equal(f.editor.getInputIdentity(p.token),p);
  assert.equal(f.editor.beginInputIdentity(1,{pageProxyValid:false,get lastValidURL(){f.editor.select(1,0,0);return 'typed';}}),null);
  assert.equal(f.editor.inputIdentityState,null);f.editor.close();
});


test('autofill preview writes cannot retain or acquire observed-input identity authority',async()=>{
  const f=await completionFixture();
  const identity=f.editor.beginInputIdentity(1,{pageProxyValid:false});assert.ok(identity);
  assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),true);
  assert.equal(f.editor.getInputIdentity(identity.token),null);
  assert.equal(f.editor.beginInputIdentity(1,{pageProxyValid:false}),null);
  assert.equal(f.editor.inputIdentityState,null);f.editor.close();
});


async function deselectionFixture() {
  const f=await completionFixture({browserId:42});
  assert.equal(f.editor.acknowledgeCompletion(f.proposal.token,observed(f.proposal)),true);
  const commits=[];f.parent.commitIndependentEditState=(browserId,value,mode,prior,current)=>{
    commits.push({browserId,value,mode,prior});assert.equal(current(),true);
    return Object.freeze({applied:true,current:current(),userTypedValue:value,persistenceChanged:false,persistedSearchTerms:false,stateToken:{}});
  };
  return Object.assign(f,{commits});
}
const caret=(start,end=start,direction='none')=>({selectionStart:start,selectionEnd:end,selectionDirection:direction});

test('autofill deselection commits displayed text once without querying or resetting selection history',async()=>{
  const f=await deselectionFixture();f.parent.child.userSelectionBehavior='tab';
  const result=f.editor.observeAutofillSelection(1,caret(13),{userTypedValue:'exam'});
  assert.equal(result.confirmed,true);assert.equal(result.applied,true);assert.equal(result.current,true);
  assert.equal(result.edit,f.editor.snapshot);assert.equal(result.edit.text,'example.test/');
  assert.deepEqual(f.commits,[{browserId:42,value:'example.test/',mode:null,prior:'exam'}]);
  assert.equal(f.parent.child.userSelectionBehavior,'tab');assert.equal(f.parent.started.length,1);
  assert.equal(f.editor.observeAutofillSelection(1,caret(0),{userTypedValue:'example.test/'}).confirmed,false);
  assert.equal(f.commits.length,1);assert.equal(f.editor.beginInputIdentity(1,{pageProxyValid:false}),null);f.editor.close();
});

test('unchanged range and direction-only selection preserve the placeholder until actual deselection',async()=>{
  const f=await deselectionFixture();
  assert.equal(f.editor.observeAutofillSelection(1,caret(4,13,'forward'),{userTypedValue:'exam'}).confirmed,false);
  assert.equal(f.commits.length,0);
  assert.equal(f.editor.observeAutofillSelection(1,caret(3,13),{userTypedValue:'exam'}).confirmed,true);
  assert.equal(f.commits.length,1);f.editor.close();
});

test('invalid selection observations do not consume the placeholder or write browser state',async()=>{
  const f=await deselectionFixture(),before=f.editor.snapshot;
  assert.throws(()=>f.editor.observeAutofillSelection(1,caret(0),{}),/Invalid selection browser/);
  assert.throws(()=>f.editor.observeAutofillSelection(0,caret(0),{userTypedValue:'exam'}),/Stale/);
  assert.throws(()=>f.editor.observeAutofillSelection(1,caret(99),{userTypedValue:'exam'}),/Invalid editor selection/);
  assert.equal(f.editor.snapshot,before);assert.equal(f.commits.length,0);
  assert.equal(f.editor.observeAutofillSelection(1,caret(0),{userTypedValue:'exam'}).confirmed,true);f.editor.close();
});

test('a stale browser commit consumes the observed deselection without retrying its write',async()=>{
  const f=await deselectionFixture();let calls=0;
  f.parent.commitIndependentEditState=()=>{calls++;return null;};
  assert.throws(()=>f.editor.observeAutofillSelection(1,caret(0),{userTypedValue:'stale'}),/browser commit is stale/);
  assert.equal(f.editor.snapshot.selectionStart,0);
  assert.equal(f.editor.observeAutofillSelection(1,caret(1),{userTypedValue:'exam'}).confirmed,false);
  assert.equal(calls,1);f.editor.close();
});

test('browser-setter reentry reports the applied selection confirmation without overwriting newer caret state',async()=>{
  const f=await deselectionFixture();
  f.parent.commitIndependentEditState=(_id,value,_mode,_prior,current)=>{
    assert.equal(current(),true);f.editor.select(1,2,2);return {applied:true,current:current(),userTypedValue:value};
  };
  const result=f.editor.observeAutofillSelection(1,caret(13),{userTypedValue:'exam'});
  assert.equal(result.applied,true);assert.equal(result.current,false);assert.equal(f.editor.snapshot.selectionStart,2);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('unapplied speculative placeholder and ordinary selection never confirm browser text',async()=>{
  const f=await deselectionFixture();await f.editor.edit({},edit(2,'examp'));
  const before=f.parent.started.length;
  assert.equal(f.editor.observeAutofillSelection(2,caret(0),{userTypedValue:'examp'}).confirmed,false);
  assert.equal(f.commits.length,0);assert.equal(f.parent.started.length,before);f.editor.close();
});


const popupInput=(revision,text='',extra={})=>({revision,text,selectionStart:text.length,
  selectionEnd:text.length,presentation:{open:true,oneOffsHaveView:false},...extra});

test('empty observed input commits browser text but stops dispatch when the open popup must close',async()=>{
  const f=await editorFixture({browserId:42});f.prefs['suggest.topsites']=false;
  const calls=[];f.parent.commitIndependentEditState=(_id,value,_mode,prior,current)=>{
    assert.equal(current(),true);calls.push({value,prior});
    return {applied:true,current:true,stateToken:{},userTypedValue:value,persistedSearchTerms:false};};
  assert.equal(await f.editor.edit({},popupInput(1),{userTypedValue:'before'}),null);
  assert.deepEqual(calls,[{value:'',prior:'before'}]);assert.equal(f.parent.started.length,0);
  const plan=f.editor.inputPresentation;
  assert.equal(plan.edit,f.editor.snapshot);assert.equal(plan.stopQuery,true);
  assert.equal(plan.clearResults,true);assert.equal(plan.close,true);assert.equal(plan.removeAccessibleFocus,false);
  assert.equal(Object.isFrozen(plan),true);f.editor.close();
});

test('closed popup, Top Sites and mode with one-off view retain independent query dispatch',async()=>{
  for(const facts of [{open:false,oneOffsHaveView:false},{open:true,oneOffsHaveView:true},
    {open:true,oneOffsHaveView:false,topSites:true},{open:true,oneOffsHaveView:false,text:'query'}]) {
    const f=await editorFixture();f.prefs['suggest.topsites']=facts.topSites??false;
    const state=facts.oneOffsHaveView?{searchMode:{source:2}}:{};
    assert.ok(await f.editor.edit(state,popupInput(1,facts.text??'',{presentation:facts})));
    assert.equal(f.parent.started.length,1);assert.equal(f.editor.inputPresentation.close,false);
    assert.equal(f.editor.inputPresentation.removeAccessibleFocus,true);f.editor.close();
  }
  const f=await editorFixture();f.prefs['suggest.topsites']=false;
  assert.equal(await f.editor.edit({},popupInput(1,'',{presentation:{open:true,oneOffsHaveView:true}})),null);
  f.editor.close();
});

test('input presentation validates before mutation and revokes descriptors on replacement, caret and cancellation',async()=>{
  const f=await editorFixture();f.prefs['suggest.topsites']=false;
  await f.editor.edit({},popupInput(1));const plan=f.editor.inputPresentation;
  for(const presentation of [{},{open:0,oneOffsHaveView:false},{open:true,oneOffsHaveView:null}]) {
    await assert.rejects(f.editor.edit({},popupInput(2,'',{presentation})),/presentation observation/);
    assert.equal(f.editor.inputPresentation,plan);
  }
  f.editor.select(1,0,0);assert.equal(f.editor.inputPresentation,null);
  await f.editor.edit({},popupInput(2));f.editor.cancel();assert.equal(f.editor.inputPresentation,null);
  await f.editor.edit({},popupInput(3));f.loseScope();assert.equal(f.editor.inputPresentation,null);
});

test('composition still suppresses queries when input presentation permits dispatch',async()=>{
  const f=await editorFixture();f.prefs['suggest.topsites']=false;
  assert.equal(await f.editor.edit({},popupInput(1,'ime',{composing:true})),null);
  assert.equal(f.editor.inputPresentation.stopQuery,false);assert.equal(f.parent.started.length,0);
  await f.editor.edit({},popupInput(2,'ordinary',{presentation:null}));
  assert.equal(f.editor.inputPresentation,null);assert.equal(f.parent.started.length,1);f.editor.close();
});


const popupEffects={rollupCompleted:true,resultsCleared:true,open:false,accessibleFocusCleared:true};

test('popup effects require exact host observations and consume the request once without dispatch',async()=>{
  const f=await editorFixture();f.prefs['suggest.topsites']=false;
  await f.editor.edit({},popupInput(1));const request=f.editor.inputPresentation,observed={...f.editor.snapshot,...popupEffects};
  assert.equal(f.editor.getInputPresentation({}),null);
  for(const observation of [null,{...observed,open:true},{...observed,rollupCompleted:false},
    {...observed,resultsCleared:false},{...observed,open:0},{...observed,text:'pending edit'},{...observed,selectionEnd:1},{...observed,composing:true}]) {
    assert.equal(f.editor.acknowledgeInputPresentation(request.token,observation),false);
    assert.equal(f.editor.getInputPresentation(request.token),request);
  }
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,{...observed,accessibleFocusCleared:false}),true);
  assert.equal(f.editor.inputPresentation,null);assert.equal(f.editor.inputPresentationState,request);
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,observed),false);
  assert.equal(f.parent.started.length,0);f.editor.close();assert.equal(f.editor.inputPresentationState,null);
});

test('continuing popup effects require accessible-focus removal but do not require a close',async()=>{
  const f=await editorFixture();await f.editor.edit({},popupInput(1,'text'));
  const request=f.editor.inputPresentation,observed={...f.editor.snapshot,...popupEffects},queries=f.parent.started.length;
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,{...observed,accessibleFocusCleared:false}),false);
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,{...observed,resultsCleared:false,open:true}),true);
  assert.equal(f.editor.inputPresentationState,request);assert.equal(f.parent.started.length,queries);
  f.editor.select(1,0,0);assert.equal(f.editor.inputPresentationState,null);f.editor.close();
});

test('popup acknowledgments reject getter reentry and preserve a newer request',async()=>{
  const f=await editorFixture();f.prefs['suggest.topsites']=false;
  await f.editor.edit({},popupInput(1));const request=f.editor.inputPresentation,observed={...f.editor.snapshot,...popupEffects};
  let replacement;
  const observation={...observed,get resultsCleared(){replacement=f.editor.edit({},popupInput(2));return true;}};
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,observation),false);await replacement;
  const newer=f.editor.inputPresentation;assert.notEqual(newer,request);
  assert.equal(f.editor.inputPresentationState,null);
  assert.equal(f.editor.acknowledgeInputPresentation(newer.token,observed),true);
  f.editor.cancel();assert.equal(f.editor.inputPresentationState,null);f.editor.close();
});

test('popup receipt follows edit and scope lifetime and cannot be changed by caller observations',async()=>{
  const f=await editorFixture();await f.editor.edit({},popupInput(1,'text'));
  const request=f.editor.inputPresentation,observed={...f.editor.snapshot,...popupEffects},observation={...observed};
  assert.equal(f.editor.acknowledgeInputPresentation(request.token,observation),true);
  observation.open=true;assert.equal(f.editor.inputPresentationState,request);assert.equal(Object.isFrozen(request),true);
  await f.editor.edit({},popupInput(2,'new'));assert.equal(f.editor.inputPresentationState,null);
  const newer=f.editor.inputPresentation;f.loseScope();
  assert.equal(f.editor.acknowledgeInputPresentation(newer.token,observed),false);
  assert.equal(f.editor.inputPresentationState,null);
});


async function popupCloseFixture(options){
  const f=await editorFixture({browserId:42,...options});f.prefs['suggest.topsites']=false;
  await f.editor.edit({searchMode:{source:2,isPreview:true}},popupInput(1));
  f.request=f.editor.inputPresentation;f.observed={...f.editor.snapshot,...popupEffects};
  f.commits=[];f.parent.commitIndependentPreviewDismissal=(id,mode,facts,current)=>{
    assert.equal(current(),true);f.commits.push({id,mode,facts});
    return {applied:true,current:true,browserId:id,stateToken:{},searchMode:null,userTypedValue:null,persistedSearchTerms:false};};
  return f;
}
const closeFacts={userTypedValue:'',persistedSearchTerms:false};

test('preview close commits only after exact popup acknowledgment and notifies once without querying',async()=>{
  const events=[],f=await popupCloseFixture({onModeStateChanged:event=>events.push(event)});
  assert.equal(f.editor.commitInputPopupClose(f.request.token,closeFacts),null);
  assert.equal(f.editor.acknowledgeInputPresentation(f.request.token,f.observed),true);
  const result=f.editor.commitInputPopupClose(f.request.token,closeFacts);
  assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(result.userTypedValue,null);
  assert.equal(f.commits.length,1);assert.equal(f.commits[0].mode.isPreview,true);
  assert.equal(f.editor.inputPresentationState,null);assert.equal(f.editor.beginSearchModeExit(1),null);
  assert.equal(events.length,1);assert.equal(events[0].reason,'input-popup-preview-close');
  assert.equal(f.editor.commitInputPopupClose(f.request.token,closeFacts),null);
  assert.equal(f.parent.started.length,0);f.editor.close();
});

test('failed, canceled and reentrant preview-close commits cannot retry or overwrite a newer edit',async()=>{
  for(const phase of ['failure','caret','setter']){
    const f=await popupCloseFixture();f.editor.acknowledgeInputPresentation(f.request.token,f.observed);
    let calls=0;f.parent.commitIndependentPreviewDismissal=()=>{calls++;
      if(phase==='failure')return null;
      f.editor.select(1,0,0);return {applied:true,current:false};};
    if(phase==='caret')f.editor.select(1,0,0);
    const outcome=f.editor.commitInputPopupClose(f.request.token,closeFacts);
    assert.equal(calls,phase==='caret'?0:1);
    assert.equal(!!outcome,phase==='setter');if(outcome)assert.equal(outcome.current,false);
    assert.equal(f.editor.commitInputPopupClose(f.request.token,closeFacts),null);
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('preview-close notification reentry reports stale browser state after the applied commit',async()=>{
  let browserCurrent=true;const f=await popupCloseFixture({onModeStateChanged:()=>{browserCurrent=false;}});
  f.parent.captureIndependentQueryAuthority=()=>()=>browserCurrent;
  f.editor.acknowledgeInputPresentation(f.request.token,f.observed);
  const outcome=f.editor.commitInputPopupClose(f.request.token,closeFacts);
  assert.equal(outcome.applied,true);assert.equal(outcome.current,false);
  assert.equal('browserCurrent' in outcome,false);f.editor.close();
});

test('confirmed-mode close and continuing previews do not authorize preview dismissal',async()=>{
  for(const preview of [false,true]){
    const f=await editorFixture({browserId:42});f.prefs['suggest.topsites']=false;
    await f.editor.edit({searchMode:{source:2,isPreview:preview}},popupInput(1,'',
      {presentation:{open:true,oneOffsHaveView:preview}}));
    const request=f.editor.inputPresentation;
    f.editor.acknowledgeInputPresentation(request.token,{...f.editor.snapshot,...popupEffects});
    assert.equal(f.editor.commitInputPopupClose(request.token,closeFacts),null);f.editor.close();
  }
});

test('parent preview dismissal pins browser text, document and persistence around the owned commit',()=>{
  for(const change of ['none','prior','mode','document','persistence','setter']){
    const {p,browser,lazy}=parentLoadFixture(),token={};let committed=false,text='prior',discarded=0;
    Object.defineProperty(browser,'userTypedValue',{get:()=>text,set:value=>{text=value;if(change==='setter')browser.lastLocationChange++;}});
    lazy.independentUrlbarBrowserState={preparePreviewDismissal:()=>{
      if(change==='mode')return null;if(change==='document')browser.browsingContext.currentWindowGlobal={};
      return {token,effects:{searchMode:null,persistedSearchTerms:false}};
    },commit:()=>{committed=true;return true;},isCommitted:()=>committed,discard:()=>discarded++};
    const result=p.commitIndependentPreviewDismissal(42,{source:2,isPreview:true},
      {userTypedValue:change==='prior'?'wrong':'prior',persistedSearchTerms:change==='persistence'},()=>true);
    const applied=['none','setter'].includes(change);
    assert.equal(!!result,applied);assert.equal(committed,applied);assert.equal(text,applied?null:'prior');
    if(result)assert.equal(result.current,change==='none');
    assert.equal(discarded,['prior','mode'].includes(change)?0:1);
  }
});


const closedPreview=request=>({...request.edit,open:false,searchMode:null});
function ownedDismissalBackend(f){
  const calls=[];f.parent.commitIndependentPreviewDismissal=(id,mode,observation,current)=>{
    assert.equal(current(),true);calls.push({id,mode,observation});
    return {applied:true,current:true,stateToken:{},searchMode:null,userTypedValue:null,persistedSearchTerms:false};
  };return calls;
}

test('owned preview dismissal separates surface acknowledgment from one-use browser cleanup',async()=>{
  const f=await previewDismissalFixture(),request=f.proposal,calls=ownedDismissalBackend(f),queries=f.parent.started.length;
  assert.equal(f.editor.commitAcknowledgedPreviewClose(request.token,closeFacts),null);
  for(const observation of [{...closedPreview(request),open:true},{...closedPreview(request),searchMode:{}},
    {...closedPreview(request),text:'pending edit'},{...closedPreview(request),selectionEnd:0}]){
    assert.equal(f.editor.acknowledgeOwnedPreviewClose(request.token,observation),false);
    assert.equal(f.editor.getCloseRequest(request.token),request);
  }
  assert.equal(f.editor.acknowledgeOwnedPreviewClose(request.token,closedPreview(request)),true);
  assert.equal(f.editor.acknowledgeOwnedPreviewClose(request.token,closedPreview(request)),false);
  assert.equal(f.editor.acknowledgeClose(request.token,{open:false,searchMode:null,userTypedValue:null}),false);
  const result=f.editor.commitAcknowledgedPreviewClose(request.token,closeFacts);
  assert.equal(result.applied,true);assert.equal(result.current,true);assert.equal(calls.length,1);
  assert.equal(calls[0].mode.isPreview,true);assert.equal(f.editor.beginSearchModeExit(1),null);
  assert.equal(f.editor.commitAcknowledgedPreviewClose(request.token,closeFacts),null);
  assert.equal(f.parent.started.length,queries);f.editor.close();
});

test('focus-loss and provider closes share owned cleanup while the legacy acknowledgment grants none',async()=>{
  for(const kind of ['hide','provider','legacy']){
    const f=await modeWriteFixture({startQuery:false});f.editor.acknowledgeResultModeApplication(f.proposal.token,observedMode(f.proposal));
    const calls=ownedDismissalBackend(f);
    if(kind==='hide')f.editor.hideResults(1,f.query.id);else f.parent.child.notify('onResultsCloseRequested',f.query.id,true);
    const request=f.requests.at(-1);
    assert.equal(request.previousSearchMode.isPreview,true);
    if(kind==='legacy')f.editor.acknowledgeClose(request.token,{open:false,searchMode:null,userTypedValue:null});
    else assert.equal(f.editor.acknowledgeOwnedPreviewClose(request.token,closedPreview(request)),true);
    const result=f.editor.commitAcknowledgedPreviewClose(request.token,closeFacts);
    assert.equal(!!result,kind!=='legacy');assert.equal(calls.length,kind==='legacy'?0:1);f.editor.close();
  }
});

test('owned preview close rejects acknowledgment reentry and cancellation after acknowledgment',async()=>{
  const f=await previewDismissalFixture(),request=f.proposal;
  const observation={...closedPreview(request),get text(){f.editor.select(1,0,0);return request.edit.text;}};
  assert.equal(f.editor.acknowledgeOwnedPreviewClose(request.token,observation),false);
  assert.equal(f.editor.commitAcknowledgedPreviewClose(request.token,closeFacts),null);f.editor.close();
  for(const revoke of [f=>f.editor.cancel(),f=>f.editor.select(1,0,0),f=>f.editor.edit({},edit(2,'new'))]){
    const f=await previewDismissalFixture(),calls=ownedDismissalBackend(f);
    f.editor.acknowledgeOwnedPreviewClose(f.proposal.token,closedPreview(f.proposal));await revoke(f);
    assert.equal(f.editor.commitAcknowledgedPreviewClose(f.proposal.token,closeFacts),null);
    assert.equal(calls.length,0);f.editor.close();
  }
});

test('owned preview dismissal consumes backend failure and detects cancellation during commit',async()=>{
  for(const reenter of [false,true]){
    const f=await previewDismissalFixture();let calls=0;
    f.editor.acknowledgeOwnedPreviewClose(f.proposal.token,closedPreview(f.proposal));
    f.parent.commitIndependentPreviewDismissal=()=>{calls++;if(!reenter)return null;
      f.editor.cancel();return {applied:true,current:true,stateToken:{}};};
    const result=f.editor.commitAcknowledgedPreviewClose(f.proposal.token,closeFacts);
    assert.equal(!!result,reenter);if(result)assert.equal(result.current,false);
    assert.equal(f.editor.commitAcknowledgedPreviewClose(f.proposal.token,closeFacts),null);
    assert.equal(calls,1);f.editor.close();
  }
});


async function closeRecordingFixture({text='',open=true,picked=false}={}) {
  const f=await editorFixture();
  const query=await f.editor.edit({searchMode:{source:2,isPreview:true}},edit(1,text));
  if(open)f.editor.publishPresentation(1,query.id,[],true);
  let request;
  if(picked){f.parent.child.notify('onResultsCloseRequested',query.id,true);request=f.requests.at(-1);}
  else request=f.editor.beginPreviewModeDismissal(1,query.id);
  const calls=[];f.parent.child.engagementEvent.discard=()=>calls.push('discard');
  f.parent.child.engagementEvent.record=(event,details)=>calls.push(['record',event,{...details}]);
  f.parent.recordZeroPrefix=kind=>calls.push(['zero',kind]);
  return {...f,query,request,calls};
}

test('close recording requires acknowledgment and records captured zero-prefix abandonment once',async()=>{
  const f=await closeRecordingFixture();
  assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),null);
  f.editor.acknowledgeOwnedPreviewClose(f.request.token,closedPreview(f.request));
  const result=f.editor.recordAcknowledgedClose(f.request.token,{focused:false});
  assert.deepEqual(f.calls,['discard',['record',null,{}],['zero','abandonment']]);
  assert.equal(result.current,true);assert.equal(result.discardedEngagement,true);
  assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),null);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('picked, focused, nonempty and unopened closes retain Firefox recording policy',async()=>{
  for(const facts of [{picked:true,focused:false},{focused:true},{text:'words',focused:false},
    {open:false,focused:false}]){
    const f=await closeRecordingFixture(facts);
    f.editor.acknowledgeClose(f.request.token,{open:false,searchMode:null,userTypedValue:null});
    const result=f.editor.recordAcknowledgedClose(f.request.token,{focused:facts.focused});
    assert.equal(result.discardedEngagement,!facts.picked&&!facts.focused&&facts.open!==false);
    assert.equal(result.zeroPrefix,facts.text||facts.open===false?null:facts.picked?'engagement':'abandonment');
    if(facts.picked)assert.deepEqual(f.calls,[['zero','engagement']]);
    if(facts.open===false)assert.deepEqual(f.calls,[]);f.editor.close();
  }
});

test('new input and cancellation revoke close recording without disturbing the next query',async()=>{
  for(const action of [f=>f.editor.edit({},edit(2,'new')),f=>f.editor.cancel(),f=>f.editor.select(1,0,0)]){
    const f=await closeRecordingFixture();f.editor.acknowledgeOwnedPreviewClose(f.request.token,closedPreview(f.request));
    await action(f);assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),null);
    assert.deepEqual(f.calls,[]);f.editor.close();
  }
});

test('recording consumes throwing hooks and stops further effects after discard reentry',async()=>{
  for(const throwing of [false,true]){
    const f=await closeRecordingFixture();f.editor.acknowledgeOwnedPreviewClose(f.request.token,closedPreview(f.request));
    f.parent.child.engagementEvent.discard=()=>{f.calls.push('discard');
      if(throwing)throw Error('recorder failure');f.editor.cancel();};
    if(throwing)assert.throws(()=>f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),/recorder failure/);
    else assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}).current,false);
    assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),null);
    assert.deepEqual(f.calls,['discard']);f.parent.child.engagementEvent.discard=()=>{};f.editor.close();
  }
});


test('empty-input close records the preceding query rather than classifying the new empty text',async()=>{
  for(const prior of ['', 'typed', null]) {
    const f=await editorFixture();f.prefs['suggest.topsites']=false;
    if(prior!==null)await f.editor.edit({},edit(1,prior));
    const queries=f.parent.started.length,calls=[];
    f.parent.child.engagementEvent.discard=()=>calls.push('discard');
    f.parent.child.engagementEvent.record=()=>calls.push('record');
    f.parent.recordZeroPrefix=kind=>calls.push(kind);
    assert.equal(await f.editor.edit({},popupInput(2)),null);
    const request=f.editor.inputPresentation;
    assert.equal(request.recording.open,true,'actual host observation overrides prior logical openness');
    assert.equal(request.recording.zeroPrefix,prior==='');assert.equal(request.recording.elementPicked,false);
    assert.equal(Object.isFrozen(request.recording),true);
    assert.equal(f.editor.recordAcknowledgedClose(request.token,{focused:false}),null);
    f.editor.acknowledgeInputPresentation(request.token,{...f.editor.snapshot,...popupEffects});
    const result=f.editor.recordAcknowledgedClose(request.token,{focused:false});
    assert.equal(result.current,true);assert.equal(result.zeroPrefix,prior===''?'abandonment':null);
    assert.deepEqual(calls,['discard','record',...(prior===''?['abandonment']:[])]);
    assert.equal(f.editor.recordAcknowledgedClose(request.token,{focused:false}),null);
    assert.equal(f.parent.started.length,queries);f.editor.close();
  }
});

test('continuing input popup paths never authorize close recording',async()=>{
  for(const presentation of [{open:false,oneOffsHaveView:false},{open:true,oneOffsHaveView:true}]){
    const f=await editorFixture();f.prefs['suggest.topsites']=false;
    await f.editor.edit({searchMode:{source:2}},popupInput(1,'',{presentation}));
    const request=f.editor.inputPresentation;assert.equal(request.close,false);assert.equal(request.recording,undefined);
    assert.equal(f.editor.acknowledgeInputPresentation(request.token,{...f.editor.snapshot,...popupEffects}),true);
    assert.equal(f.editor.recordAcknowledgedClose(request.token,{focused:false}),null);f.editor.close();
  }
});

test('input close recording remains one-use after preview cleanup and is revoked by subsequent input',async()=>{
  for(const newer of [false,true]){
    const f=await popupCloseFixture(),calls=[];
    f.parent.child.engagementEvent.discard=()=>calls.push('discard');f.parent.child.engagementEvent.record=()=>calls.push('record');
    f.editor.acknowledgeInputPresentation(f.request.token,f.observed);
    assert.equal(f.editor.commitInputPopupClose(f.request.token,closeFacts).current,true);
    if(newer)await f.editor.edit({},edit(2,'new query'));
    const result=f.editor.recordAcknowledgedClose(f.request.token,{focused:false});
    assert.equal(!!result,!newer);assert.deepEqual(calls,newer?[]:['discard','record']);
    assert.equal(f.editor.recordAcknowledgedClose(f.request.token,{focused:false}),null);f.editor.close();
  }
});


test('independent finished zero-prefix query records once before completion publication',async()=>{
  const f=fixture(),calls=[];f.parent.recordZeroPrefix=kind=>calls.push(kind);
  const query=await f.session.query({}, {searchString:''},update=>{if(update.kind==='finished')calls.push('finished');});
  f.parent.child.notify('onQueryFinished',query);f.parent.child.notify('onQueryFinished',query);
  assert.deepEqual(calls,['exposure','finished']);assert.equal(f.parent.child.resultsOpen,false);
  assert.deepEqual([...f.parent.child.visibleResults],[]);f.session.close();
});

test('nonempty, canceled, frozen, replaced and scope-lost queries cannot record stale completion',async()=>{
  for(const kind of ['nonempty','cancelled','frozen','replaced','scope']){
    const f=fixture(),query=await f.session.query({}, {searchString:kind==='nonempty'?'typed':''});
    if(kind==='cancelled')f.parent.child.notify('onQueryCancelled',query);
    if(kind==='frozen'){query.results=[{id:1}];f.session.freezeResults(query.id);}
    if(kind==='replaced')await f.session.query({}, {searchString:'new'});
    if(kind==='scope')f.invalidate();
    f.parent.child.notify('onQueryFinished',query);assert.deepEqual(f.parent.zeroPrefix??[],[]);f.session.close();
  }
});

test('completion recorder reentry suppresses old updates and cannot duplicate exposure',async()=>{
  for(const cancel of [false,true]){
    const f=fixture(),updates=[];let calls=0;
    const query=await f.session.query({}, {searchString:''},update=>updates.push(update.kind));
    f.parent.recordZeroPrefix=()=>{calls++;if(cancel)f.session.cancel();else f.parent.child.notify('onQueryFinished',query);};
    f.parent.child.notify('onQueryFinished',query);
    assert.equal(calls,1);assert.equal(updates.filter(kind=>kind==='finished').length,cancel?0:1);f.session.close();
  }
});

test('throwing completion recorder still delivers completion and consumes the attempt',async()=>{
  const f=fixture(),updates=[];let calls=0;
  const query=await f.session.query({}, {searchString:''},update=>updates.push(update.kind));
  f.parent.recordZeroPrefix=()=>{calls++;throw Error('expected completion recorder failure');};
  f.parent.child.notify('onQueryFinished',query);f.parent.child.notify('onQueryFinished',query);
  assert.equal(calls,1);assert.equal(updates.filter(kind=>kind==='finished').length,1);f.session.close();
});


async function finishedPresentationFixture(mode=null,updated=false){
  const f=await editorFixture(),query=await f.editor.edit({searchMode:mode},edit(1,'words'));
  if(updated){query.results=[{id:1,payload:{}}];f.parent.child.notify('onQueryResults',query);query.results=[];}
  f.parent.child.notify('onQueryFinished',query);
  return {...f,query};
}

test('finished presentation uses result-update history rather than final result count',async()=>{
  for(const updated of [false,true]){
    const f=await finishedPresentationFixture(null,updated),snapshot=f.editor.snapshot;
    const proposal=f.editor.beginFinishedPresentation(1,f.query.id);
    assert.equal(proposal.rows,updated?'remove-stale':'clear');
    assert.equal(proposal.action,updated?'keep':'close');
    assert.equal(f.editor.getFinishedPresentation(proposal.token),proposal);
    assert.equal(Object.isFrozen(proposal),true);assert.equal(f.editor.snapshot,snapshot);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('finished one-off decisions validate observations and resolve only once',async()=>{
  for(const open of [false,true])for(const oneOffsWillHide of [false,true]){
    const f=await finishedPresentationFixture({source:2}),snapshot=f.editor.snapshot;
    const proposal=f.editor.beginFinishedPresentation(1,f.query.id);
    assert.equal(proposal.action,'resolve-one-offs');
    assert.throws(()=>f.editor.resolveFinishedPresentation(proposal.token,{oneOffsWillHide:0,open}),/visibility/);
    assert.equal(f.editor.getFinishedPresentation(proposal.token),proposal);
    const resolved=f.editor.resolveFinishedPresentation(proposal.token,{oneOffsWillHide,open});
    assert.equal(resolved.action,!oneOffsWillHide?'open-one-offs':open?'close':'keep');
    assert.equal(f.editor.getFinishedPresentation(proposal.token),resolved);
    assert.equal(f.editor.resolveFinishedPresentation(proposal.token,{oneOffsWillHide,open}),null);
    assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('late finished presentation is revoked by caret, edit, cancellation and result updates',async()=>{
  for(const mutation of ['caret','edit','cancel','results']){
    const f=await finishedPresentationFixture({source:2});
    const proposal=f.editor.beginFinishedPresentation(1,f.query.id);
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='edit')await f.editor.edit({},edit(2,'next'));
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='results')f.parent.child.notify('onQueryResults',f.query);
    assert.equal(f.editor.getFinishedPresentation(proposal.token),null);
    assert.equal(f.editor.resolveFinishedPresentation(proposal.token,{oneOffsWillHide:false,open:true}),null);
    f.editor.close();
  }
});

test('finished presentation requires successful completion and current proposal and scope',async()=>{
  const f=await editorFixture(),query=await f.editor.edit({},edit(1,'words'));
  assert.equal(f.editor.beginFinishedPresentation(1,query.id),null);
  f.parent.child.notify('onQueryCancelled',query);f.parent.child.notify('onQueryFinished',query);
  assert.equal(f.editor.beginFinishedPresentation(1,query.id),null);f.editor.close();
  const g=await finishedPresentationFixture({source:2});
  const old=g.editor.beginFinishedPresentation(1,g.query.id),current=g.editor.beginFinishedPresentation(1,g.query.id);
  assert.equal(g.editor.getFinishedPresentation(old.token),null);
  assert.equal(g.editor.getFinishedPresentation(current.token),current);
  g.loseScope();assert.throws(()=>g.editor.getFinishedPresentation(current.token),/scope changed/);g.editor.close();
});


const finishedObservation=proposal=>({...proposal.edit,open:proposal.action==='open-one-offs',
  resultsCleared:true,staleRowsRemoved:true,oneOffsEnabled:true,searchMode:null});

async function finishedAckFixture({mode=null,text='words',open=false,hide=true,updated=false}={}){
  const f=await editorFixture(),query=await f.editor.edit({searchMode:mode},edit(1,text));
  if(updated){query.results=[{id:1,payload:{}}];f.parent.child.notify('onQueryResults',query);}
  f.editor.publishPresentation(1,query.id,updated?query.results:[],open);
  f.parent.child.notify('onQueryFinished',query);
  let proposal=f.editor.beginFinishedPresentation(1,query.id);
  if(proposal.action==='resolve-one-offs')proposal=f.editor.resolveFinishedPresentation(proposal.token,{oneOffsWillHide:hide,open});
  return {...f,query,proposal};
}

test('finished acknowledgment requires observed row and popup effects and is one-use',async()=>{
  for(const facts of [{},{mode:{source:2},hide:false},{mode:{source:2},open:false},{updated:true}]){
    const f=await finishedAckFixture(facts),p=f.proposal,observed=finishedObservation(p);
    const bad=[{...observed,text:'pending input'},{...observed,selectionStart:0},{...observed,open:null},
      {...observed,[p.rows==='clear'?'resultsCleared':'staleRowsRemoved']:false}];
    if(p.action==='open-one-offs')bad.push({...observed,open:false},{...observed,oneOffsEnabled:false});
    if(p.action==='close'||(p.action==='keep'&&p.open===false))bad.push({...observed,open:true});
    if(facts.updated)bad.push({...observed,open:true});
    for(const observation of bad){
      assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,observation),false);
      assert.equal(f.editor.getFinishedPresentation(p.token),p);
    }
    assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,observed),true);
    const receipt=f.editor.finishedPresentationState;
    assert.equal(receipt.token,p.token);assert.equal(Object.isFrozen(receipt),true);
    assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,observed),false);
    assert.equal(f.editor.beginFinishedPresentation(1,f.query.id),null);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('unresolved one-off proposals cannot acknowledge effects',async()=>{
  const f=await finishedPresentationFixture({source:2}),p=f.editor.beginFinishedPresentation(1,f.query.id);
  assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(p)),false);
  assert.equal(f.editor.getFinishedPresentation(p.token),p);f.editor.close();
});

test('finished one-off acknowledgment publishes no result exposure and row cleanup preserves viewport authority',async()=>{
  for(const facts of [{mode:{source:2},hide:false},{mode:{source:2},hide:true},{updated:true,open:true}]){
    const f=await finishedAckFixture(facts),p=f.proposal,before=[...f.parent.child.visibleResults];
    assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,{...finishedObservation(p),open:facts.updated||!facts.hide}),true);
    assert.equal(f.parent.child.resultsOpen,!!(facts.updated||!facts.hide));
    assert.deepEqual([...f.parent.child.visibleResults],facts.updated?before:[]);
    assert.equal(f.editor.finishedPresentationState.closeToken,null);f.editor.close();
  }
});

test('finished close grants one-use recording only after host acknowledgment and revokes query authority',async()=>{
  for(const text of ['','words'])for(const open of [false,true]){
    const f=await finishedAckFixture({text,open}),p=f.proposal,calls=[];
    f.parent.child.engagementEvent.discard=()=>calls.push('discard');
    f.parent.child.engagementEvent.record=()=>calls.push('record');f.parent.recordZeroPrefix=kind=>calls.push(kind);
    assert.equal(f.editor.recordAcknowledgedClose(p.token,{focused:false}),null);
    assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(p)),true);
    const receipt=f.editor.finishedPresentationState;
    assert.ok(receipt.closeToken);assert.equal(f.parent.child.resultsOpen,false);
    assert.deepEqual([...f.parent.child.visibleResults],[]);
    assert.equal(f.editor.beginFinishedPresentation(1,f.query.id),null);
    const recorded=f.editor.recordAcknowledgedClose(receipt.closeToken,{focused:false});
    assert.equal(recorded.current,true);assert.equal(recorded.zeroPrefix,open&&!text?'abandonment':null);
    assert.deepEqual(calls,open?['discard','record',...(!text?['abandonment']:[])]:[]);
    assert.equal(f.editor.recordAcknowledgedClose(receipt.closeToken,{focused:false}),null);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('finished preview close shares owned browser cleanup without implicitly writing browser state',async()=>{
  const f=await finishedAckFixture({mode:{source:2,isPreview:true},open:true}),p=f.proposal,calls=ownedDismissalBackend(f);
  assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,{...finishedObservation(p),searchMode:p.searchMode}),false);
  assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(p)),true);
  const receipt=f.editor.finishedPresentationState;assert.equal(calls.length,0);
  const outcome=f.editor.commitAcknowledgedPreviewClose(receipt.closeToken,closeFacts);
  assert.equal(outcome.current,true);assert.equal(calls.length,1);assert.equal(calls[0].mode.isPreview,true);
  assert.equal(f.editor.commitAcknowledgedPreviewClose(receipt.closeToken,closeFacts),null);
  assert.equal(f.editor.finishedPresentationState,null);assert.equal(f.parent.started.length,1);f.editor.close();
});

test('finished acknowledgment rejects stale observations and getter reentry without granting effects',async()=>{
  for(const mutation of ['caret','cancel','edit','results','replace','scope']){
    const f=await finishedAckFixture({mode:{source:2},hide:false}),p=f.proposal;let pending=null;
    const observed={...finishedObservation(p),get text(){
      if(mutation==='caret')f.editor.select(1,0,0);
      if(mutation==='cancel')f.editor.cancel();
      if(mutation==='edit')pending=f.editor.edit({},edit(2,'next'));
      if(mutation==='results')f.parent.child.notify('onQueryResults',f.query);
      if(mutation==='replace')f.editor.beginFinishedPresentation(1,f.query.id);
      if(mutation==='scope')f.loseScope();
      return p.edit.text;
    }};
    if(mutation==='scope')assert.throws(()=>f.editor.acknowledgeFinishedPresentation(p.token,observed),/scope changed/);
    else assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,observed),false);
    if(pending)await pending;
    assert.equal(f.editor.finishedPresentationState,null);f.editor.close();
  }
});

test('finished receipt cannot survive later edits, caret movement, results, cancellation or scope loss',async()=>{
  for(const mutation of ['caret','edit','results','cancel','scope']){
    const f=await finishedAckFixture({mode:{source:2},hide:false}),p=f.proposal;
    f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(p));
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='edit')await f.editor.edit({},edit(2,'next'));
    if(mutation==='results')f.parent.child.notify('onQueryResults',f.query);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='scope')f.loseScope();
    assert.equal(f.editor.finishedPresentationState,null);
    if(mutation==='caret')assert.equal(f.editor.beginFinishedPresentation(1,f.query.id),null,'same finish cannot acknowledge twice after caret movement');
    f.editor.close();
  }
});

test('finished close cancellation reentry cannot acknowledge an obsolete editor',async()=>{
  const f=await finishedAckFixture({open:true}),p=f.proposal;
  const cancel=f.parent.child.cancelQuery.bind(f.parent.child);
  f.parent.child.cancelQuery=()=>{cancel();f.editor.select(1,0,0);};
  assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(p)),false);
  assert.equal(f.editor.finishedPresentationState,null);
  assert.equal(f.editor.recordAcknowledgedClose(p.token,{focused:false}),null);f.editor.close();
});


function oneOffStore(f,{second=false,local=false,disabled=false}={}){
  const engine={id:'default',name:'Default',hideOneOffButton:false};
  f.parent.child.updateEngineStore('changed',engine,-1);f.parent.child.updateEngineStore('default',engine,0);
  if(second)f.parent.child.updateEngineStore('changed',{id:'second',name:'Second'},-1);
  f.prefs['scotchBonnet.disableOneOffs']=disabled;f.prefs['scotchBonnet.enableOverride']=false;
  for(const mode of f.sandbox.UrlbarShared.LOCAL_SEARCH_MODES)f.prefs[mode.pref]=local;
  return engine;
}

test('independent one-off visibility uses its private-scoped store and live local preferences',async()=>{
  for(const isPrivate of [false,true])for(const facts of [{},{second:true},{local:true},{second:true,local:true,disabled:true}]){
    const f=fixture({isPrivate});await f.parent.child.engineStore.init();oneOffStore(f,facts);
    const plan=await f.session.prepareOneOffVisibility();
    assert.equal(plan.willHide,!!facts.disabled||!(facts.second||facts.local));
    assert.equal(plan.isCurrent(),true);assert.equal(f.parent.child.engineStore.isPrivate,isPrivate);
    assert.equal(f.parent.started.length,0);f.session.close();assert.equal(plan.isCurrent(),false);
  }
});

test('one-off visibility rejects engine/default and policy changes but ignores unrelated prefs',async()=>{
  for(const mutation of ['engine','hidden','default','local','disabled','override','scope']){
    const f=fixture();await f.parent.child.engineStore.init();const engine=oneOffStore(f,{second:true});
    const plan=await f.session.prepareOneOffVisibility();f.prefs['unrelated']=true;assert.equal(plan.isCurrent(),true);
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'second'});
    if(mutation==='hidden')f.parent.child.updateEngineStore('changed',{id:'second',name:'Second',hideOneOffButton:true},1);
    if(mutation==='default')f.parent.child.updateEngineStore('default',{id:'second'},1);
    if(mutation==='local')f.prefs[f.sandbox.UrlbarShared.LOCAL_SEARCH_MODES[0].pref]=true;
    if(mutation==='disabled')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='override')f.prefs['scotchBonnet.enableOverride']=true;
    if(mutation==='scope')f.invalidate();
    assert.equal(plan.isCurrent(),false,mutation);f.session.close();
  }
});

test('owned finished one-off resolution feeds exact acknowledgment without injecting visibility',async()=>{
  for(const facts of [{},{second:true},{local:true},{second:true,disabled:true}]){
    const f=await finishedPresentationFixture({source:2});oneOffStore(f,facts);
    const proposal=f.editor.beginFinishedPresentation(1,f.query.id);
    const resolved=await f.editor.resolveFinishedOneOffs(proposal.token,{open:true});
    const hidden=!!facts.disabled||!(facts.second||facts.local);
    assert.equal(resolved.action,hidden?'close':'open-one-offs');assert.equal(resolved.oneOffsWillHide,hidden);
    assert.equal(f.editor.acknowledgeFinishedPresentation(proposal.token,finishedObservation(resolved)),true);
    assert.equal(await f.editor.resolveFinishedOneOffs(proposal.token,{open:true}),null);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('resolved engine policy changes revoke host effect authority and receipts',async()=>{
  for(const acknowledged of [false,true]){
    const f=await finishedPresentationFixture({source:2});oneOffStore(f,{second:true});
    const p=f.editor.beginFinishedPresentation(1,f.query.id),resolved=await f.editor.resolveFinishedOneOffs(p.token,{open:true});
    if(acknowledged)f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(resolved));
    f.prefs['scotchBonnet.disableOneOffs']=true;
    assert.equal(f.editor.getFinishedPresentation(p.token),null);assert.equal(f.editor.finishedPresentationState,null);
    assert.equal(f.editor.acknowledgeFinishedPresentation(p.token,finishedObservation(resolved)),false);f.editor.close();
  }
});

test('asynchronous one-off initialization cannot resolve a replaced edit, query or proposal',async()=>{
  for(const mutation of ['caret','edit','cancel','results','replace','scope']){
    const f=await finishedPresentationFixture({source:2});oneOffStore(f,{second:true});
    const p=f.editor.beginFinishedPresentation(1,f.query.id),store=f.parent.child.engineStore,init=store.init;
    let release;store.init=()=>new Promise(resolve=>{release=resolve;});
    const pending=f.editor.resolveFinishedOneOffs(p.token,{open:false});
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='edit'){store.init=init;await f.editor.edit({},edit(2,'next'));}
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='results')f.parent.child.notify('onQueryResults',f.query);
    if(mutation==='replace')f.editor.beginFinishedPresentation(1,f.query.id);
    if(mutation==='scope')f.loseScope();
    release();
    if(mutation==='scope')await assert.rejects(pending,/scope changed/);else assert.equal(await pending,null);
    store.init=init;f.editor.close();
  }
});

test('engine initialization failures propagate without consuming the pending finish',async()=>{
  const f=await finishedPresentationFixture({source:2});oneOffStore(f);
  const p=f.editor.beginFinishedPresentation(1,f.query.id),store=f.parent.child.engineStore,init=store.init;
  store.init=async()=>{throw Error('search service unavailable');};
  await assert.rejects(f.editor.resolveFinishedOneOffs(p.token,{open:false}),/search service unavailable/);
  assert.equal(f.editor.getFinishedPresentation(p.token),p);store.init=init;
  assert.equal((await f.editor.resolveFinishedOneOffs(p.token,{open:false})).action,'keep');f.editor.close();
});

test('non-address SAPs hide one-offs without initializing engines',async()=>{
  const f=fixture(),other=new f.sandbox.UrlbarQuerySession({context:{...f.context,sapName:'searchbar'},isCurrent:()=>true});
  f.parents.at(-1).child.engineStore.init=()=>{throw Error('unexpected initialization');};
  const plan=await other.prepareOneOffVisibility();assert.equal(plan.willHide,true);assert.equal(plan.isCurrent(),true);
  other.close();assert.equal(plan.isCurrent(),false);f.session.close();
});


const oneOffFacts=(extra={})=>({pageProxyValid:false,keyboardAlt:false,mouseBackground:false,
  shiftKey:false,selectedTabIsEmpty:false,...extra});
async function oneOffCommandFixture(options={}){
  const f=await editorFixture(options);oneOffStore(f,{second:true,local:true});
  const query=await f.editor.edit({},edit(1,'typed words'));
  f.editor.publishPresentation(1,query.id,[],true);
  return {...f,query,local:f.sandbox.UrlbarShared.LOCAL_SEARCH_MODES[0]};
}

test('independent one-off plans preserve engine/local command, modifier and page-value decisions without effects',async()=>{
  for(const isPrivate of [false,true]){
    const f=await oneOffCommandFixture({isPrivate}),snapshot=f.editor.snapshot,before=f.parent.started.length;
    for(const engine of [false,true])for(const pageProxyValid of [false,true])for(const keyboardAlt of [false,true])
    for(const shiftKey of [false,true])for(const mouseBackground of [false,true])for(const selectedTabIsEmpty of [false,true]){
      const key=engine?'engine:default':`local:${f.local.restrict}`;
      const p=await f.editor.beginOneOffCommand(1,f.query.id,key,oneOffFacts({pageProxyValid,keyboardAlt,shiftKey,mouseBackground,selectedTabIsEmpty}));
      const tab=mouseBackground||(keyboardAlt&&!selectedTabIsEmpty);
      const kind=engine&&!pageProxyValid&&(shiftKey||tab)?'navigation':tab?'new-tab':'query';
      assert.ok(p);assert.equal(p.kind,kind);assert.equal(p.where,tab?'tab':'current');
      assert.equal(p.engineId,engine?'default':null);assert.equal(p.allowAutofill,!engine);
      assert.equal(p.params?.inBackground,mouseBackground?true:undefined);
      assert.equal(p.searchMode.isPreview,kind!=='new-tab');assert.equal(p.searchMode.entry,'oneoff');
      assert.equal(p.newTabTypedValue,kind==='new-tab'&&!pageProxyValid?'typed words':null);
      assert.equal(p.startQuery,kind==='query'||(kind==='new-tab'&&!mouseBackground));
      assert.equal(f.editor.getOneOffCommand(p.token),p);assert.ok(Object.isFrozen(p));assert.ok(Object.isFrozen(p.searchMode));
      assert.equal(f.editor.snapshot,snapshot);assert.equal(f.parent.started.length,before);assert.equal(f.parent.engagements.length,0);
    }
    f.editor.close();
  }
});

test('one-off plans apply search-open-in-tab preference and empty-value mode entry',async()=>{
  const f=await oneOffCommandFixture();f.prefs['browser.search.openintab']=true;
  for(const keyboardAlt of [false,true]){
    const p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts({keyboardAlt}));
    assert.equal(p.where,keyboardAlt?'current':'tab');assert.equal(p.kind,keyboardAlt?'query':'navigation');
  }
  const q=await f.editor.edit({},edit(2,''));f.editor.publishPresentation(2,q.id,[],true);
  const p=await f.editor.beginOneOffCommand(2,q.id,'engine:default',oneOffFacts({shiftKey:true}));
  assert.equal(p.kind,'new-tab');assert.equal(p.newTabTypedValue,null);assert.equal(p.searchMode.isPreview,false);f.editor.close();
});

test('one-off plans reject missing, hidden and disabled choices without constructing a selected button',async()=>{
  for(const choice of ['missing','hidden','local-disabled','all-disabled','closed-popup','wrong-query','wrong-revision']){
    const f=await oneOffCommandFixture();let key='engine:default',queryId=f.query.id,revision=1;
    if(choice==='missing')key='engine:missing';
    if(choice==='hidden')f.parent.child.updateEngineStore('changed',{id:'default',name:'Default',hideOneOffButton:true},0);
    if(choice==='local-disabled'){key=`local:${f.local.restrict}`;f.prefs[f.local.pref]=false;}
    if(choice==='all-disabled')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(choice==='closed-popup')f.editor.publishPresentation(1,queryId,[],false);
    if(choice==='wrong-query')queryId++;
    if(choice==='wrong-revision')revision++;
    assert.equal(await f.editor.beginOneOffCommand(revision,queryId,key,oneOffFacts()),null,choice);f.editor.close();
  }
});

test('one-off proposals are scoped to exact caret, query, popup lifetime and current eligibility',async()=>{
  for(const mutation of ['caret','edit','cancel','close-reopen','replace','engine','default','local','disabled','open-pref','scope']){
    const f=await oneOffCommandFixture(),p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts());
    assert.equal(f.editor.getOneOffCommand({}),null);
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='edit')await f.editor.edit({},edit(2,'next'));
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='close-reopen'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='replace')await f.editor.beginOneOffCommand(1,f.query.id,`local:${f.local.restrict}`,oneOffFacts());
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'default'});
    if(mutation==='default')f.parent.child.updateEngineStore('default',{id:'second'},1);
    if(mutation==='local')f.prefs[f.local.pref]=false;
    if(mutation==='disabled')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='open-pref')f.prefs['browser.search.openintab']=true;
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.getOneOffCommand(p.token),/scope changed/);}
    else assert.equal(f.editor.getOneOffCommand(p.token),null,mutation);
    f.editor.close();
  }
});

test('one-off asynchronous engine readiness cannot revive a superseded editor or proposal',async()=>{
  for(const mutation of ['caret','edit','cancel','close-reopen','replace','disabled','scope','close']){
    const f=await oneOffCommandFixture(),store=f.parent.child.engineStore,init=store.init;let release;
    store.init=()=>new Promise(resolve=>{release=resolve;});
    const pending=f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts());store.init=init;
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='edit')await f.editor.edit({},edit(2,'next'));
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='close-reopen'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='replace')await f.editor.beginOneOffCommand(1,f.query.id,'engine:second',oneOffFacts());
    if(mutation==='disabled')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='scope')f.loseScope();
    if(mutation==='close')f.editor.close();
    release();
    if(mutation==='scope'||mutation==='close')await assert.rejects(pending,/scope changed|closed/);
    else assert.equal(await pending,null,mutation);
    f.editor.close();
  }
});

test('one-off host observations are copied once and reentrant caret changes reject the plan',async()=>{
  for(const reenter of [false,true]){
    const f=await oneOffCommandFixture(),facts=oneOffFacts();let reads=0;
    Object.defineProperty(facts,'shiftKey',{get(){reads++;if(reenter)f.editor.select(1,0,0);return false;}});
    const p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',facts);
    assert.equal(reads,1);if(reenter)assert.equal(p,null);else {
      facts.mouseBackground=true;assert.equal(f.editor.getOneOffCommand(p.token),p);assert.equal(p.where,'current');
    }
    f.editor.close();
  }
});

test('one-off malformed observations and engine initialization failures do not leave a usable plan',async()=>{
  const f=await oneOffCommandFixture();
  for(const facts of [null,{},oneOffFacts({shiftKey:'yes'})])await assert.rejects(
    f.editor.beginOneOffCommand(1,f.query.id,'engine:default',facts),/Invalid one-off command facts/);
  const store=f.parent.child.engineStore,init=store.init;
  store.init=async()=>{throw Error('search unavailable');};
  await assert.rejects(f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts()),/search unavailable/);
  store.init=init;const p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts());
  assert.equal(f.editor.getOneOffCommand(p.token),p);f.editor.close();
});


async function oneOffModeFixture({pageValid=false,placeholder=false,engine=true}={}){
  const f=await oneOffCommandFixture({browserId:17}),calls=[],notifications=[];
  const entry={untrimmedValue:f.editor.snapshot.text,pageProxyValid:pageValid,
    hasAutofillPlaceholder:placeholder,userTypedValue:placeholder?'https://www.example.test/':'typed words'};
  f.parent.describeIndependentCompositionRestoration=()=>f.restoration;
  f.restoration=Object.freeze({value:'example.test',untrimmedValue:'https://www.example.test/',
    protocolIsTrimmed:true,wwwIsTrimmed:true,valueIsTyped:false,pageProxyValid:false,formatPolicy:{trimURLs:true}});
  f.parent.commitIndependentModeState=(browserId,mode,observation,current,onCommitted)=>{
    if(!current())return null;
    calls.push({browserId,mode,observation,current});
    onCommitted?.({browserId,searchMode:mode,persistedSearchTerms:false,userTypedValue:entry.untrimmedValue});
    return {applied:true,stateToken:{},engineUsageNotified:!!observation.engineId};
  };
  const command=await f.editor.beginOneOffCommand(1,f.query.id,engine?'engine:default':`local:${f.local.restrict}`,
    oneOffFacts({pageProxyValid:pageValid}));
  const proposal=f.editor.beginOneOffModeApplication(command.token,entry);
  return {...f,entry,command,proposal,calls,notifications};
}
function oneOffModeObservation(p){return {...p.edit,...p.editorWrite,composing:false,searchMode:p.searchMode,
  modeEntry:p.modeEntry,pageProxyValid:false};}

test('current-tab one-off mode acknowledgment preserves exact text/caret or applies page/autofill writes once',async()=>{
  for(const pageValid of [false,true])for(const placeholder of [false,true])for(const engine of [false,true]){
    const f=await oneOffModeFixture({pageValid,placeholder,engine}),p=f.proposal,before=f.parent.started.length;
    assert.ok(p);assert.equal(p.searchMode.isPreview,true);
    assert.equal(p.editorWrite?.text,pageValid?'':placeholder?'example.test':undefined);
    assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null);
    const observed=oneOffModeObservation(p);
    assert.equal(f.editor.acknowledgeOneOffModeApplication(p.token,observed),true);
    assert.equal(f.editor.acknowledgeOneOffModeApplication(p.token,observed),false);
    assert.equal(f.editor.getOneOffCommand(f.command.token),null);
    assert.equal(f.editor.snapshot.text,observed.text);assert.equal(f.calls.length,0);
    const outcome=f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false});
    assert.equal(outcome.applied,true);assert.equal(f.calls.length,1);
    assert.equal(f.calls[0].browserId,17);assert.equal(f.calls[0].observation.modeEntry,p.modeEntry);
    assert.equal(f.calls[0].observation.engineId,engine?'default':null);
    assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null);
    assert.equal(f.parent.started.length,before);f.editor.close();
  }
});

test('one-off mode acknowledgment rejects inexact mode, editor, formatting and page observations',async()=>{
  const f=await oneOffModeFixture({placeholder:true}),p=f.proposal,valid=oneOffModeObservation(p);
  for(const patch of [{text:'wrong'},{selectionStart:0},{selectionDirection:'backward'},{composing:true},
    {pageProxyValid:true},{searchMode:{...p.searchMode,isPreview:false}},{modeEntry:{...p.modeEntry,userTypedValue:'other'}},
    {untrimmedValue:'wrong'},{protocolIsTrimmed:false},{wwwIsTrimmed:false},{valueIsTyped:true}]){
    assert.equal(f.editor.acknowledgeOneOffModeApplication(p.token,{...valid,...patch}),false);
    assert.equal(f.editor.getOneOffModeApplication(p.token),p);
  }
  assert.equal(f.editor.acknowledgeOneOffModeApplication(p.token,valid),true);f.editor.close();
});

test('one-off mode handoff rejects new-tab and immediate navigation plans and mismatched pre-mode facts',async()=>{
  const f=await oneOffCommandFixture({browserId:17}),entry={untrimmedValue:'typed words',pageProxyValid:false,
    hasAutofillPlaceholder:false,userTypedValue:'typed words'};
  for(const facts of [oneOffFacts({shiftKey:true}),oneOffFacts({keyboardAlt:true,pageProxyValid:true})]){
    const p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',facts);
    assert.equal(f.editor.beginOneOffModeApplication(p.token,entry),null);
  }
  const p=await f.editor.beginOneOffCommand(1,f.query.id,'engine:default',oneOffFacts());
  assert.throws(()=>f.editor.beginOneOffModeApplication(p.token,null),/Missing/);
  assert.throws(()=>f.editor.beginOneOffModeApplication(p.token,{...entry,hasAutofillPlaceholder:'yes'}),/Invalid/);
  assert.equal(f.editor.beginOneOffModeApplication(p.token,{...entry,pageProxyValid:true}),null);f.editor.close();
});

test('acknowledged one-off modes cannot commit after editor, popup or eligibility changes',async()=>{
  for(const mutation of ['caret','cancel','popup','engine','pref','new-command','scope']){
    const f=await oneOffModeFixture(),p=f.proposal;
    assert.equal(f.editor.acknowledgeOneOffModeApplication(p.token,oneOffModeObservation(p)),true);
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='popup'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'default'});
    if(mutation==='pref')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='new-command')await f.editor.beginOneOffCommand(1,f.query.id,'engine:second',oneOffFacts());
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),/scope changed/);}
    else assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null,mutation);
    assert.equal(f.calls.length,0);f.editor.close();
  }
});

test('autofill formatting changes revoke one-off application and acknowledged mode state',async()=>{
  for(const acknowledged of [false,true]){
    const f=await oneOffModeFixture({placeholder:true}),p=f.proposal;
    if(acknowledged)f.editor.acknowledgeOneOffModeApplication(p.token,oneOffModeObservation(p));
    f.parent.describeIndependentCompositionRestoration=()=>({...p.restoration,value:'new formatting'});
    if(acknowledged)assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null);
    else assert.equal(f.editor.getOneOffModeApplication(p.token),null);
    assert.equal(f.calls.length,0);f.editor.close();
  }
});

test('reentrant mode-entry and host acknowledgment getters cannot revive or overwrite another proposal',async()=>{
  const f=await oneOffModeFixture();let inner;
  const entry={...f.entry,get untrimmedValue(){inner=f.editor.beginOneOffModeApplication(f.command.token,f.entry);return f.entry.untrimmedValue;}};
  assert.equal(f.editor.beginOneOffModeApplication(f.command.token,entry),null);
  assert.equal(f.editor.getOneOffModeApplication(inner.token),inner);
  const observation={...oneOffModeObservation(inner),get text(){f.editor.select(1,0,0);return inner.edit.text;}};
  assert.equal(f.editor.acknowledgeOneOffModeApplication(inner.token,observation),false);f.editor.close();
});

test('one-off browser commit consumes before callbacks, exceptions and changed persistence observations',async()=>{
  for(const kind of ['reentry','throw','getter']){
    const f=await oneOffModeFixture(),p=f.proposal;
    f.editor.acknowledgeOneOffModeApplication(p.token,oneOffModeObservation(p));
    const commit=f.parent.commitIndependentModeState;
    f.parent.commitIndependentModeState=(...args)=>{
      assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null);
      if(kind==='throw')throw Error('setter failed');return commit(...args);
    };
    assert.throws(()=>f.editor.commitAcknowledgedOneOffMode(p.token,{}),/Missing/);
    if(kind==='throw')await assert.rejects(async()=>f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),/setter failed/);
    else if(kind==='getter')assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{get persistedSearchTerms(){f.editor.select(1,0,0);return false;}}),null);
    else assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}).applied,true);
    assert.equal(f.editor.commitAcknowledgedOneOffMode(p.token,{persistedSearchTerms:false}),null);f.editor.close();
  }
});


test('one-off query-layer commits reject forged and cross-session plans and consume their own plan once',async()=>{
  const f=fixture({browserId:17});await f.parent.child.engineStore.init();oneOffStore(f,{second:true});
  const q=await f.session.query({}, {searchString:'words'});f.session.publishPresentation(q.id,[],true);
  const prepared=await f.session.prepareOneOffCommand(q.id,'engine:default','words',oneOffFacts());
  const entry={untrimmedValue:'words',pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:'words'};
  let writes=0;f.parent.commitIndependentModeState=()=>{writes++;return {applied:true};};
  const args=[entry,null,{persistedSearchTerms:false},()=>true];
  assert.equal(f.session.commitOneOffMode({...prepared},...args),null);
  const other=new f.sandbox.UrlbarQuerySession({context:f.context,isCurrent:()=>true});
  assert.equal(other.commitOneOffMode(prepared,...args),null);
  assert.equal(f.session.commitOneOffMode(prepared,...args).applied,true);
  assert.equal(f.session.commitOneOffMode(prepared,...args),null);assert.equal(writes,1);
  other.close();f.session.close();
});


async function oneOffRequeryFixture(options={}){
  const f=await oneOffModeFixture(options),browser={token:null,typed:f.entry.userTypedValue,confirmed:options.confirmedMode??null},writes=[];
  f.parent.captureIndependentQueryAuthority=(_id,token,typed)=>{
    const current=()=>browser.token===token&&browser.typed===typed;
    return current()?current:null;
  };
  f.parent.describeIndependentConfirmedMode=(id,token,typed)=>
    f.parent.captureIndependentQueryAuthority(id,token,typed)?Object.freeze({searchMode:browser.confirmed}):null;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    if(!current()||(facts.expectedStateToken&&browser.token!==facts.expectedStateToken)||browser.typed!==facts.modeEntry.userTypedValue)return null;
    if(mode)browser.typed=facts.modeEntry.pageProxyValid?'':facts.modeEntry.hasAutofillPlaceholder&&facts.modeEntry.userTypedValue?
      facts.modeEntry.userTypedValue:facts.modeEntry.untrimmedValue;
    if(!mode || !mode.isPreview)browser.confirmed=mode;
    browser.token={};writes.push(mode);
    notify?.({browserId:id,searchMode:mode,userTypedValue:browser.typed,persistedSearchTerms:false});
    return {applied:true,stateToken:browser.token,userTypedValue:browser.typed};
  };
  f.editor.acknowledgeOneOffModeApplication(f.proposal.token,oneOffModeObservation(f.proposal));
  const receipt=f.editor.commitAcknowledgedOneOffMode(f.proposal.token,{persistedSearchTerms:false});
  const entry={untrimmedValue:browser.typed,userTypedValue:browser.typed,pageProxyValid:false,hasAutofillPlaceholder:false};
  const confirmation=f.editor.beginOneOffConfirmation(f.proposal.token,entry);
  return {...f,browser,writes,receipt,confirmation};
}
function oneOffConfirmationObservation(p){return {...p.edit,composing:false,searchMode:p.searchMode,
  modeEntry:p.modeEntry,pageProxyValid:false,oneOffSelected:false,persistedSearchTerms:false};}

test('current-tab one-off continuation confirms once then dispatches the captured preview query once',async()=>{
  for(const engine of [false,true])for(const pageValid of [false,true]){
    const f=await oneOffRequeryFixture({engine,pageValid}),c=f.confirmation;
    assert.ok(c);assert.equal(c.searchMode.isPreview,false);
    await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode}),/Stale/);
    f.parent.child.userSelectionBehavior='arrow';
    assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)).applied,true);
    assert.equal(f.parent.child.userSelectionBehavior,'arrow');
    assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),null);
    assert.equal(f.parent.started.length,1);assert.deepEqual(f.writes.map(mode=>mode.isPreview),[true,false]);
    const query=await f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode});
    assert.equal(query.searchMode.isPreview,true);assert.equal(query.searchMode.entry,'oneoff');
    assert.equal(query.allowAutofill,!engine);assert.equal(query.searchString,pageValid?'':'typed words');
    assert.equal(f.parent.started.length,2);assert.equal(f.editor.getOneOffRequery(f.proposal.token),null);
    await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode}),/Stale/);f.editor.close();
  }
});

test('one-off confirmation requires exact editor, mode, cleared selection and pre-mode observations',async()=>{
  const f=await oneOffRequeryFixture(),c=f.confirmation,valid=oneOffConfirmationObservation(c);
  for(const patch of [{text:'other'},{selectionStart:0},{selectionDirection:'backward'},{composing:true},
    {pageProxyValid:true},{oneOffSelected:true},{searchMode:{...c.searchMode,isPreview:true}},
    {modeEntry:{...c.modeEntry,userTypedValue:'other'}}]){
    assert.equal(f.editor.commitOneOffConfirmation(c.token,{...valid,...patch}),null);
    assert.equal(f.writes.length,1);
  }
  assert.equal(f.editor.commitOneOffConfirmation(c.token,valid).applied,true);f.editor.close();
});

test('one-off confirmation rejects stale browser receipts, editor and engine policy',async()=>{
  for(const mutation of ['typed','receipt','caret','engine','policy','close-reopen','scope']){
    const f=await oneOffRequeryFixture(),c=f.confirmation;
    if(mutation==='typed')f.browser.typed='newer';
    if(mutation==='receipt')f.browser.token={};
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'default'});
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='close-reopen'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),/scope changed/);}
    else assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),null,mutation);
    assert.equal(f.writes.length,1);f.editor.close();
  }
});

test('one-off confirmation consumes before reentrant setters or exceptions',async()=>{
  for(const failure of [false,true]){
    const f=await oneOffRequeryFixture(),c=f.confirmation,commit=f.parent.commitIndependentModeState;
    f.parent.commitIndependentModeState=(...args)=>{
      assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),null);
      if(failure)throw Error('confirmation setter failed');return commit(...args);
    };
    if(failure)assert.throws(()=>f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),/setter failed/);
    else assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)).applied,true);
    assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)),null);f.editor.close();
  }
});

test('one-off confirmation and dispatch getters cannot replace a newer editor or command',async()=>{
  const f=await oneOffRequeryFixture(),c=f.confirmation;
  assert.equal(f.editor.commitOneOffConfirmation(c.token,{...oneOffConfirmationObservation(c),get text(){f.editor.select(1,0,0);return c.edit.text;}}),null);
  f.editor.close();
  const g=await oneOffRequeryFixture(),d=g.confirmation;
  g.editor.commitOneOffConfirmation(d.token,oneOffConfirmationObservation(d));
  await assert.rejects(g.editor.requeryAfterOneOffMode(g.proposal.token,{get searchMode(){g.editor.select(1,0,0);return d.searchMode;}}),/superseded/);
  assert.equal(g.parent.started.length,1);g.editor.close();
});

test('one-off dispatch rechecks browser, editor and engine policy after waiting for engine readiness',async()=>{
  for(const mutation of ['typed','receipt','caret','policy','engine','none']){
    const f=await oneOffRequeryFixture(),c=f.confirmation;
    f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c));
    const store=f.parent.child.engineStore,gate=Promise.withResolvers(),cancel=f.parent.cancelQuery.bind(f.parent);let held=false;
    f.parent.cancelQuery=()=>{
      if(!held){held=true;Object.defineProperty(store,'initialized',{value:false,configurable:true});store.init=()=>gate.promise;}
      return cancel();
    };
    const pending=f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode});
    assert.equal(held,true);assert.equal(f.parent.started.length,1);
    Object.defineProperty(store,'initialized',{value:true,configurable:true});
    if(mutation==='typed')f.browser.typed='newer';
    if(mutation==='receipt')f.browser.token={};
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'default'});
    gate.resolve();
    if(mutation==='none'){await pending;assert.equal(f.parent.started.length,2);}
    else {await assert.rejects(pending,/dispatch authority/);assert.equal(f.parent.started.length,1,mutation);}
    f.editor.close();
  }
});


test('one-off continuation rejects cancellation reentry and requires cleared placeholder facts',async()=>{
  const f=await oneOffRequeryFixture(),c=f.confirmation;
  assert.equal(f.editor.beginOneOffConfirmation(f.proposal.token,{...c.modeEntry,hasAutofillPlaceholder:true}),null);
  const replacement=f.editor.beginOneOffConfirmation(f.proposal.token,c.modeEntry);
  f.editor.commitOneOffConfirmation(replacement.token,oneOffConfirmationObservation(replacement));
  const cancel=f.parent.cancelQuery.bind(f.parent);
  f.parent.cancelQuery=()=>{cancel();f.editor.select(1,0,0);};
  await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:replacement.searchMode}),/superseded/);
  assert.equal(f.parent.started.length,1);f.parent.cancelQuery=cancel;f.editor.close();
});


test('one-off start engages and dispatches the identical captured query after confirmation',async()=>{
  for(const withEvent of [false,true]){
    const f=await oneOffRequeryFixture(),state={searchMode:f.proposal.searchMode,browser:{userContextId:4,currentPage:'https://before.test/'}};
    const admission=f.editor.startOneOffQuery(f.proposal.token,state,{event:withEvent?{type:'keydown',timeStamp:123}:null,suppressStartQuery:false});
    assert.equal(admission.disposition,'dispatch');assert.equal(admission.engagementStarted,withEvent);
    assert.equal(f.parent.started.length,1);assert.equal(f.writes.length,1);
    assert.equal(f.editor.startOneOffQuery(f.proposal.token,state,{suppressStartQuery:false}),null);
    const future=withEvent?f.parent.engagements.at(-1).args[1]:null;
    if(future){assert.equal(future.id,admission.queryId);assert.equal(future.searchMode.isPreview,true);}
    state.browser.currentPage='https://later.test/';
    const c=f.editor.beginOneOffConfirmation(f.proposal.token,f.confirmation.modeEntry);
    f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c));
    await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode}),/overflow/);
    const query=await f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode},{inOverflowPanel:false});
    assert.equal(query.id,admission.queryId);assert.equal(query.currentPage,'https://before.test/');
    if(future)assert.equal(query,future);
    assert.equal(f.parent.engagements.length,withEvent?1:0);assert.equal(f.parent.started.length,2);f.editor.close();
  }
});

test('suppression is read after engagement and stops before confirmation without canceling the old query',async()=>{
  const f=await oneOffRequeryFixture(),order=[];let suppressed=false;
  f.parent.engagementEvent.start=()=>{order.push('engagement');suppressed=true;};
  const before=f.parent.cancelled;
  const p=f.editor.startOneOffQuery(f.proposal.token,{}, {event:{type:'click',timeStamp:4},
    get suppressStartQuery(){order.push('suppression');return suppressed;},
    get inOverflowPanel(){throw Error('read overflow before confirmation');}});
  assert.deepEqual(order,['engagement','suppression']);assert.equal(p.disposition,'suppressed');
  assert.equal(f.editor.beginOneOffConfirmation(f.proposal.token,f.confirmation.modeEntry),null);
  await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:f.confirmation.searchMode},{inOverflowPanel:false}),/Stale/);
  assert.equal(f.parent.started.length,1);assert.equal(f.writes.length,1);assert.equal(f.parent.cancelled,before);f.editor.close();
});

test('overflow is observed after real confirmation and stops without dispatch or old-query cancellation',async()=>{
  const f=await oneOffRequeryFixture();
  f.editor.startOneOffQuery(f.proposal.token,{}, {event:{type:'click',timeStamp:4},suppressStartQuery:false});
  const c=f.editor.beginOneOffConfirmation(f.proposal.token,f.confirmation.modeEntry);
  f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c));
  const before=f.parent.cancelled;
  const result=await f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:c.searchMode},{get inOverflowPanel(){
    assert.equal(f.writes.length,2);return true;}});
  assert.equal(result,null);assert.equal(f.parent.cancelled,before);assert.equal(f.parent.started.length,1);
  assert.equal(f.parent.engagements.length,1);assert.equal(f.editor.getOneOffRequery(f.proposal.token),null);f.editor.close();
});

test('reentrant engagement or suppression observations revoke the prepared one-off start',async()=>{
  for(const mutation of ['engagement','suppression','state']){
    const f=await oneOffRequeryFixture();let starts=0;
    f.parent.engagementEvent.start=()=>{starts++;if(mutation==='engagement')f.editor.select(1,0,0);};
    const state=mutation==='state'?{get browser(){f.editor.select(1,0,0);return {};}}:{};
    const result=f.editor.startOneOffQuery(f.proposal.token,state,{event:{type:'click',timeStamp:4},
      get suppressStartQuery(){if(mutation==='suppression')f.editor.select(1,0,0);return false;}});
    assert.equal(result,null);assert.equal(starts,mutation==='state'?0:1);
    assert.equal(f.editor.getOneOffRequery(f.proposal.token),null);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('invalid or failed engagement starts cannot leave a dispatchable one-off admission',async()=>{
  for(const kind of ['event','suppression','recorder']){
    const f=await oneOffRequeryFixture();
    if(kind==='recorder')f.parent.engagementEvent.start=()=>{throw Error('recorder failed');};
    assert.throws(()=>f.editor.startOneOffQuery(f.proposal.token,{},
      {event:{type:kind==='event'?'bad':'click',timeStamp:4},suppressStartQuery:kind==='suppression'?undefined:false}),/Invalid|Missing|recorder/);
    assert.equal(f.editor.getOneOffRequery(f.proposal.token),null);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});


async function oneOffSelectionFixture({suppressed=false,...options}={}){
  const f=await oneOffRequeryFixture(options);
  const admission=f.editor.startOneOffQuery(f.proposal.token,{}, {event:{type:'click',timeStamp:8},suppressStartQuery:suppressed});
  if(!suppressed){
    const c=f.editor.beginOneOffConfirmation(f.proposal.token,f.confirmation.modeEntry);
    assert.equal(f.editor.commitOneOffConfirmation(c.token,oneOffConfirmationObservation(c)).applied,true);
  }
  const entry={untrimmedValue:f.browser.typed,userTypedValue:f.browser.typed,pageProxyValid:false,hasAutofillPlaceholder:false};
  const restoration=f.editor.beginOneOffSelectionRestoration(f.proposal.token,entry);
  return {...f,admission,restoration,restoreEntry:entry};
}
function oneOffRestorationObservation(p){return {...oneOffConfirmationObservation(p),...p.editorWrite};}

test('deselection repeats the confirmed setter once before overflow or identical captured provider dispatch',async()=>{
  for(const engine of [false,true])for(const overflow of [false,true]){
    const f=await oneOffSelectionFixture({engine}),p=f.restoration;
    assert.ok(p);assert.equal(p.terminal,false);assert.equal(p.editorWrite,null);
    assert.equal(f.editor.getOneOffSelectionRestoration(p.token),p);
    await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:p.searchMode},{inOverflowPanel:false}),/Stale/);
    const captured=f.parent.engagements.at(-1).args[1];
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)).applied,true);
    assert.deepEqual(f.writes.map(mode=>mode.isPreview),[true,false,false]);
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null);
    assert.equal(f.editor.beginOneOffSelectionRestoration(f.proposal.token,f.restoreEntry),null);
    const cancelled=f.parent.cancelled;
    const result=await f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:p.searchMode},{get inOverflowPanel(){
      assert.equal(f.writes.length,3);return overflow;}});
    if(overflow){assert.equal(result,null);assert.equal(f.parent.cancelled,cancelled);}
    else {assert.equal(result,captured);assert.equal(result.searchMode.isPreview,true);assert.equal(result.allowAutofill,!engine);}
    assert.equal(f.parent.engagements.length,1);assert.equal(f.parent.started.length,overflow?1:2);f.editor.close();
  }
});

test('suppressed deselection restores owned local, engine or null state without confirming or querying',async()=>{
  for(const confirmedMode of [null,{source:5,isPreview:false,entry:'other'},
    {engineName:'Second',isPreview:false,entry:'searchbutton'},{engineName:'Removed',isPreview:false,entry:'other'}]){
    const f=await oneOffSelectionFixture({suppressed:true,confirmedMode}),p=f.restoration;
    assert.ok(p);assert.equal(p.terminal,true);assert.equal(p.editorWrite,null);
    const before=f.editor.snapshot,typed=f.browser.typed,cancelled=f.parent.cancelled;
    const expected=confirmedMode?.engineName==='Removed'?null:confirmedMode;
    assert.equal(p.searchMode?.engineName,expected?.engineName);assert.equal(p.searchMode?.source,expected?.source);
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)).applied,true);
    assert.equal(f.writes.length,2);assert.deepEqual(f.writes[1],p.searchMode);assert.deepEqual(f.browser.confirmed,p.searchMode);
    assert.equal(f.editor.snapshot.text,before.text);assert.equal(f.browser.typed,typed);
    assert.equal(f.editor.getOneOffRequery(f.proposal.token),null);
    assert.equal(f.editor.beginOneOffConfirmation(f.proposal.token,f.restoreEntry),null);
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null);
    assert.equal(f.parent.cancelled,cancelled);assert.equal(f.parent.started.length,1);assert.equal(f.parent.engagements.length,1);f.editor.close();
  }
});

test('restoration requires exact editor, mode, selection and setter observations',async()=>{
  const f=await oneOffSelectionFixture(),p=f.restoration,valid=oneOffRestorationObservation(p);
  for(const patch of [{text:'other'},{selectionStart:0},{selectionEnd:0},{selectionDirection:'backward'},
    {composing:true},{pageProxyValid:true},{oneOffSelected:true},{searchMode:null},
    {modeEntry:{...p.modeEntry,userTypedValue:'other'}}]){
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,{...valid,...patch}),null);
    assert.equal(f.writes.length,2);assert.equal(f.editor.getOneOffSelectionRestoration(p.token),p);
  }
  assert.throws(()=>f.editor.commitOneOffSelectionRestoration(p.token,{...valid,persistedSearchTerms:undefined}),/Missing/);
  assert.equal(f.editor.commitOneOffSelectionRestoration({},valid),null);
  assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,valid).applied,true);f.editor.close();
});

test('restoration rejects stale browser receipt, owned mode, caret, popup, engine and scope',async()=>{
  for(const mutation of ['typed','receipt','stored-mode','caret','cancel','popup','engine','policy','scope']){
    const f=await oneOffSelectionFixture(),p=f.restoration;
    if(mutation==='typed')f.browser.typed='newer';
    if(mutation==='receipt')f.browser.token={};
    if(mutation==='stored-mode')f.browser.confirmed=null;
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='popup'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'default'});
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),/scope changed/);}
    else assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null,mutation);
    assert.equal(f.writes.length,2);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('suppressed restoration normalizes a missing stored engine but never accepts an engine change after proposal',async()=>{
  const f=await oneOffSelectionFixture({suppressed:true,confirmedMode:{engineName:'Second',isPreview:false,entry:'other'}});
  const p=f.restoration;assert.equal(p.searchMode.engineName,'Second');
  f.parent.child.updateEngineStore('removed',{id:'second'});
  assert.equal(f.editor.getOneOffSelectionRestoration(p.token),null);
  assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null);
  assert.equal(f.writes.length,1);f.editor.close();
});

test('restoration consumes before reentrant setters, throws and rejected persistence effects',async()=>{
  for(const kind of ['reenter','throw','reject','edit']){
    const f=await oneOffSelectionFixture(),p=f.restoration,commit=f.parent.commitIndependentModeState;
    f.parent.commitIndependentModeState=(...args)=>{
      assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null);
      assert.equal(f.editor.beginOneOffSelectionRestoration(f.proposal.token,f.restoreEntry),null);
      if(kind==='throw')throw Error('restoration setter failed');
      if(kind==='reject')return null;
      if(kind==='edit')f.editor.select(1,0,0);
      return commit(...args);
    };
    if(kind==='throw')assert.throws(()=>f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),/setter failed/);
    else {
      const result=f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p));
      if(kind==='reenter')assert.equal(result.applied,true);else assert.equal(result,null);
    }
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)),null);
    assert.equal(f.writes.length,kind==='reenter'?3:2);f.editor.close();
  }
});

test('restoration getter reentry preserves a newer proposal and cannot overwrite a new caret',async()=>{
  const f=await oneOffSelectionFixture();let inner,reads=0;
  const outer=f.editor.beginOneOffSelectionRestoration(f.proposal.token,{...f.restoreEntry,
    get untrimmedValue(){reads++;inner=f.editor.beginOneOffSelectionRestoration(f.proposal.token,f.restoreEntry);return f.restoreEntry.untrimmedValue;}});
  assert.equal(outer,null);assert.equal(reads,1);assert.equal(f.editor.getOneOffSelectionRestoration(inner.token),inner);
  assert.equal(f.editor.commitOneOffSelectionRestoration(inner.token,{...oneOffRestorationObservation(inner),
    get text(){f.editor.select(1,0,0);return inner.edit.text;}}),null);
  assert.equal(f.editor.snapshot.selectionStart,0);assert.equal(f.writes.length,2);f.editor.close();
});

test('suppressed nonnull restoration acknowledges Firefox formatting while null mode preserves the editor',async()=>{
  for(const confirmedMode of [null,{source:5,isPreview:false,entry:'other'}]){
    const f=await oneOffSelectionFixture({suppressed:true,placeholder:true,confirmedMode});
    const p=f.editor.beginOneOffSelectionRestoration(f.proposal.token,{...f.restoreEntry,hasAutofillPlaceholder:true});
    assert.ok(p);assert.equal(!!p.editorWrite,!!confirmedMode);
    if(confirmedMode){
      assert.equal(p.editorWrite.text,'example.test');
      for(const patch of [{text:'wrong'},{untrimmedValue:'wrong'},{protocolIsTrimmed:false},{wwwIsTrimmed:false},{valueIsTyped:true}]){
        assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,{...oneOffRestorationObservation(p),...patch}),null);
      }
    }
    assert.equal(f.editor.commitOneOffSelectionRestoration(p.token,oneOffRestorationObservation(p)).applied,true);
    assert.equal(f.browser.typed,'https://www.example.test/');assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('formatting and browser-reader callbacks cannot revive a stale restoration',async()=>{
  const f=await oneOffSelectionFixture({suppressed:true,placeholder:true,confirmedMode:{source:5,isPreview:false,entry:'other'}});
  const p=f.editor.beginOneOffSelectionRestoration(f.proposal.token,{...f.restoreEntry,hasAutofillPlaceholder:true});
  f.parent.describeIndependentCompositionRestoration=()=>({...f.restoration,value:'new format'});
  assert.equal(f.editor.getOneOffSelectionRestoration(p.token),null);assert.equal(f.writes.length,1);f.editor.close();
  const g=await oneOffSelectionFixture(),read=g.parent.describeIndependentConfirmedMode;
  g.parent.describeIndependentConfirmedMode=(...args)=>{const value=read(...args);g.editor.select(1,0,0);return value;};
  assert.equal(g.editor.getOneOffSelectionRestoration(g.restoration.token),null);assert.equal(g.writes.length,2);g.editor.close();
});

test('restoration rejects foreign receipts at the query layer and cannot begin before suppression or confirmation',async()=>{
  const f=await oneOffRequeryFixture();
  assert.equal(f.editor.beginOneOffSelectionRestoration(f.proposal.token,f.confirmation.modeEntry),null);
  const other=new f.sandbox.UrlbarQuerySession({context:f.context,isCurrent:()=>true});
  assert.equal(other.describeOneOffSelectionRestoration(f.receipt),null);
  assert.equal(other.commitOneOffSelectionRestoration({...f.receipt},{},f.confirmation.modeEntry,null,
    {persistedSearchTerms:false},()=>true),null);
  other.close();f.editor.close();
});


test('failed replacement of a restoration proposal cannot bypass required deselection before dispatch',async()=>{
  const f=await oneOffSelectionFixture(),old=f.restoration;
  assert.equal(f.editor.beginOneOffSelectionRestoration(f.proposal.token,{...f.restoreEntry,pageProxyValid:true}),null);
  assert.equal(f.editor.getOneOffSelectionRestoration(old.token),null);
  await assert.rejects(f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:old.searchMode},{inOverflowPanel:false}),/Stale/);
  const replacement=f.editor.beginOneOffSelectionRestoration(f.proposal.token,f.restoreEntry);
  assert.equal(f.editor.commitOneOffSelectionRestoration(replacement.token,oneOffRestorationObservation(replacement)).applied,true);
  const query=await f.editor.requeryAfterOneOffMode(f.proposal.token,{searchMode:replacement.searchMode},{inOverflowPanel:false});
  assert.equal(query.id,f.admission.queryId);assert.equal(f.writes.length,3);f.editor.close();
});


async function oneOffNewTabFixture({engine=false,pageValid=false,background=false,...options}={}){
  const f=await oneOffCommandFixture({browserId:17,...options}),created=[];
  const command=await f.editor.beginOneOffCommand(1,f.query.id,engine?'engine:default':`local:${f.local.restrict}`,
    oneOffFacts({pageProxyValid:pageValid,keyboardAlt:!background,mouseBackground:background}));
  f.parent.createIndependentOneOffSearchTab=(sourceBrowserId,searchMode,facts,current)=>{
    if(!current())return null;
    created.push({sourceBrowserId,searchMode,facts});
    return Object.freeze({created:true,initialized:true,targetBrowserId:18,stateToken:{},searchMode,userTypedValue:facts.userTypedValue});
  };
  return {...f,command,created};
}

test('new-tab one-off commands create confirmed local or engine targets once without source editor or query effects',async()=>{
  for(const background of [false,true])for(const pageValid of [false,true])for(const engine of [false,true]){
    if(engine&&!pageValid)continue;
    const f=await oneOffNewTabFixture({engine,pageValid,background}),before=f.editor.snapshot;
    assert.equal(f.command.kind,'new-tab');
    const result=f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:pageValid?null:'typed words'});
    assert.equal(result.initialized,true);assert.equal(result.inBackground,background);
    assert.equal(result.requiresActivation,!background);assert.equal(result.startQuery,!background);
    assert.equal(result.sourceModeNotificationPending,true);assert.equal(result.sourceDeselectionPending,true);
    assert.equal(result.commandToken,f.command.token);assert.equal(result.queryId,f.query.id);
    assert.equal(result.searchMode.isPreview,false);assert.equal(result.userTypedValue,pageValid?null:'typed words');
    assert.equal(f.created.length,1);assert.equal(f.created[0].sourceBrowserId,17);
    assert.equal(f.editor.snapshot,before);assert.equal(f.parent.started.length,1);assert.equal(f.parent.engagements.length,0);
    assert.equal(f.editor.getOneOffCommand(f.command.token),null);
    assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);
    assert.equal(f.created.length,1);f.editor.close();
  }
});

test('new-tab creation excludes immediate engine navigation and current-tab commands',async()=>{
  const f=await oneOffNewTabFixture({engine:true});assert.equal(f.command.kind,'navigation');
  assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);
  const command=await f.editor.beginOneOffCommand(1,f.query.id,`local:${f.local.restrict}`,oneOffFacts());
  assert.equal(command.kind,'query');assert.equal(f.editor.createOneOffSearchTab(command.token,{userTypedValue:'typed words'}),null);
  assert.equal(f.created.length,0);f.editor.close();
});

test('new-tab creation rejects stale editor, popup, engine policy and scope before effects',async()=>{
  for(const mutation of ['caret','cancel','popup','engine','policy','scope','getter']){
    const f=await oneOffNewTabFixture();
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='popup'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'second'});
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    const facts={get userTypedValue(){if(mutation==='getter')f.editor.select(1,0,0);return 'typed words';}};
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.createOneOffSearchTab(f.command.token,facts),/scope changed/);}
    else assert.equal(f.editor.createOneOffSearchTab(f.command.token,facts),null,mutation);
    assert.equal(f.created.length,0);f.editor.close();
  }
});

test('new-tab command consumes before creation callbacks, throws or partial outcomes',async()=>{
  for(const kind of ['success','throw','partial','null']){
    const f=await oneOffNewTabFixture(),create=f.parent.createIndependentOneOffSearchTab;let calls=0;
    f.parent.createIndependentOneOffSearchTab=(...args)=>{
      calls++;assert.equal(f.editor.getOneOffCommand(f.command.token),null);
      assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);
      if(kind==='throw')throw Error('tab creation failed');
      if(kind==='null')return null;
      const result=create(...args);return kind==='partial'?{...result,initialized:false}:result;
    };
    if(kind==='throw')assert.throws(()=>f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),/creation failed/);
    else {const result=f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'});
      assert.equal(result?.initialized,kind==='null'?undefined:kind==='success');}
    assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);assert.equal(calls,1);f.editor.close();
  }
});

test('new-tab source observations are read once and reentrant creation cannot erase a newer command',async()=>{
  const f=await oneOffNewTabFixture();let reads=0,newer;
  assert.throws(()=>f.editor.createOneOffSearchTab(f.command.token,{}),/Missing/);
  const create=f.parent.createIndependentOneOffSearchTab;
  f.parent.createIndependentOneOffSearchTab=(...args)=>{
    newer=f.editor.beginOneOffCommand(1,f.query.id,`local:${f.local.restrict}`,oneOffFacts({keyboardAlt:true}));
    return create(...args);
  };
  assert.equal(f.editor.createOneOffSearchTab(f.command.token,{get userTypedValue(){reads++;return 'typed words';}}),null);
  const next=await newer;assert.ok(next);assert.equal(f.editor.getOneOffCommand(next.token),next);assert.equal(reads,1);
  f.parent.createIndependentOneOffSearchTab=create;
  assert.equal(f.editor.createOneOffSearchTab(next.token,{userTypedValue:'typed words'}).initialized,true);f.editor.close();
});

test('query-layer new-tab creation rejects copied or foreign plans and unpinned sources',async()=>{
  const f=fixture({browserId:17});await f.parent.child.engineStore.init();oneOffStore(f,{second:true,local:true});
  const q=await f.session.query({}, {searchString:'words'});f.session.publishPresentation(q.id,[],true);
  const key=`local:${f.sandbox.UrlbarShared.LOCAL_SEARCH_MODES[0].restrict}`;
  const prepared=await f.session.prepareOneOffCommand(q.id,key,'words',oneOffFacts({keyboardAlt:true}));let calls=0;
  f.parent.createIndependentOneOffSearchTab=()=>{calls++;return {created:true,initialized:true};};
  assert.equal(f.session.createOneOffSearchTab({...prepared},'words',()=>true),null);
  const other=new f.sandbox.UrlbarQuerySession({context:f.context,isCurrent:()=>true});
  assert.equal(other.createOneOffSearchTab(prepared,'words',()=>true),null);
  assert.equal(f.session.createOneOffSearchTab(prepared,'words',()=>true).initialized,true);
  assert.equal(f.session.createOneOffSearchTab(prepared,'words',()=>true),null);assert.equal(calls,1);
  other.close();f.session.close();
});


async function oneOffSourceEffectsFixture(listener=()=>{},options={}){
  const events=[],f=await oneOffNewTabFixture({...options,onModeStateChanged:event=>{events.push(event);listener(event);}});
  f.parent.createIndependentOneOffSearchTab=(sourceBrowserId,searchMode,facts,current,notify)=>{
    if(!current())return null;
    f.created.push({sourceBrowserId,searchMode,facts});
    notify?.({browserId:sourceBrowserId,targetBrowserId:18,searchMode:facts.sourceModeEffects.searchMode,
      persistedSearchTerms:false,persistenceChanged:false,userTypedValue:facts.sourceUserTypedValue});
    return {created:true,initialized:current(),sourceModeStateCommitted:true,sourceModeNotified:true,
      targetBrowserId:18,typedValueWritten:current()&&facts.userTypedValue!==null};
  };
  return {...f,events};
}

test('ordered source effects publish correlated source mode once while retaining pending activation and deselection',async()=>{
  for(const background of [false,true]){
    const f=await oneOffSourceEffectsFixture(()=>{},{background}),before=f.editor.snapshot;let reads=0;
    const result=f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',get persistedSearchTerms(){reads++;return false;}});
    assert.equal(result.initialized,true);assert.equal(result.sourceModeNotificationPending,false);assert.equal(result.sourceDeselectionPending,true);
    assert.equal(result.requiresActivation,!background);assert.equal(f.events.length,1);assert.equal(reads,1);
    const event=f.events[0];assert.equal(event.reason,'one-off-new-tab-source');assert.equal(event.kind,'mode-state-changed');
    assert.equal(event.commandToken,f.command.token);assert.equal(event.queryId,f.query.id);assert.equal(event.edit,before);
    assert.equal(event.browserId,17);assert.equal(event.targetBrowserId,18);assert.equal(event.searchMode,null);
    assert.equal(Object.isFrozen(event),true);assert.equal(f.editor.snapshot,before);assert.equal(f.parent.started.length,1);
    assert.equal(f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false}),null);
    assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);f.editor.close();
  }
});

test('ordered source creation requires a listener and pre-notification persistence facts without consuming malformed calls',async()=>{
  const f=await oneOffNewTabFixture();
  assert.throws(()=>f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false}),/listener/);
  assert.equal(f.editor.getOneOffCommand(f.command.token),f.command);assert.equal(f.created.length,0);f.editor.close();
  const g=await oneOffSourceEffectsFixture();
  for(const value of [undefined,null,0,'false'])assert.throws(()=>g.editor.createOneOffSearchTabWithSourceEffects(g.command.token,
    {userTypedValue:'typed words',persistedSearchTerms:value}),/observed persistence/);
  assert.equal(g.editor.getOneOffCommand(g.command.token),g.command);assert.equal(g.created.length,0);g.editor.close();
});

test('source notification listener reentry cannot repeat a tab command or authorize later target text',async()=>{
  for(const mutation of ['caret','cancel','scope']){
    let f;f=await oneOffSourceEffectsFixture(()=>{
      assert.equal(f.editor.createOneOffSearchTab(f.command.token,{userTypedValue:'typed words'}),null);
      if(mutation==='caret')f.editor.select(1,0,0);
      if(mutation==='cancel')f.editor.cancel();
      if(mutation==='scope')f.loseScope();
    });
    const result=f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false});
    assert.equal(result.initialized,false);assert.equal(result.typedValueWritten,false);assert.equal(f.created.length,1);
    assert.equal(f.events.length,1);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('source notification listener exceptions are isolated like mode events and persistence getters cannot overwrite newer edits',async()=>{
  const errors=[],f=await oneOffSourceEffectsFixture(()=>{throw Error('host listener failed');});
  const old=f.sandbox.console;f.sandbox.console={...old,error:error=>errors.push(error)};
  const result=f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false});
  assert.equal(result.initialized,true);assert.equal(result.typedValueWritten,true);assert.equal(errors.length,1);f.editor.close();
  const g=await oneOffSourceEffectsFixture();
  assert.equal(g.editor.createOneOffSearchTabWithSourceEffects(g.command.token,{userTypedValue:'typed words',
    get persistedSearchTerms(){g.editor.select(1,0,0);return false;}}),null);
  assert.equal(g.created.length,0);assert.equal(g.events.length,0);assert.equal(g.editor.snapshot.selectionStart,0);g.editor.close();
});


async function oneOffTabActivationFixture(options={}){
  const f=await oneOffSourceEffectsFixture(()=>{},options);let selections=0;
  const created=f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false});
  assert.equal(created.initialized,true);
  f.parent.describeIndependentOneOffTabActivation=receipt=>({sourceBrowserId:17,targetBrowserId:18,searchMode:f.command.searchMode,userTypedValue:'typed words'});
  f.parent.activateIndependentOneOffSearchTab=(receipt,sourceCurrent,transferCurrent)=>{
    if(!sourceCurrent()||!transferCurrent())return null;
    selections++;f.onSelect?.();
    const current=transferCurrent();return {activated:true,current,sourceBrowserId:17,targetBrowserId:18,isSourceCleanupCurrent:()=>f.sourceCleanupCurrent!==false&&f.destinationCurrent!==false,destination:current?{stateToken:Object.freeze({}),context:{...f.context,browserId:18},searchMode:f.command.searchMode,userTypedValue:'typed words',documentId:42,isCurrent:()=>f.destinationCurrent!==false,isReady:()=>f.destinationCurrent!==false&&f.destinationReady!==false}:null};
  };
  const activation=f.editor.beginOneOffTabActivation(f.command.token);
  return Object.assign(f,{created,activation,getSelections:()=>selections});
}

test('foreground activation consumes once, tolerates deliberate source scope loss and never starts a query',async()=>{
  const f=await oneOffTabActivationFixture(),p=f.activation;
  assert.ok(p);assert.equal(p.targetBrowserId,18);assert.equal(f.editor.getOneOffTabActivation(p.token),p);
  f.onSelect=()=>{assert.equal(f.editor.activateOneOffSearchTab(p.token),null);f.loseScope();};
  const result=f.editor.activateOneOffSearchTab(p.token);
  assert.equal(result.current,true);assert.equal(result.destination.context.browserId,18);
  assert.equal(result.sourceDeselectionPending,true);assert.equal(result.destinationQueryPending,true);
  assert.equal(result.allowAutofill,true);assert.equal(f.getSelections(),1);assert.equal(f.parent.started.length,1);
  assert.equal(f.editor.activateOneOffSearchTab(p.token),null);f.editor.close();
});

test('background and target-only new-tab commands do not offer foreground activation',async()=>{
  const f=await oneOffTabActivationFixture({background:true});assert.equal(f.activation,null);assert.equal(f.getSelections(),0);f.editor.close();
  const g=await oneOffNewTabFixture();g.editor.createOneOffSearchTab(g.command.token,{userTypedValue:'typed words'});
  assert.equal(g.editor.beginOneOffTabActivation(g.command.token),null);g.editor.close();
});

test('activation preparation rejects stale caret, popup, engine policy, source and target receipts',async()=>{
  for(const mutation of ['caret','cancel','popup','engine','policy','parent','scope']){
    const f=await oneOffTabActivationFixture(),p=f.activation;
    if(mutation==='caret')f.editor.select(1,0,0);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='popup'){f.editor.publishPresentation(1,f.query.id,[],false);f.editor.publishPresentation(1,f.query.id,[],true);}
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'second'});
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='parent')f.parent.describeIndependentOneOffTabActivation=()=>null;
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.activateOneOffSearchTab(p.token),/scope changed/);}
    else assert.equal(f.editor.activateOneOffSearchTab(p.token),null,mutation);
    assert.equal(f.getSelections(),0);f.editor.close();
  }
});

test('activation rechecks editor, query and engine eligibility during selection callbacks',async()=>{
  for(const mutation of ['caret','cancel','engine','policy','close','throw']){
    const f=await oneOffTabActivationFixture(),p=f.activation;
    f.onSelect=()=>{
      if(mutation==='caret')f.editor.select(1,0,0);
      if(mutation==='cancel')f.editor.cancel();
      if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:'second'});
      if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
      if(mutation==='close')f.editor.close();
      if(mutation==='throw')throw Error('activation callback failed');
    };
    if(mutation==='throw')assert.throws(()=>f.editor.activateOneOffSearchTab(p.token),/callback failed/);
    else {const result=f.editor.activateOneOffSearchTab(p.token);assert.equal(result.current,false);assert.equal(result.destination,null);}
    assert.equal(f.editor.activateOneOffSearchTab(p.token),null);assert.equal(f.getSelections(),1);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('query activation refuses forged or foreign new-tab receipts',async()=>{
  const f=fixture({browserId:17});
  assert.equal(f.session.describeOneOffTabActivation({initialized:true,sourceModeNotified:true,requiresActivation:true}),null);
  assert.equal(f.session.activateOneOffSearchTab({},()=>true,()=>true),null);f.session.close();
});


async function oneOffDestinationFixture(options={}){
  const f=await oneOffTabActivationFixture(options);
  const result=f.editor.activateOneOffSearchTab(f.activation.token),destination=result.destination;
  const destinationEngines=f.parent.child.engineStore.getEngines().map(engine=>({...engine}));
  const sourceProposal=result.sourceDeselection;
  const deselection=f.editor.acknowledgeOneOffSourceDeselection(sourceProposal.token,
    {...sourceProposal.edit,searchMode:sourceProposal.searchMode,oneOffSelected:false});
  f.editor.close();
  const updates=[],modeEvents=[],editor=new f.sandbox.UrlbarEditSession({context:destination.context,isCurrent:destination.isCurrent,onUpdate:u=>updates.push(u),onModeStateChanged:e=>modeEvents.push(e)});
  const parent=f.parents.at(-1),observation={revision:1,text:destination.userTypedValue??'',selectionStart:0,
    selectionEnd:0,selectionDirection:'none',composing:false,pageProxyValid:false,searchMode:destination.searchMode};
  parent.initEngineStore=()=>parent.child.updateEngineStore('init',destinationEngines,0);
  return {...f,source:f.editor,editor,destination,parent,updates,modeEvents,observation,owner:f,destinationEngines,deselection};
}

test('activated destinations adopt once into matching fresh editors and explicitly query with local or engine policy',async()=>{
  for(const engine of [false,true]){
    const f=await oneOffDestinationFixture({engine,pageValid:engine});
    assert.equal(f.editor.snapshot,null);assert.equal(f.parent.started.length,0);
    assert.equal(f.editor.adoptOneOffDestination({...f.destination},f.observation),null);
    const p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    assert.ok(p);assert.equal(p.allowAutofill,!engine);assert.equal(f.editor.getOneOffDestinationQuery(p.token),p);
    assert.equal(f.editor.snapshot.text,f.observation.text);assert.equal(f.parent.started.length,0);assert.equal(f.parent.engagements.length,0);
    const other=new f.sandbox.UrlbarEditSession({context:f.destination.context,isCurrent:()=>true,onUpdate(){}});
    assert.equal(other.adoptOneOffDestination(f.destination,f.observation),null);other.close();
    const query=await f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode,isPrivate:false});
    assert.equal(query.searchString,f.observation.text);assert.equal(query.allowAutofill,!engine);assert.equal(query.isPrivate,true);
    assert.deepEqual(query.searchMode,p.searchMode);assert.equal(f.parent.started.length,1);assert.equal(f.parent.engagements.length,0);
    await assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),/Stale/);f.editor.close();
  }
});

test('destination adoption rejects wrong context, host state, composition and stale browser receipts without claiming them',async()=>{
  for(const mutation of ['browserId','actor','isPrivate','sapName']){
    const f=await oneOffDestinationFixture(),context={...f.destination.context};context[mutation]=mutation==='actor'?{browsingContext:{isContent:false}}:mutation==='browserId'?19:mutation==='isPrivate'?false:'searchbar';
    const other=new f.sandbox.UrlbarEditSession({context,isCurrent:()=>true,onUpdate(){}});
    assert.equal(other.adoptOneOffDestination(f.destination,f.observation),null);other.close();
    assert.ok(f.editor.adoptOneOffDestination(f.destination,f.observation));f.editor.close();
  }
  for(const changes of [{text:'different'},{composing:true},{pageProxyValid:true},{searchMode:null}]){
    const f=await oneOffDestinationFixture();assert.equal(f.editor.adoptOneOffDestination(f.destination,{...f.observation,...changes}),null);
    assert.equal(f.editor.snapshot,null);assert.ok(f.editor.adoptOneOffDestination(f.destination,f.observation));f.editor.close();
  }
  const f=await oneOffDestinationFixture();f.owner.destinationReady=false;
  assert.equal(f.editor.adoptOneOffDestination(f.destination,f.observation),null);assert.equal(f.editor.snapshot,null);f.editor.close();
});

test('destination observation getters cannot claim twice or overwrite an intervening editor',async()=>{
  for(const mutation of ['claim','edit','close','ready']){
    const f=await oneOffDestinationFixture();let claimed;
    const observation={...f.observation,get text(){
      if(mutation==='claim')claimed=f.editor.adoptOneOffDestination(f.destination,f.observation);
      if(mutation==='edit')void f.editor.edit({}, {revision:2,text:'new',selectionStart:3,selectionEnd:3,composing:true});
      if(mutation==='close')f.editor.close();if(mutation==='ready')f.owner.destinationReady=false;
      return f.observation.text;
    }};
    assert.equal(f.editor.adoptOneOffDestination(f.destination,observation),null);
    if(mutation==='claim')assert.equal(f.editor.getOneOffDestinationQuery(claimed.token),claimed);
    if(mutation==='edit')assert.equal(f.editor.snapshot.text,'new');
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('destination query rejects stale caret, cancellation, mode observations, browser state and scope',async()=>{
  for(const mutation of ['caret','cancel','state','ready','scope','getter']){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    let state={searchMode:p.searchMode};
    if(mutation==='caret')f.editor.select(1,1,1);
    if(mutation==='cancel')f.editor.cancel();if(mutation==='state')state={searchMode:null};
    if(mutation==='ready')f.owner.destinationReady=false;if(mutation==='scope')f.owner.destinationCurrent=false;
    if(mutation==='getter')state={searchMode:p.searchMode,get currentPage(){f.editor.select(1,1,1);return 'about:newtab';}};
    await assert.rejects(f.editor.queryOneOffDestination(p.token,state),/Stale|scope changed/);
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('engine-held destination queries recheck target authority and editor lifetime before provider dispatch',async()=>{
  for(const mutation of ['none','ready','caret','cancel','close','scope']){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    f.parent.initEngineStore=()=>{};
    const pending=f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
    const rejected=mutation==='none'?null:assert.rejects(pending,/authority|cancelled|closed|scope/);
    assert.equal(f.parent.started.length,0);
    if(mutation==='ready')f.owner.destinationReady=false;if(mutation==='caret')f.editor.select(1,1,1);
    if(mutation==='cancel')f.editor.cancel();if(mutation==='close')f.editor.close();if(mutation==='scope')f.owner.destinationCurrent=false;
    f.parent.child.updateEngineStore('init',[],null);
    if(rejected)await rejected;else await pending;
    assert.equal(f.parent.started.length,mutation==='none'?1:0,mutation);f.editor.close();
  }
});


test('destination admission records engagement before suppression and overflow and reuses the exact captured query',async()=>{
  for(const kind of ['dispatch','suppressed','overflow','programmatic']){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation),order=[];
    const original=f.parent.engagementEvent.start;let captured;
    f.parent.engagementEvent.start=(...args)=>{order.push('engagement');captured=args[1];original(...args);};
    const admission=f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode,browser:{currentPage:'about:newtab',userContextId:4}},{
      event:kind==='programmatic'?null:{type:'click',timeStamp:10},
      get suppressStartQuery(){order.push('suppression');return kind==='suppressed';},
      get inOverflowPanel(){order.push('overflow');return kind==='overflow';}});
    assert.equal(admission.disposition,kind==='programmatic'?'dispatch':kind);
    assert.deepEqual(order,kind==='programmatic'?['suppression','overflow']:kind==='suppressed'?['engagement','suppression']:['engagement','suppression','overflow']);
    assert.equal(f.parent.started.length,0);assert.equal(f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},{}),null);
    if(kind==='suppressed'||kind==='overflow'){
      await assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),new RegExp(kind));
      assert.equal(f.parent.started.length,0);
    }else{
      const q=await f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode,browser:{currentPage:'https://ignored.example/'}});
      assert.equal(q.id,admission.queryId);assert.equal(q.currentPage,'about:newtab');assert.equal(q.userContextId,4);
      if(captured)assert.equal(q,captured);assert.equal(f.parent.started.length,1);
    }
    f.editor.close();
  }
});

test('destination admission cannot be bypassed during callbacks or after malformed and throwing observations',async()=>{
  for(const mutation of ['event','suppression','overflow','throw','reentry']){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);let rejected;
    const options={event:{type:'click',timeStamp:1},suppressStartQuery:false,inOverflowPanel:false};
    if(mutation==='event')options.event={type:'bad',timeStamp:1};
    if(mutation==='suppression')delete options.suppressStartQuery;if(mutation==='overflow')delete options.inOverflowPanel;
    if(mutation==='throw')f.parent.engagementEvent.start=()=>{throw Error('recorder failed');};
    if(mutation==='reentry')f.parent.engagementEvent.start=()=>{
      assert.equal(f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},options),null);
      rejected=assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),/Stale/);
    };
    if(mutation==='reentry')assert.ok(f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},options));
    else assert.throws(()=>f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},options),/Invalid|Missing|recorder failed/);
    if(rejected)await rejected;
    assert.equal(f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},options),null);
    if(mutation!=='reentry')await assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),/Stale/);
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('destination admission rechecks browser and editor lifetime after recorder and option getters',async()=>{
  for(const phase of ['engagement','suppression','overflow','state'])for(const mutation of ['ready','caret','cancel']){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    const change=()=>{if(mutation==='ready')f.owner.destinationReady=false;if(mutation==='caret')f.editor.select(1,0,0);if(mutation==='cancel')f.editor.cancel();};
    const options={event:{type:'click',timeStamp:1},get suppressStartQuery(){if(phase==='suppression')change();return false;},
      get inOverflowPanel(){if(phase==='overflow')change();return false;}};
    if(phase==='engagement')f.parent.engagementEvent.start=change;
    const state={searchMode:p.searchMode,get browser(){if(phase==='state')change();return {};}};
    assert.equal(f.editor.startOneOffDestinationQuery(p.token,state,options),null);
    await assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),/Stale/);
    assert.equal(f.parent.started.length,0);f.editor.close();
  }
});

test('admitted destination queries preserve captured identity through engine waits and still reject stale target state',async()=>{
  for(const stale of [false,true]){
    const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    const admission=f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},{event:{type:'click',timeStamp:1},suppressStartQuery:false,inOverflowPanel:false});
    f.parent.initEngineStore=()=>{};
    const pending=f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
    const failure=stale?assert.rejects(pending,/authority/):null;
    if(stale)f.owner.destinationReady=false;f.parent.child.updateEngineStore('init',[],null);
    if(failure)await failure;else assert.equal((await pending).id,admission.queryId);
    assert.equal(f.parent.started.length,stale?0:1);f.editor.close();
  }
});

test('query-layer destination admission rejects copied, foreign, suppressed and replayed capabilities',async()=>{
  const f=fixture();const admission=f.session.startDestinationQuery({}, {searchString:'one-off',allowAutofill:true},
    {suppressStartQuery:false,inOverflowPanel:false},()=>true);
  await assert.rejects(f.session.queryDestination({...admission},()=>{},()=>true),/Stale/);
  const other=new f.sandbox.UrlbarQuerySession({context:f.context,isCurrent:()=>true});
  await assert.rejects(other.queryDestination(admission,()=>{},()=>true),/Stale/);other.close();
  assert.equal((await f.session.queryDestination(admission,()=>{},()=>true)).id,admission.queryId);
  await assert.rejects(f.session.queryDestination(admission,()=>{},()=>true),/Stale/);f.session.close();
});


async function backgroundSourceFixture({confirmedMode={source:5,isPreview:false,entry:'other'},restoreMode=true,background=true,pageValid=false}={}){
  const f=await oneOffSourceEffectsFixture(()=>{},{background,pageValid}),browser={token:{},typed:'typed words',confirmed:confirmedMode},writes=[];
  const initial=browser.token;
  f.parent.describeIndependentOneOffSourceRestoration=()=>browser.token===initial&&browser.typed==='typed words'?
    {searchMode:browser.confirmed,restoreMode,userTypedValue:browser.typed,stateToken:initial}:null;
  f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{
    if(!current()||browser.token!==facts.expectedStateToken)return null;
    browser.token={};browser.confirmed=mode;if(mode&&facts.modeEntry.pageProxyValid)browser.typed='';writes.push(mode);
    notify?.({browserId:id,searchMode:mode,userTypedValue:browser.typed,persistedSearchTerms:false});
    return {applied:true,current:current(),stateToken:browser.token,userTypedValue:browser.typed};
  };
  const receipt=f.editor.createOneOffSearchTabWithSourceEffects(f.command.token,{userTypedValue:'typed words',persistedSearchTerms:false});
  const entry={untrimmedValue:'typed words',userTypedValue:'typed words',pageProxyValid:pageValid,hasAutofillPlaceholder:false};
  const proposal=f.editor.beginBackgroundSourceRestoration(f.command.token,entry);
  return {...f,browser,writes,receipt,entry,proposal};
}

test('background source deselection restores owned confirmed local, engine, absent and removed modes without querying',async()=>{
  for(const confirmedMode of [{source:5,isPreview:false,entry:'other'},{engineName:'Second',isPreview:false,entry:'other'},null,
    {engineName:'Removed',isPreview:false,entry:'other'}]){
    const f=await backgroundSourceFixture({confirmedMode}),p=f.proposal,before=f.parent.started.length,cancelled=f.parent.cancelled;
    assert.ok(p);assert.equal(f.editor.getBackgroundSourceRestoration(p.token),p);
    const expected=confirmedMode?.engineName==='Removed'?null:confirmedMode;assert.deepEqual(JSON.parse(JSON.stringify(p.searchMode)),expected);
    const outcome=f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p));
    assert.equal(outcome.applied,true);assert.equal(outcome.modeRestored,true);assert.equal(outcome.sourceDeselectionPending,false);
    assert.deepEqual(JSON.parse(JSON.stringify(f.browser.confirmed)),expected);assert.equal(f.writes.length,1);assert.equal(f.events.length,2);
    assert.equal(f.events[1].reason,'one-off-background-source-restoration');assert.equal(f.editor.snapshot.text,'typed words');
    assert.equal(f.parent.started.length,before);assert.equal(f.parent.cancelled,cancelled);assert.equal(f.parent.engagements.length,0);
    assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)),null);f.editor.close();
  }
});

test('background source with no active mode acknowledges deselection without setter or notification; foreground commands are excluded',async()=>{
  const f=await backgroundSourceFixture({confirmedMode:null,restoreMode:false}),p=f.proposal,token=f.browser.token;
  const result=f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p));
  assert.equal(result.applied,true);assert.equal(result.modeRestored,false);assert.equal(f.browser.token,token);
  assert.equal(f.writes.length,0);assert.equal(f.events.length,1);assert.equal(f.parent.started.length,1);f.editor.close();
  const g=await backgroundSourceFixture({background:false});assert.equal(g.proposal,null);assert.equal(g.writes.length,0);g.editor.close();
});

test('background deselection requires exact host observations and source lifetime, and rejects forged receipts',async()=>{
  const f=await backgroundSourceFixture(),p=f.proposal,valid=oneOffRestorationObservation(p);
  for(const patch of [{text:'other'},{selectionStart:0},{composing:true},{pageProxyValid:true},{oneOffSelected:true},{searchMode:null}]){
    assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,{...valid,...patch}),null);assert.equal(f.writes.length,0);
  }
  assert.throws(()=>f.editor.commitBackgroundSourceRestoration(p.token,{...valid,persistedSearchTerms:undefined}),/Missing/);
  assert.equal(f.editor.commitBackgroundSourceRestoration({},valid),null);
  assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,valid).applied,true);f.editor.close();
  for(const mutation of ['caret','cancel','popup','typed','token','engine','policy','scope']){
    const g=await backgroundSourceFixture(),q=g.proposal;
    if(mutation==='caret')g.editor.select(1,0,0);if(mutation==='cancel')g.editor.cancel();
    if(mutation==='popup')g.editor.publishPresentation(1,g.query.id,[],false);
    if(mutation==='typed')g.browser.typed='newer';if(mutation==='token')g.browser.token={};
    if(mutation==='engine')g.parent.child.updateEngineStore('removed',{id:'second'});
    if(mutation==='policy')g.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='scope'){g.loseScope();assert.throws(()=>g.editor.commitBackgroundSourceRestoration(q.token,oneOffRestorationObservation(q)),/scope changed/);}
    else assert.equal(g.editor.commitBackgroundSourceRestoration(q.token,oneOffRestorationObservation(q)),null,mutation);
    assert.equal(g.writes.length,0);g.editor.close();
  }
  const h=fixture({browserId:17});assert.equal(h.session.describeBackgroundSourceRestoration({inBackground:true}),null);h.session.close();
});

test('background restoration consumes before reentrant setters and exceptions, while host getters cannot overwrite new edits',async()=>{
  for(const throws of [false,true]){
    const f=await backgroundSourceFixture(),p=f.proposal,commit=f.parent.commitIndependentModeState;
    f.parent.commitIndependentModeState=(...args)=>{
      assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)),null);
      if(throws)throw Error('source setter failed');return commit(...args);
    };
    if(throws)assert.throws(()=>f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)),/setter failed/);
    else assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)).applied,true);
    assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)),null);f.editor.close();
  }
  const f=await backgroundSourceFixture(),p=f.proposal;
  assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,{...oneOffRestorationObservation(p),get text(){f.editor.select(1,0,0);return p.edit.text;}}),null);
  assert.equal(f.writes.length,0);assert.equal(f.editor.snapshot.selectionStart,0);f.editor.close();
});


test('background source formatting requires the exact restored display and metadata before mode effects',async()=>{
  const f=await backgroundSourceFixture();let description={value:'display words',untrimmedValue:'typed words',protocolIsTrimmed:true,wwwIsTrimmed:false};
  f.parent.describeIndependentCompositionRestoration=()=>({...description});
  const p=f.editor.beginBackgroundSourceRestoration(f.command.token,{...f.entry,hasAutofillPlaceholder:true});
  assert.equal(p.editorWrite.text,'display words');
  assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,{...oneOffRestorationObservation(p),protocolIsTrimmed:false}),null);
  assert.equal(f.writes.length,0);
  const result=f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p));
  assert.equal(result.applied,true);assert.equal(f.editor.snapshot.text,'display words');assert.equal(f.writes.length,1);f.editor.close();
  const g=await backgroundSourceFixture();g.parent.describeIndependentCompositionRestoration=()=>({...description});
  const q=g.editor.beginBackgroundSourceRestoration(g.command.token,{...g.entry,hasAutofillPlaceholder:true});
  description={...description,value:'new display'};
  assert.equal(g.editor.commitBackgroundSourceRestoration(q.token,oneOffRestorationObservation(q)),null);assert.equal(g.writes.length,0);g.editor.close();
});


test('valid-page background sources clear text only when restoring a live confirmed mode',async()=>{
  for(const kind of ['local','engine','absent','removed','inactive']){
    const confirmedMode=kind==='engine'?{engineName:'Second',isPreview:false,entry:'other'}:kind==='removed'?{engineName:'Removed',isPreview:false,entry:'other'}:
      ['absent','inactive'].includes(kind)?null:{source:5,isPreview:false,entry:'other'};
    const f=await backgroundSourceFixture({confirmedMode,restoreMode:kind!=='inactive',pageValid:true}),p=f.proposal;
    assert.ok(p);const clears=['local','engine'].includes(kind);
    assert.equal(p.editorWrite?.text,clears?'':undefined);assert.equal(p.pageProxyValid,!clears);
    const observation={...oneOffRestorationObservation(p),pageProxyValid:p.pageProxyValid};
    const result=f.editor.commitBackgroundSourceRestoration(p.token,observation);
    assert.equal(result.applied,true);assert.equal(result.modeRestored,kind!=='inactive');
    assert.equal(f.editor.snapshot.text,clears?'':'typed words');assert.equal(f.browser.typed,clears?'':'typed words');
    assert.equal(f.writes.length,kind==='inactive'?0:1);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('valid-page restoration requires exact cleared text, caret, formatting and proxy observations before effects',async()=>{
  const f=await backgroundSourceFixture({pageValid:true}),p=f.proposal,valid={...oneOffRestorationObservation(p),pageProxyValid:p.pageProxyValid};
  for(const patch of [{text:'page address'},{selectionEnd:1},{pageProxyValid:true},{untrimmedValue:'page address'},
    {protocolIsTrimmed:true},{wwwIsTrimmed:true},{valueIsTyped:true},{modeEntry:{...p.modeEntry,pageProxyValid:false}}]){
    assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,{...valid,...patch}),null);assert.equal(f.writes.length,0);
  }
  assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,valid).applied,true);f.editor.close();
});

test('source page validity is pinned to the original command and valid-page clearing skips placeholder restoration',async()=>{
  for(const pageValid of [false,true]){
    const f=await backgroundSourceFixture({pageValid});
    assert.equal(f.editor.beginBackgroundSourceRestoration(f.command.token,{...f.entry,pageProxyValid:!pageValid}),null);
    f.parent.describeIndependentCompositionRestoration=()=>assert.fail('valid page queried placeholder restoration');
    if(pageValid){
      const p=f.editor.beginBackgroundSourceRestoration(f.command.token,{...f.entry,hasAutofillPlaceholder:true});
      assert.equal(p.restoration,null);assert.equal(p.editorWrite.text,'');
      assert.equal(f.editor.commitBackgroundSourceRestoration(p.token,oneOffRestorationObservation(p)).applied,true);
    }
    f.editor.close();
  }
});


test('destination dispatch retains activated engine identity across store removal, rename, replacement and policy changes',async()=>{
  for(const admitted of [false,true])for(const mutation of ['none','removed','renamed','replaced','mode-metadata','irrelevant','one-off-hidden']){
    const f=await oneOffDestinationFixture({engine:true,pageValid:true}),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    assert.equal(p.engineId,'default');await f.parent.child.engineStore.init();
    if(admitted)assert.ok(f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},{suppressStartQuery:false,inOverflowPanel:false}));
    const child=f.parent.child,engine=child.engineStore.getEngineByName('Default');
    if(mutation==='removed'||mutation==='replaced')child.updateEngineStore('removed',{id:engine.id});
    if(mutation==='renamed')child.updateEngineStore('changed',{...engine,name:'Renamed'},-1);
    if(mutation==='replaced')child.updateEngineStore('changed',{...engine,id:'replacement'},-1);
    if(mutation==='mode-metadata')child.updateEngineStore('changed',{...engine,isGeneralPurposeEngine:true},-1);
    if(mutation==='irrelevant')child.updateEngineStore('changed',{id:'unrelated',name:'Unrelated'},-1);
    if(mutation==='one-off-hidden')child.updateEngineStore('changed',{...engine,hideOneOffButton:true},-1);
    const rejected=['removed','renamed','replaced','mode-metadata'].includes(mutation);
    const query=f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode,engineId:'replacement'});
    if(rejected)await assert.rejects(query,/authority/);else assert.equal((await query).searchMode.engineName,'Default');
    assert.equal(f.parent.started.length,rejected?0:1,mutation);
    child.updateEngineStore('changed',{...f.destinationEngines[0]},-1);
    await assert.rejects(f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode}),/Stale/);f.editor.close();
  }
});

test('engine-held destination dispatch validates the resolved store snapshot rather than the old source store',async()=>{
  for(const admitted of [false,true])for(const mutation of ['none','removed','replaced','renamed','metadata']){
    const f=await oneOffDestinationFixture({engine:true,pageValid:true}),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    if(admitted)f.editor.startOneOffDestinationQuery(p.token,{searchMode:p.searchMode},{suppressStartQuery:false,inOverflowPanel:false});
    f.parent.initEngineStore=()=>{};
    const pending=f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
    const rejection=mutation==='none'?null:assert.rejects(pending,/authority/);assert.equal(f.parent.started.length,0);
    let engines=f.destinationEngines.map(engine=>({...engine}));
    if(mutation==='removed')engines=engines.filter(engine=>engine.id!=='default');
    if(mutation==='replaced')engines[0].id='replacement';if(mutation==='renamed')engines[0].name='Renamed';
    if(mutation==='metadata')engines[0].isGeneralPurposeEngine=true;
    f.parent.child.updateEngineStore('init',engines,0);
    if(rejection)await rejection;else await pending;assert.equal(f.parent.started.length,mutation==='none'?1:0);f.editor.close();
  }
});

test('activated local destinations remain independent of unrelated engine identity changes',async()=>{
  const f=await oneOffDestinationFixture(),p=f.editor.adoptOneOffDestination(f.destination,f.observation);
  assert.equal(p.engineId,null);await f.parent.child.engineStore.init();
  for(const engine of f.destinationEngines)f.parent.child.updateEngineStore('removed',{id:engine.id});
  const query=await f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
  assert.equal(query.searchMode.source,p.searchMode.source);assert.equal(f.parent.started.length,1);f.editor.close();
});


test('editor readiness observations require the genuine live foreground continuation and do not consume activation',async()=>{
  const f=await oneOffTabActivationFixture(),facts=Object.freeze({phase:'initial-document',documentId:42,requiresScopeTransfer:false});
  f.parent.describeIndependentOneOffTargetReadiness=()=>facts;
  assert.equal(f.editor.getOneOffTargetReadiness({}),null);assert.equal(f.editor.getOneOffTargetReadiness(f.command.token),facts);
  assert.equal(f.editor.getOneOffTargetReadiness(f.command.token),facts);assert.equal(f.parent.started.length,1);assert.equal(f.getSelections(),0);
  assert.equal(f.editor.activateOneOffSearchTab(f.activation.token).current,true);
  assert.equal(f.editor.getOneOffTargetReadiness(f.command.token),null);f.editor.close();
  const g=await oneOffTabActivationFixture({background:true});
  g.parent.describeIndependentOneOffTargetReadiness=()=>assert.fail('background receipt used foreground readiness');
  assert.equal(g.editor.getOneOffTargetReadiness(g.command.token),null);g.editor.close();
});

test('readiness facts cannot replace an editor or authorize activation of a changed document',async()=>{
  for(const mutation of ['caret','cancel','policy','parent-reentry','scope']){
    const f=await oneOffTabActivationFixture();f.parent.describeIndependentOneOffTargetReadiness=()=>{
      if(mutation==='parent-reentry')f.editor.select(1,0,0);return Object.freeze({phase:'scope-transfer-required',requiresScopeTransfer:true});};
    if(mutation==='caret')f.editor.select(1,0,0);if(mutation==='cancel')f.editor.cancel();
    if(mutation==='policy')f.prefs['scotchBonnet.disableOneOffs']=true;
    if(mutation==='scope'){f.loseScope();assert.throws(()=>f.editor.getOneOffTargetReadiness(f.command.token),/scope changed/);}
    else assert.equal(f.editor.getOneOffTargetReadiness(f.command.token),null);
    assert.equal(f.getSelections(),0);assert.equal(f.parent.started.length,1);f.editor.close();
  }
  const f=await oneOffTabActivationFixture();f.parent.describeIndependentOneOffTargetReadiness=()=>Object.freeze({phase:'scope-transfer-required',requiresScopeTransfer:true});
  f.parent.describeIndependentOneOffTabActivation=()=>null;
  assert.equal(f.editor.getOneOffTargetReadiness(f.command.token).requiresScopeTransfer,true);
  assert.equal(f.editor.activateOneOffSearchTab(f.activation.token),null);assert.equal(f.getSelections(),0);f.editor.close();
});


test('foreground source deselection acknowledges exactly once after deliberate source scope loss',async()=>{
  const f=await oneOffTabActivationFixture();f.onSelect=()=>f.loseScope();
  const result=f.editor.activateOneOffSearchTab(f.activation.token),p=result.sourceDeselection;
  assert.ok(p);assert.equal(Object.isFrozen(p),true);assert.equal(f.editor.getOneOffSourceDeselection(p.token),p);
  const observed={...p.edit,searchMode:p.searchMode,oneOffSelected:false};
  for(const change of [{revision:99},{text:'other'},{selectionStart:0},{selectionDirection:'backward'},{composing:true},{searchMode:{}},{oneOffSelected:true}])
    assert.equal(f.editor.acknowledgeOneOffSourceDeselection(p.token,{...observed,...change}),null);
  assert.equal(f.editor.getOneOffSourceDeselection({...p.token}),null);
  const ack=f.editor.acknowledgeOneOffSourceDeselection(p.token,observed);
  assert.equal(ack.acknowledged,true);assert.equal(ack.sourceDeselectionPending,false);assert.equal(ack.modeRestorationPending,true);
  assert.equal(f.editor.acknowledgeOneOffSourceDeselection(p.token,observed),null);
  assert.equal(f.parent.started.length,1);assert.equal(f.getSelections(),1);f.editor.close();
});

test('foreground source acknowledgment rejects closed, stale and reentrant cleanup without revoking destination scope',async()=>{
  for(const mutation of ['source','destination','close','getter']){
    const f=await oneOffTabActivationFixture();f.onSelect=()=>f.loseScope();
    const result=f.editor.activateOneOffSearchTab(f.activation.token),p=result.sourceDeselection;
    const observed={...p.edit,searchMode:p.searchMode,oneOffSelected:false};
    if(mutation==='source')f.sourceCleanupCurrent=false;
    if(mutation==='destination')f.destinationCurrent=false;
    if(mutation==='close')f.editor.close();
    if(mutation==='getter')Object.defineProperty(observed,'oneOffSelected',{get(){f.editor.close();return false;}});
    assert.equal(f.editor.acknowledgeOneOffSourceDeselection(p.token,observed),null,mutation);
    if(mutation!=='destination')assert.equal(result.destination.isReady(),true);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});


test('foreground deselection hands restoration to the authentic adopted destination only after its query',async()=>{
  for(const engine of [false,true]){
    const f=await oneOffDestinationFixture({engine,pageValid:engine}),ack=f.deselection;
    assert.equal(ack.modeRestorationBrowserId,18);assert.equal(ack.sourceBrowserId,17);
    assert.equal('sourceModeRestorationPending' in ack,false);
    assert.equal(f.editor.getOneOffDestinationRestoration(ack),null);
    const p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    assert.equal(f.editor.getOneOffDestinationRestoration(ack),null);
    const query=await f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
    const restoration=f.editor.getOneOffDestinationRestoration(ack);
    assert.ok(restoration);assert.equal(restoration.browserId,18);assert.equal(restoration.queryId,query.id);
    assert.equal(restoration.searchMode,p.searchMode);assert.equal(restoration.engineId,p.engineId);
    assert.equal(restoration.modeRestorationPending,true);assert.equal(restoration.startQuery,false);
    assert.equal(Object.isFrozen(restoration),true);assert.equal(f.editor.getOneOffDestinationRestoration(ack),restoration);
    assert.equal(f.editor.getOneOffDestinationRestoration({...ack}),null);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('destination restoration descriptions reject foreign sessions and stale editor, query, engine or browser authority',async()=>{
  for(const mutation of ['caret','cancel','ready','engine','foreign']){
    const f=await oneOffDestinationFixture({engine:true,pageValid:true});
    const p=f.editor.adoptOneOffDestination(f.destination,f.observation);
    await f.editor.queryOneOffDestination(p.token,{searchMode:p.searchMode});
    assert.ok(f.editor.getOneOffDestinationRestoration(f.deselection));
    if(mutation==='caret')f.editor.select(1,1,1);
    if(mutation==='cancel')f.editor.cancel();
    if(mutation==='ready')f.owner.destinationReady=false;
    if(mutation==='engine')f.parent.child.updateEngineStore('removed',{id:p.engineId});
    if(mutation==='foreign'){
      const other=new f.sandbox.UrlbarEditSession({context:f.destination.context,isCurrent:f.destination.isCurrent,onUpdate(){}});
      assert.equal(other.getOneOffDestinationRestoration(f.deselection),null);other.close();
    }else assert.equal(f.editor.getOneOffDestinationRestoration(f.deselection),null,mutation);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});


async function destinationRestorationCommitFixture(options={}){
  const f=await oneOffDestinationFixture(options),adopted=f.editor.adoptOneOffDestination(f.destination,f.observation);
  await f.editor.queryOneOffDestination(adopted.token,{searchMode:adopted.searchMode});
  const p=f.editor.getOneOffDestinationRestoration(f.deselection),writes=[];
  f.parent.commitIndependentModeState=(browserId,mode,facts,current,notify)=>{
    if(!current()||facts.expectedStateToken!==f.destination.stateToken)return null;
    writes.push({browserId,mode,facts});f.owner.destinationReady=false;
    assert.equal(current(),true,'own state commit must not invalidate remaining effects');
    f.duringCommit?.();if(current())notify?.({browserId,searchMode:mode,persistedSearchTerms:false,userTypedValue:p.edit.text});
    return {applied:true,stateToken:{},modeChangeNotified:current(),modeEntryNotified:false,engineUsageNotified:!!facts.engineId};
  };
  const observed={token:p.token,...p.edit,searchMode:p.searchMode,pageProxyValid:false,oneOffSelected:false,persistedSearchTerms:false,
    modeEntry:{untrimmedValue:p.edit.text,pageProxyValid:false,hasAutofillPlaceholder:false,userTypedValue:f.destination.userTypedValue}};
  return {...f,p,writes,observed};
}

test('destination restoration commits its owned confirmed mode once without another query or source write',async()=>{
  for(const engine of [false,true]){
    const f=await destinationRestorationCommitFixture({engine,pageValid:engine});
    const result=f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed);
    assert.equal(result.applied,true);assert.equal(result.modeRestorationPending,false);assert.equal(result.engineUsageNotified,engine);
    assert.equal(f.writes.length,1);assert.equal(f.writes[0].browserId,18);assert.equal(f.writes[0].facts.expectedStateToken,f.destination.stateToken);
    assert.equal(f.modeEvents.length,1);assert.equal(f.modeEvents[0].reason,'one-off-destination-restoration');
    assert.equal(f.editor.getOneOffDestinationRestoration(f.deselection),null);
    assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),null);
    assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('destination restoration rejects mismatched host state, formatting and reentrant observations before mutation',async()=>{
  const f=await destinationRestorationCommitFixture();
  for(const change of [{token:{}},{revision:99},{text:'other'},{selectionStart:1},{selectionDirection:'backward'},{composing:true},
    {pageProxyValid:true},{oneOffSelected:true},{searchMode:null},
    {modeEntry:{...f.observed.modeEntry,hasAutofillPlaceholder:true}},
    {modeEntry:{...f.observed.modeEntry,userTypedValue:'other'}},{modeEntry:{...f.observed.modeEntry,untrimmedValue:'other'}}])
    assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,{...f.observed,...change}),null);
  assert.equal(f.writes.length,0);
  const observed={...f.observed,get oneOffSelected(){f.editor.select(1,1,1);return false;}};
  assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,observed),null);assert.equal(f.writes.length,0);f.editor.close();
});

test('destination restoration consumes failed setters and closes safely during effects',async()=>{
  for(const failure of ['null','throw','close']){
    const f=await destinationRestorationCommitFixture();let calls=0;
    f.parent.commitIndependentModeState=(id,mode,facts,current,notify)=>{calls++;
      assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),null);
      if(failure==='throw')throw Error('setter failed');if(failure==='null')return null;
      f.editor.close();assert.equal(current(),false);return {applied:true,modeChangeNotified:false};};
    if(failure==='throw')assert.throws(()=>f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),/setter failed/);
    else{const outcome=f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed);assert.equal(outcome?.applied??false,failure==='close');}
    if(failure!=='close')assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),null);
    assert.equal(calls,1);assert.equal(f.modeEvents.length,0);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});


async function destinationFormattingFixture(){
  const f=await destinationRestorationCommitFixture();
  f.parent.describeIndependentCompositionRestoration=()=>({value:'formatted.test',untrimmedValue:f.p.edit.text,
    protocolIsTrimmed:true,wwwIsTrimmed:false});
  const entry={...f.observed.modeEntry,hasAutofillPlaceholder:true};
  const application=f.editor.beginOneOffDestinationModeApplication(f.deselection,entry);
  assert.ok(application);
  const observation={...application.edit,...application.editorWrite,searchMode:application.searchMode,modeEntry:application.modeEntry,
    pageProxyValid:false,oneOffSelected:false,persistedSearchTerms:false};
  return {...f,application,observation};
}

test('destination formatting separates exact display acknowledgment from the original full browser text',async()=>{
  const f=await destinationFormattingFixture(),p=f.application;
  assert.equal(f.editor.snapshot.text,f.p.edit.text);assert.equal(f.writes.length,0);
  assert.equal(f.editor.getOneOffDestinationModeApplication(f.deselection,p.token),p);
  for(const change of [{text:'wrong'},{selectionStart:0},{protocolIsTrimmed:false},{wwwIsTrimmed:true},{untrimmedValue:'wrong'},
    {valueIsTyped:true},{pageProxyValid:true},{oneOffSelected:true},{modeEntry:{...p.modeEntry,userTypedValue:'wrong'}}])
    assert.equal(f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,{...f.observation,...change}),null);
  assert.equal(f.writes.length,0);
  const result=f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,f.observation);
  assert.equal(result.applied,true);assert.equal(result.modeRestorationPending,false);
  assert.equal(f.editor.snapshot.text,'formatted.test');assert.equal(f.writes[0].facts.modeEntry.untrimmedValue,f.p.edit.text);
  assert.equal(f.modeEvents.length,1);assert.equal(f.parent.started.length,1);
  assert.equal(f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,f.observation),null);
  assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),null);f.editor.close();
});

test('destination formatting rejects changed formatter output, superseded proposals and reentrant host reads',async()=>{
  for(const mutation of ['formatter','superseded','getter']){
    const f=await destinationFormattingFixture(),p=f.application;
    if(mutation==='formatter')f.parent.describeIndependentCompositionRestoration=()=>({value:'different'});
    if(mutation==='superseded')assert.ok(f.editor.beginOneOffDestinationModeApplication(f.deselection,p.modeEntry));
    if(mutation==='getter')Object.defineProperty(f.observation,'text',{get(){f.editor.select(1,1,1);return p.editorWrite.text;}});
    assert.equal(f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,f.observation),null,mutation);
    assert.equal(f.writes.length,0);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('destination mode application keeps valid-page transitions gated and accepts explicit no-format facts',async()=>{
  const f=await destinationRestorationCommitFixture();
  assert.equal(f.editor.beginOneOffDestinationModeApplication(f.deselection,{...f.observed.modeEntry,pageProxyValid:true}),null);
  const p=f.editor.beginOneOffDestinationModeApplication(f.deselection,f.observed.modeEntry);
  assert.equal(p.editorWrite,null);
  assert.equal(f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,{...f.observed,modeEntry:p.modeEntry}).applied,true);
  assert.equal(f.parent.started.length,1);f.editor.close();
});


test('missing destination engines require refreshed proposals and clear mode without engine-use effects',async()=>{
  const f=await destinationRestorationCommitFixture({engine:true,pageValid:true});
  f.parent.child.updateEngineStore('removed',{id:f.p.engineId});
  assert.equal(f.editor.getOneOffDestinationRestoration(f.deselection),null);
  assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,f.observed),null);
  const p=f.editor.beginOneOffDestinationRestoration(f.deselection);
  assert.equal(p.searchMode,null);assert.equal(p.engineId,null);assert.equal(p.requestedEngineId,f.p.engineId);
  assert.notEqual(p.token,f.p.token);
  const result=f.editor.commitOneOffDestinationRestoration(f.deselection,{...f.observed,token:p.token,searchMode:null});
  assert.equal(result.applied,true);assert.equal(result.engineUsageNotified,false);assert.equal(f.writes[0].mode,null);
  assert.equal(f.writes[0].facts.requestedSearchMode,f.p.searchMode);assert.equal(f.modeEvents[0].searchMode,null);
  assert.equal(f.parent.started.length,1);f.editor.close();
});

test('a returning engine or same-name replacement cannot reuse a missing-engine proposal',async()=>{
  for(const replacement of [false,true]){
    const f=await destinationRestorationCommitFixture({engine:true,pageValid:true});
    const engine=f.destinationEngines.find(engine=>engine.id===f.p.engineId);
    f.parent.child.updateEngineStore('removed',{id:engine.id});
    const missing=f.editor.beginOneOffDestinationRestoration(f.deselection);assert.equal(missing.searchMode,null);
    f.parent.child.updateEngineStore('changed',{...engine,id:replacement?'new-identity':engine.id},-1);
    assert.equal(f.editor.getOneOffDestinationRestoration(f.deselection),null);
    assert.equal(f.editor.commitOneOffDestinationRestoration(f.deselection,{...f.observed,token:missing.token,searchMode:null}),null);
    const refreshed=f.editor.beginOneOffDestinationRestoration(f.deselection);
    if(replacement)assert.equal(refreshed,null);else assert.equal(refreshed.engineId,engine.id);
    assert.equal(f.writes.length,0);assert.equal(f.parent.started.length,1);f.editor.close();
  }
});

test('clearing an unavailable engine does not request autofill formatting and consumes the shared handoff',async()=>{
  const f=await destinationRestorationCommitFixture({engine:true,pageValid:true});
  f.parent.child.updateEngineStore('removed',{id:f.p.engineId});f.editor.beginOneOffDestinationRestoration(f.deselection);
  f.parent.describeIndependentCompositionRestoration=()=>assert.fail('null mode must not restore autofill');
  const p=f.editor.beginOneOffDestinationModeApplication(f.deselection,{...f.observed.modeEntry,hasAutofillPlaceholder:true});
  assert.equal(p.searchMode,null);assert.equal(p.editorWrite,null);
  const outcome=f.editor.commitOneOffDestinationModeApplication(f.deselection,p.token,{...f.observed,searchMode:null,modeEntry:p.modeEntry});
  assert.equal(outcome.applied,true);assert.equal(outcome.engineUsageNotified,false);
  assert.equal(f.editor.beginOneOffDestinationRestoration(f.deselection),null);assert.equal(f.parent.started.length,1);f.editor.close();
});
