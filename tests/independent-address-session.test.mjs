import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

// Load the staged subclass over a fake retained base so its own decisions are
// exercised without a browser: what it owns, when it falls back, and what it
// delegates unchanged.
function load(){
  const calls=[];
  class Base {
    constructor(window){this.window=window;this.closed=false;this.sessionId='s';this.revision=1;this.sequence=1;this.active=null;
      this.nativeOpen=true;this.input=window.gURLBar;this.nativeVisible=[];}
    query(...args){calls.push(['super.query',...args]);return Promise.resolve('retained');}
    activate(...args){calls.push(['super.activate',...args]);return 'retained-activated';}
    reportNativeVisibility(...args){calls.push(['super.visibility',...args]);return 'retained-visible';}
    cancel(){calls.push(['super.cancel']);}
    close(){calls.push(['super.close']);this.closed=true;}
    onQueryStarted(query){this.active.context=query;calls.push(['started',query.id]);}
    onFirstResult(query){calls.push(['first',query.id]);}
    onQueryResults(query){calls.push(['results',query.id]);}
    onQueryFinished(query){calls.push(['finished',query.id]);}
    snapshot(){return {snapshot:true};}
    canActivate(result){return result.activatable!==false;}
    nativeKeywordResult(result){return result.kind==='keyword';}
    nativeOmniboxResult(result){return result.kind==='omnibox';}
    updateSelection(request){calls.push(['super.updateSelection',request]);return {status:'selected'};}
    captureAutofill(active){calls.push(['super.capture']);}
  }
  const context=vm.createContext({console,ChromeUtils:{importESModule:()=>({UrlbarEditSession:class {}})}});
  const source=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIIndependentUrlbarSession.sys.mjs',import.meta.url),'utf8')
    .replace(/^import .*;$/m,'').replace('export class WinUIIndependentUrlbarSession','this.Session = class WinUIIndependentUrlbarSession')
    .replace('extends WinUIUrlbarSession','extends Base');
  context.Base=Base;
  vm.runInContext(source,context);
  return {Session:context.Session,calls};
}

function fixture({visible=true,page='https://page.test/'}={}){
  const {Session,calls}=load();
  const browser={browserId:7,currentURI:{spec:page},browsingContext:{currentWindowGlobal:{innerWindowId:2}}};
  const published=[],engagements=[];
  const view={nativePresentation:null,close:o=>calls.push(['view.close',o])};
  const inputField={value:'typed',selectionStart:5,selectionEnd:5,selectionDirection:'none',
    setSelectionRange(start,end,direction='none'){this.selectionStart=start;this.selectionEnd=end;this.selectionDirection=direction;}};
  const input={view,inputField,controller:{visibleResults:[]},untrimmedValue:'typed',
    setNativeInputValue(v){this.inputField.value=v;calls.push(['native-value',v]);},handleRevert(){calls.push(['revert']);}};
  const window={gBrowser:{selectedBrowser:browser,selectedTab:{isEmpty:false,userContextId:0}},gURLBar:input,
    KeyboardEvent:class {constructor(type,init){this.type=type;Object.assign(this,init);}},performance:{now:()=>1},
    windowGlobalChild:{getActor:()=>({})}};
  const s=new Session(window);
  view.nativePresentation=s;
  const result={id:11,type:1,payload:{url:'https://result.test/'}};
  const row={id:'s:1:1:0'};
  s.active={revision:1,text:'typed',inputValue:'typed',source:'independent',browser,page,context:{id:5,results:[result],heuristicResult:result},
    rows:[row],results:[result],resultIndexes:[0]};
  s.nativeVisible=visible?[row.id]:[];
  s.editorRevision=3;
  const editor={
    publishPresentation:(...a)=>published.push(a),
    startEngagement:(...a)=>engagements.push(['start',...a]),discardEngagement:()=>engagements.push(['discard']),
    beginNavigationActivation:()=>({token:'sel'}),
    acknowledgeNavigationActivation:()=>({token:'write',value:'https://result.test/',searchMode:null}),
    acknowledgeNavigationWrite:()=>true,
    acknowledgeOneOffExternalNavigation:(token,o)=>{calls.push(['external-input',o]);return token.external!==false;},
    prepareOneOffExternalRevert:(token,event,o)=>{calls.push(['external-revert',event,o]);
      return token.revert===null?null:{token:{...token},value:'https://page.test/',selectionStart:0,selectionEnd:0,focused:o.focused,pageProxyValid:true};},
    acknowledgeOneOffExternalRevert:(token,o)=>{calls.push(['external-restored',o]);return token.restore!==false;},
    commitOneOffExternalNavigation:()=>{calls.push(['external-commit']);return {attempted:true,reverted:false};},
    commitAcknowledgedNavigation:()=>({attempted:true,reverted:false}),
    cancel:()=>calls.push(['editor.cancel']),close:()=>calls.push(['editor.close']),
    getCompletion:token=>token.proposal,
    acknowledgeCompletion:(token,write)=>{calls.push(['ack',write]);return token.accept!==false;},
    observeAutofillSelection:(...a)=>{calls.push(['observe',...a]);return {confirmed:editor.confirms===true};},
  };
  s.editor=editor;
  return {s,calls,published,engagements,editor,result,row,window,input,browser};
}

test('the staged session reports its engine and the operations it owns',()=>{
  const {s}=fixture();
  assert.equal(s.engine,'independent');
  assert.deepEqual([...s.independentOperations],['query','visibility','activate']);
});

test('unmodified activation runs the editor handoff and counts one activation',()=>{
  const f=fixture(),counters=f.s.constructor.counters,before=counters.activations;
  assert.equal(f.s.activate(1,f.row.id,{}),'activated');
  assert.equal(counters.activations,before+1);
  assert.deepEqual(f.engagements[0].slice(0,3),['start',3,5]);
  assert.equal(f.s.active,null);assert.equal(f.s.revision,2);
  assert.ok(f.calls.some(c=>c[0]==='view.close'&&c[1].elementPicked===true));
  assert.equal(f.calls.some(c=>c[0]==='super.activate'),false);
});

test('Enter without a row id uses the selected or heuristic result',()=>{
  const f=fixture();
  assert.equal(f.s.activate(1,'',{}),'activated');
  const g=fixture();g.s.active.context.heuristicResult=null;
  assert.equal(g.s.activate(1,'',{}),'firefox-fallback');
  assert.equal(g.s.constructor.counters.fallbacks.at(-1),'no-result');
});

test('modified intents, hidden rows and rejected handoffs fall back with a reason',()=>{
  const last=f=>f.s.constructor.counters.fallbacks.at(-1);
  const hidden=fixture({visible:false});
  assert.equal(hidden.s.activate(1,hidden.row.id,{}),'firefox-fallback');
  assert.equal(last(hidden),'not-visible');
  for(const [method,reason] of [['beginNavigationActivation','unsupported-plan'],['acknowledgeNavigationActivation','selection-rejected'],
    ['acknowledgeNavigationWrite','write-rejected']]){
    const f=fixture();f.editor[method]=()=>null;
    assert.equal(f.s.activate(1,f.row.id,{}),'firefox-fallback');
    assert.equal(last(f),reason);
    assert.deepEqual(f.engagements.at(-1),['discard']);
    assert.notEqual(f.s.active,null,'a fallback before consumption keeps the query');
  }
  const thrown=fixture();thrown.editor.beginNavigationActivation=()=>{throw Error('boom');};
  assert.equal(thrown.s.activate(1,thrown.row.id,{}),'firefox-fallback');assert.equal(last(thrown),'error');
});

test('staleness and malformed intents are answered before any independent work',()=>{
  const f=fixture();
  assert.equal(f.s.activate(2,f.row.id,{}),'stale');
  assert.equal(f.s.activate(1,f.row.id,{unknown:true}),'invalid-intent');
  assert.equal(f.s.activate(1,f.row.id,{ctrlKey:'yes'}),'invalid-intent');
  f.input.inputField.value='edited';
  assert.equal(f.s.activate(1,f.row.id,{}),'stale');
  assert.equal(f.engagements.length,0);
});

test('rows built by inherited paths are activated by the retained code',()=>{
  const f=fixture();f.s.active.source=undefined;
  assert.equal(f.s.activate(1,f.row.id,{}),'retained-activated');
  assert.equal(f.s.reportNativeVisibility({ids:[]}),'retained-visible');
});

test('visibility reports publish the same results to the editor and keep the host contract',()=>{
  const f=fixture();
  const request={session:'s',revision:1,sequence:1,ids:[f.row.id]};
  assert.equal(f.s.reportNativeVisibility(request),'visible');
  assert.equal(JSON.stringify(f.published),JSON.stringify([[3,5,[f.result],true]]));
  assert.equal(JSON.stringify(f.s.nativeVisible),JSON.stringify([f.row.id]));
  assert.equal(JSON.stringify(f.input.controller.visibleResults),JSON.stringify([f.result]));
  for(const bad of [{...request,ids:['unknown']},{...request,revision:9},{...request,sequence:9},{...request,session:'x'},
    {...request,ids:[f.row.id,f.row.id]}]){
    assert.equal(f.s.reportNativeVisibility(bad),'stale');
  }
  f.editor.publishPresentation=()=>{throw Error('superseded');};
  assert.equal(f.s.reportNativeVisibility(request),'stale');
});

test('editor updates feed the inherited row builder and bind the query exactly once',()=>{
  const f=fixture();f.s.active.context=null;f.calls.length=0;
  f.s.onEditorUpdate({kind:'first-result',query:{id:9}});
  f.s.onEditorUpdate({kind:'results',query:{id:9}});
  f.s.onEditorUpdate({kind:'finished',query:{id:9}});
  f.s.onEditorUpdate({kind:'prepared',query:{id:9}});
  assert.deepEqual(f.calls.map(c=>c[0]),['started','first','results','finished','started']);
  f.calls.length=0;f.s.closed=true;f.s.onEditorUpdate({kind:'results',query:{id:9}});
  assert.equal(f.calls.length,0);
});

test('composition, non-edit and unowned queries use the retained path',async()=>{
  const f=fixture();
  assert.equal(await f.s.query('x',null,null),'retained');
  assert.equal(await f.s.query('x',null,{composing:true,selectionStart:0,selectionEnd:0}),'retained');
  f.input.view.nativePresentation=null;
  assert.equal(await f.s.query('x',null,{composing:false,selectionStart:0,selectionEnd:0}),'retained');
});

test('lifecycle cancels and closes the editor around the inherited behavior',()=>{
  const f=fixture();
  f.s.cancel();
  assert.deepEqual(f.calls.filter(c=>c[0].endsWith('cancel')).map(c=>c[0]),['editor.cancel','super.cancel']);
  f.calls.length=0;
  f.s.close();
  assert.deepEqual(f.calls.map(c=>c[0]),['super.close','editor.close']);
  assert.equal(f.s.editor,null);
  f.s.close();
  assert.equal(f.calls.filter(c=>c[0]==='super.close').length,1);
});

function completion(f,proposal,accept=true){
  const token={proposal,accept};
  return {kind:'first-result',query:{id:5},edit:{revision:f.s.editorRevision},completion:{...proposal,token}};
}
const suffix={kind:'autofill',value:'typed.test/',selectionStart:5,selectionEnd:11,selectionDirection:'backward'};

test('an autofill proposal is written, acknowledged and published as the native completion',()=>{
  const f=fixture();f.s.editId=4;
  const update=completion(f,suffix);
  f.s.onEditorUpdate(update);
  assert.equal(f.input.inputField.value,'typed.test/');
  assert.deepEqual([f.input.inputField.selectionStart,f.input.inputField.selectionEnd,f.input.inputField.selectionDirection],[5,11,'backward']);
  assert.equal(f.s.active.inputValue,'typed.test/');
  assert.deepEqual(JSON.parse(JSON.stringify(f.s.autofillResult)),{serial:1,editId:4,before:'typed',text:'typed.test/',start:5,end:11});
  assert.equal(f.calls.some(c=>c[0]==='super.capture'),false);
  assert.equal(f.calls.find(c=>c[0]==='ack')[1].selectionDirection,'backward');
});

test('a completion that is stale, unacknowledged or restoring does not leave a native completion',()=>{
  const staleText=fixture();staleText.input.inputField.value='changed';
  staleText.s.onEditorUpdate(completion(staleText,suffix));
  assert.equal(staleText.input.inputField.value,'changed');
  const staleEdit=fixture();const old=completion(staleEdit,suffix);old.edit.revision=1;
  staleEdit.s.onEditorUpdate(old);
  assert.equal(staleEdit.input.inputField.value,'typed');
  const rejected=fixture();
  rejected.s.onEditorUpdate(completion(rejected,suffix,false));
  assert.equal(rejected.input.inputField.value,'typed');
  assert.equal(rejected.s.active.inputValue,'typed');
  assert.equal(rejected.s.autofillResult??null,null);
  const restore=fixture();restore.s.autofillResult={serial:1};
  restore.s.onEditorUpdate(completion(restore,{kind:'restore',value:'typed',selectionStart:5,selectionEnd:5,selectionDirection:'none'}));
  assert.equal(restore.s.autofillResult,null);
});

test('activation after a completion uses the completed value as the observed text',()=>{
  const f=fixture();
  f.s.onEditorUpdate(completion(f,suffix));
  assert.equal(f.s.activate(1,f.row.id,{}),'activated');
});

test('caret acceptance is observed by the editor and confirms the typed value once',()=>{
  const f=fixture();f.editor.confirms=true;f.window.gBrowser.selectedBrowser.userTypedValue='typed';
  assert.equal(f.s.updateSelection({start:11,end:11,direction:'none'}).status,'selected');
  const observe=f.calls.find(c=>c[0]==='observe');
  assert.equal(observe[1],3);
  assert.equal(JSON.stringify(observe[2]),JSON.stringify({selectionStart:11,selectionEnd:11,selectionDirection:'none'}));
  assert.equal(JSON.stringify(observe[3]),JSON.stringify({userTypedValue:'typed'}));
  assert.equal(f.input.userTypedValue,f.input.inputField.value);
  const inherited=fixture();inherited.s.active.source=undefined;
  inherited.s.updateSelection({start:1,end:1,direction:'none'});
  assert.equal(inherited.calls.some(c=>c[0]==='observe'),false);
});

function externalFixture(extra={}){
  const f=fixture();
  const write={token:{...extra},phase:'input',value:'https://result.test/',selectionStart:20,selectionEnd:20,
    selectionDirection:'none',searchMode:null,pageProxyValid:false};
  f.editor.acknowledgeNavigationActivation=()=>write;
  return f;
}

test('modified intents reach the editor as activation modifiers on the Enter event',()=>{
  const f=fixture();let seen=null;
  f.editor.beginNavigationActivation=(r,q,res,event)=>{seen=event;return {token:'sel'};};
  f.s.activate(1,f.row.id,{ctrlKey:true,shiftKey:true});
  assert.equal(seen.key,'Enter');assert.equal(seen.ctrlKey,true);assert.equal(seen.shiftKey,true);assert.equal(seen.altKey,undefined);
});

test('a new-tab activation writes the result, restores the source and commits one external load',()=>{
  const f=externalFixture(),counters=f.s.constructor.counters,before=counters.activations;
  assert.equal(f.s.activate(1,f.row.id,{ctrlKey:true}),'activated');
  assert.equal(counters.activations,before+1);
  const names=f.calls.map(c=>c[0]).filter(n=>n.startsWith('external')||n==='revert');
  assert.deepEqual(names,['external-input','external-revert','external-restored','external-commit','revert']);
  const input=f.calls.find(c=>c[0]==='external-input')[1];
  assert.equal(input.text,'https://result.test/');assert.equal(input.pageProxyValid,false);assert.equal(input.searchMode,null);
  const restored=f.calls.find(c=>c[0]==='external-restored')[1];
  assert.equal(restored.text,'https://page.test/');assert.equal(restored.userTypedValue,null);assert.equal(restored.pageProxyValid,true);
  assert.equal(restored.persistedSearchTerms,false);
  assert.equal(f.s.active,null);assert.equal(f.s.revision,2);
  assert.ok(f.calls.some(c=>c[0]==='view.close'&&c[1].elementPicked===true));
});

test('a rejected or unavailable external stage falls back and restores the typed text',()=>{
  const last=f=>f.s.constructor.counters.fallbacks.at(-1);
  for(const [extra,reason] of [[{external:false},'input-rejected'],[{revert:null},'revert-unavailable'],[{restore:false},'revert-rejected']]){
    const f=externalFixture(extra);
    assert.equal(f.s.activate(1,f.row.id,{ctrlKey:true}),'firefox-fallback');
    assert.equal(last(f),reason);
    assert.equal(f.calls.some(c=>c[0]==='external-commit'),false);
    assert.equal(f.input.inputField.value,'typed');
    assert.notEqual(f.s.active,null);
  }
});

test('an omnibox result skips the input write and commits after the source is restored',()=>{
  const f=fixture();f.result.kind='omnibox';f.s.active.rows[0].keywordDocument=2;
  f.editor.beginResultOmnibox=()=>({token:{},phase:'input',value:'typed',selectionStart:5,selectionEnd:5,
    selectionDirection:'none',searchMode:null,pageProxyValid:false});
  assert.equal(f.s.activate(1,f.row.id,{}),'activated');
  const names=f.calls.map(c=>c[0]).filter(n=>n.startsWith('external'));
  assert.deepEqual(names,['external-revert','external-restored','external-commit']);
});

test('keyword and omnibox rows require the pinned document and an unlocked address',()=>{
  const wrong=fixture();wrong.result.kind='keyword';wrong.s.active.rows[0].keywordDocument=9;
  assert.equal(wrong.s.activate(1,wrong.row.id,{}),'stale');
  const missing=fixture();missing.result.kind='omnibox';
  assert.equal(missing.s.activate(1,missing.row.id,{}),'stale');
  const locked=fixture();locked.result.kind='keyword';locked.s.active.rows[0].keywordDocument=2;locked.input.readOnly=true;
  assert.equal(locked.s.activate(1,locked.row.id,{}),'stale');
  assert.equal(locked.engagements.length,0);
});

test('results the original input cannot activate fall back and refocus Firefox',()=>{
  const f=fixture();f.result.activatable=false;f.input.focus=()=>f.calls.push(['focus']);
  assert.equal(f.s.activate(1,f.row.id,{}),'firefox-fallback');
  assert.equal(f.s.constructor.counters.fallbacks.at(-1),'not-activatable');
  assert.ok(f.calls.some(c=>c[0]==='focus'));
});
