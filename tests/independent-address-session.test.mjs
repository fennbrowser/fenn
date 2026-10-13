import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

// Load the staged subclass over a fake retained base so its own decisions are
// exercised without a browser: what it owns, when it falls back, and what it
// delegates unchanged.
function load(){
  const calls=[],created=[];
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
    onQueryResultRemoved(id,l10n){calls.push(['removed',id,l10n]);}
    snapshot(){return {snapshot:true};}
    canActivate(result){return result.activatable!==false;}
    changed(){calls.push(['changed']);}
    resolveTipButton(request){calls.push(['resolve',request]);return request.resolved??null;}
    activateTipButton(request){calls.push(['super.tip',request]);return 'retained-tip';}
    activateGlobalAction(request){calls.push(['super.global',request]);return 'retained-global';}
    nativeKeywordResult(result){return result.kind==='keyword';}
    nativeOmniboxResult(result){return result.kind==='omnibox';}
    nativeModeResult(result){return result.kind==='mode';}
    updateSelection(request){calls.push(['super.updateSelection',request]);return {status:'selected'};}
    captureAutofill(active){calls.push(['super.capture']);}
    exposureRecorder(){return {addExposure(){calls.push(['retained-exposure']);},discardTentativeExposures(){}};}
  }
  const context=vm.createContext({console,UrlbarShared:{RESULT_TYPE:{DYNAMIC:8}},ChromeUtils:{importESModule:()=>({UrlbarEditSession:class {constructor(options){this.options=options;created.push(this);}close(){calls.push(['stale-editor-close']);}},independentUrlbarBrowserState:{snapshot:()=>({persistedSearchTerms:globalThis.r2Persisted===true})}})}});
  const source=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIIndependentUrlbarSession.sys.mjs',import.meta.url),'utf8')
    .replace(/^import .*;\r?$/gm,'').replace('export class WinUIIndependentUrlbarSession','this.Session = class WinUIIndependentUrlbarSession')
    .replace('extends WinUIUrlbarSession','extends Base');
  context.Base=Base;
  vm.runInContext(source,context);
  return {Session:context.Session,calls,created};
}

function fixture({visible=true,page='https://page.test/',heuristic=true}={}){
  const {Session,calls,created}=load();
  const browser={browserId:7,currentURI:{spec:page},browsingContext:{currentWindowGlobal:{innerWindowId:2}}};
  const published=[],engagements=[];
  const view={nativePresentation:null,close:o=>calls.push(['view.close',o])};
  const inputField={value:'typed',selectionStart:5,selectionEnd:5,selectionDirection:'none',
    setSelectionRange(start,end,direction='none'){this.selectionStart=start;this.selectionEnd=end;this.selectionDirection=direction;}};
  const input={view,inputField,controller:{visibleResults:[]},untrimmedValue:'typed',
    setNativeInputValue(v){this.inputField.value=v;calls.push(['native-value',v]);},handleRevert(){calls.push(['revert']);},handleCommand(e){calls.push(['handleCommand',e]);},
    searchMode:{engineName:'Held'},mirrorSearchMode(mode,browser){calls.push(['mirror',mode,browser]);}};
  const window={gBrowser:{selectedBrowser:browser,selectedTab:{isEmpty:false,userContextId:0}},gURLBar:input,
    KeyboardEvent:class {constructor(type,init){this.type=type;Object.assign(this,init);}},performance:{now:()=>1},
    windowGlobalChild:{getActor:()=>({})},gWinUIPane:{addressFallback(){calls.push(['address-fallback']);}}};
  const s=new Session(window);
  view.nativePresentation=s;
  const result={id:11,type:1,payload:{url:'https://result.test/'}};
  const row={id:'s:1:1:0'};
  s.active={revision:1,text:'typed',inputValue:'typed',source:'independent',browser,page,context:{id:5,results:[result],heuristicResult:heuristic?result:null},
    rows:[row],results:[result],resultIndexes:[0]};
  s.nativeVisible=visible?[row.id]:[];
  s.editorRevision=3;
  const editor={
    publishPresentation:(...a)=>published.push(a),
    addExposure:(...a)=>{calls.push(['editor-exposure',...a]);if(editor.exposureThrows)throw new Error('stale');},
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
    recordResultCommand:(...a)=>{calls.push(['command',a[4]]);return editor.commandOutcome===undefined?{recorded:true,sessionOngoing:true}:editor.commandOutcome;},
    beginResultHelp:(...a)=>{calls.push(['help',a[4]]);return {token:{},phase:'input',value:'typed',selectionStart:5,selectionEnd:5,
      selectionDirection:'none',searchMode:null,pageProxyValid:false};},
    getRestartRequest:token=>token.proposal??null,
    acknowledgeRestartQuery:async(token,o,state)=>{calls.push(['restart',o,state]);return token.restarted===false?null:{id:7};},
    beginResultModeApplication:(...a)=>{calls.push(['mode-begin',a[2],a[3]]);return editor.modeProposal===undefined?{token:{},value:'@',
      selectionStart:1,selectionEnd:1,selectionDirection:'none',searchMode:{engineName:'E',isPreview:false},modeEntry:undefined,
      pageProxyValid:undefined}:editor.modeProposal;},
    acknowledgeResultModeApplication:(token,o)=>{calls.push(['mode-ack',o]);return editor.modeAck!==false;},
    commitAcknowledgedModeState:(token,o)=>{calls.push(['mode-commit',o]);return editor.modeCommit===undefined?{applied:true}:editor.modeCommit;},
    requeryAfterModeApplication:async(token,state)=>{calls.push(['mode-requery',state]);return {id:12};},
    getCompletion:token=>token.proposal,
    acknowledgeCompletion:(token,write)=>{calls.push(['ack',write]);return token.accept!==false;},
    observeAutofillSelection:(...a)=>{calls.push(['observe',...a]);return {confirmed:editor.confirms===true};},
  };
  s.editor=editor;s.editorScope={browser,documentId:2,lost:false};
  return {s,calls,created,published,engagements,editor,result,row,window,input,browser};
}

test('the editor is rebuilt when the browser or document it was pinned to changes',()=>{
  const f=fixture();f.s.editor=null;
  const first=f.s.ensureEditor();
  assert.equal(f.s.ensureEditor(),first,'the same scope keeps the editor');
  assert.equal(f.created.length,1);
  f.browser.browsingContext.currentWindowGlobal.innerWindowId=3;
  const second=f.s.ensureEditor();
  assert.notEqual(second,first);
  assert.equal(f.calls.filter(c=>c[0]==='stale-editor-close').length,1,'the old editor is closed');
  assert.equal(first.options.isCurrent(),false,'the old scope is over');
  assert.equal(second.options.isCurrent(),true);
  f.window.gBrowser.selectedBrowser={browserId:8,currentURI:{spec:'https://other.test/'},browsingContext:{currentWindowGlobal:{innerWindowId:9}}};
  assert.notEqual(f.s.ensureEditor(),second,'a tab switch gets its own editor');
});

test('the editor context carries the window privacy',()=>{
  const normal=fixture();normal.s.editor=null;
  assert.equal(normal.s.ensureEditor().options.context.isPrivate,false);
  const priv=fixture();priv.s.editor=null;
  priv.window.PrivateBrowsingUtils={isWindowPrivate:win=>win===priv.window};
  assert.equal(priv.s.ensureEditor().options.context.isPrivate,true);
});

test('an editor whose scope was lost is not reused when the scope comes back',()=>{
  const f=fixture();f.s.editor=null;
  const first=f.s.ensureEditor();
  const original=f.window.gBrowser.selectedBrowser;
  f.window.gBrowser.selectedBrowser={browserId:8,browsingContext:{currentWindowGlobal:{innerWindowId:9}}};
  assert.equal(first.options.isCurrent(),false,'the editor sees another tab selected and closes itself');
  f.window.gBrowser.selectedBrowser=original;
  const again=f.s.ensureEditor();
  assert.notEqual(again,first,'switching back must not hand out the closed editor');
  assert.equal(again.options.isCurrent(),true);
});

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
  assert.equal(g.s.activate(1,'',{}),'activated','an Enter the editor cannot host is completed by Firefox');
  assert.equal(g.s.constructor.counters.fallbacks.at(-1),'no-result');
});

test('modified intents, hidden rows and rejected handoffs fall back with a reason',()=>{
  const last=f=>f.s.constructor.counters.fallbacks.at(-1);
  const hidden=fixture({visible:false,heuristic:false});
  assert.equal(hidden.s.activate(1,hidden.row.id,{}),'firefox-fallback');
  assert.equal(last(hidden),'not-visible');
  for(const [method,reason] of [['beginNavigationActivation','unsupported-plan'],['acknowledgeNavigationActivation','selection-rejected'],
    ['acknowledgeNavigationWrite','write-rejected']]){
    const f=fixture({heuristic:false});f.editor[method]=()=>null;
    assert.equal(f.s.activate(1,f.row.id,{}),'firefox-fallback');
    assert.equal(last(f),reason);
    assert.deepEqual(f.engagements.at(-1),['discard']);
    assert.notEqual(f.s.active,null,'a fallback before consumption keeps the query');
  }
  const thrown=fixture({heuristic:false});thrown.editor.beginNavigationActivation=()=>{throw Error('boom');};
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

function externalFixture(extra={},options={}){
  const f=fixture(options);
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
    const f=externalFixture(extra,{heuristic:false});
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
  const f=fixture({heuristic:false});f.result.activatable=false;f.input.focus=()=>f.calls.push(['focus']);
  assert.equal(f.s.activate(1,f.row.id,{}),'firefox-fallback');
  assert.equal(f.s.constructor.counters.fallbacks.at(-1),'not-activatable');
  assert.ok(f.calls.some(c=>c[0]==='focus'));
});

function tip(f,action,{help=false}={}){
  const button={action,enabled:true};
  const resolved={active:f.s.active,row:{id:f.row.id,tipButtons:[{}],helpLink:help?{}:null},button,result:f.result,intent:{},isHelp:help};
  return {version:1,resolved};
}

test('a tip URL button activates through the navigation handoff with its exact action',()=>{
  const f=fixture();let seen=null;
  f.editor.beginNavigationActivation=(r,q,res,event,options)=>{seen=options.resultAction;return {token:'sel'};};
  assert.equal(f.s.activateTipButton(tip(f,{url:'https://tip.test/',name:'0'})),'activated');
  assert.equal(JSON.stringify(seen),JSON.stringify({url:'https://tip.test/',name:'0'}));
  assert.ok(f.calls.some(c=>c[0]==='changed'));
});

test('a tip command is recorded as a command and leaves the view to the provider',()=>{
  const f=fixture();
  assert.equal(f.s.activateTipButton(tip(f,{command:'main',input:'ignored',name:'1'})),'activated');
  assert.equal(JSON.stringify(f.calls.find(c=>c[0]==='command')[1].resultAction),JSON.stringify({command:'main',name:'1'}));
  assert.notEqual(f.s.active,null);
  const ended=fixture();ended.editor.commandOutcome={recorded:true,sessionOngoing:false};
  assert.equal(ended.s.activateTipButton(tip(ended,{command:'manage'})),'activated');
  assert.notEqual(ended.s.active,null,'the retained path leaves the view to the provider');
  const rejected=fixture();rejected.editor.commandOutcome=null;
  assert.equal(rejected.s.activateTipButton(tip(rejected,{command:'main'})),'firefox-fallback');
  assert.ok(rejected.calls.some(c=>c[0]==='address-fallback'));
});

test('learn-more links navigate, help commands use the help handoff and a command with a URL is not guessed',()=>{
  const link=fixture();let linkAction=null;
  link.editor.beginNavigationActivation=(r,q,res,e,o)=>{linkAction=o.resultAction;return {token:'sel'};};
  assert.equal(link.s.activateTipButton(tip(link,{help:true,url:'https://help.test/'},{help:true})),'activated');
  assert.equal(JSON.stringify(linkAction),JSON.stringify({url:'https://help.test/'}));
  assert.equal(link.calls.some(c=>c[0]==='help'),false,'a learn-more link is not a help command');
  const command=fixture();
  assert.equal(command.s.activateTipButton(tip(command,{command:'help',name:'0'})),'activated');
  assert.equal(JSON.stringify(command.calls.find(c=>c[0]==='help')[1].resultAction),JSON.stringify({command:'help',name:'0'}));
  const guessed=fixture();
  assert.equal(guessed.s.activateTipButton(tip(guessed,{command:'manage',url:'https://x.test/'})),'firefox-fallback');
  assert.equal(guessed.s.constructor.counters.fallbacks.at(-1),'unsupported-action');
  assert.equal(guessed.calls.some(c=>c[0]==='command'),false);
});

test('stale tip requests and inherited rows keep their retained answers',()=>{
  const f=fixture();
  assert.equal(f.s.activateTipButton({version:1}),'stale');
  const inherited=fixture();inherited.s.active.source=undefined;
  assert.equal(inherited.s.activateTipButton({version:1}),'retained-tip');
});

test('a provider restart writes its value, admits that query and is acknowledged once',async()=>{
  const f=fixture();f.s.editId=3;
  const proposal={value:'rewritten',edit:{text:'typed',selectionDirection:'none'}};
  await f.s.onEditorPresentationRequest({kind:'restart-query',token:{proposal}});
  await new Promise(r=>setTimeout(r,0));
  assert.equal(f.input.inputField.value,'rewritten');
  assert.equal(f.calls.some(c=>c[0]==='native-value'&&c[1]==='rewritten'),false,'a provider value write is not typed text');
  const restart=f.calls.find(c=>c[0]==='restart');
  assert.equal(restart[1].text,'rewritten');assert.equal(restart[1].selectionStart,9);
  assert.equal(f.s.tipQueryRestart.text,'rewritten');assert.equal(f.s.tipQueryRestart.editId,3);
  f.s.onEditorUpdate({kind:'first-result',query:{id:99}});
  assert.ok(f.calls.some(c=>c[0]==='started'&&c[1]===99),'the restarted query rebinds the active edit');
});

test('restarts that cannot be hosted hand the surface back',async()=>{
  const rejected=fixture();
  await rejected.s.onEditorPresentationRequest({kind:'restart-query',token:{proposal:{value:null,edit:{text:'typed',
    selectionStart:5,selectionEnd:5,selectionDirection:'none'},restarted:false}}});
  const search=fixture();
  search.s.onEditorPresentationRequest({kind:'restart-query',token:{},search:{engineName:'x'}});
  await new Promise(r=>setTimeout(r,0));
  assert.ok(search.calls.some(c=>c[0]==='address-fallback'));
  const other=fixture();
  other.s.onEditorPresentationRequest({kind:'close'});
  await new Promise(r=>setTimeout(r,0));
  assert.equal(other.calls.some(c=>c[0]==='address-fallback'),false);
});

test('result menu items route to the editor flow that owns their kind',()=>{
  const event={type:'click'};
  const open=fixture();let seen=null;
  open.editor.beginNavigationActivation=(r,q,res,e,options)=>{seen={event:e,action:options.resultAction};return {token:'sel'};};
  open.s.invokeResultMenu(open.result,{openIn:'container-tab',userContextId:'3'},event);
  assert.equal(seen.event,event);
  assert.equal(JSON.stringify(seen.action),JSON.stringify({openIn:'container-tab',userContextId:'3'}));
  const command=fixture();
  command.s.invokeResultMenu(command.result,{command:'dismiss'},event);
  assert.equal(JSON.stringify(command.calls.find(c=>c[0]==='command')[1].resultAction),JSON.stringify({command:'dismiss'}));
  const help=fixture();help.result.payload.helpUrl='https://help.test/x';
  help.s.invokeResultMenu(help.result,{command:'help'},event);
  assert.equal(JSON.stringify(help.calls.find(c=>c[0]==='help')[1].resultAction),
    JSON.stringify({command:'help',url:'https://help.test/x'}));
});

test('local menu operations stay with the shared menu model and unknown ones hand the surface back',()=>{
  const local=fixture();let invoked=null;
  local.input.resultActions={invokeMenu:(...a)=>{invoked=a;}};
  local.s.invokeResultMenu(local.result,{command:'toggle-keyboard-accessible'},{});
  assert.equal(invoked[1].command,'toggle-keyboard-accessible');
  assert.equal(local.calls.some(c=>c[0]==='command'),false);
  const unknown=fixture();
  unknown.s.invokeResultMenu(unknown.result,{operation:'something-else'},{});
  assert.ok(unknown.calls.some(c=>c[0]==='address-fallback'));
  assert.equal(unknown.s.constructor.counters.fallbacks.at(-1),'unsupported-menu-action');
  const inherited=fixture();inherited.s.active.source=undefined;
  inherited.s.invokeResultMenu(inherited.result,{command:'dismiss'},{});
  assert.ok(inherited.calls.some(c=>c[0]==='address-fallback'));
});

test('provider removals and their acknowledgment text reach the inherited row builder for the current query',()=>{
  const f=fixture();
  f.s.onEditorPresentationRequest({kind:'remove-result',queryId:5,resultId:11,acknowledgeDismissalL10n:{id:'ack'}});
  assert.equal(JSON.stringify(f.calls.find(c=>c[0]==='removed').slice(1)),JSON.stringify([11,{id:'ack'}]));
  const other=fixture();
  other.s.onEditorPresentationRequest({kind:'remove-result',queryId:99,resultId:11});
  assert.equal(other.calls.some(c=>c[0]==='removed'),false);
});

function globalActionFixture(dataset){
  const f=fixture({heuristic:false});
  const action={key:'contextual-search',dataset};
  f.result.providerName='UrlbarProviderGlobalActions';f.result.payload.actionsResults=[action];
  f.s.active.rows[0].globalActions=[{id:'a1',key:'contextual-search'}];
  f.s.active.revision=1;f.s.nativeOpen=true;f.input.searchMode=null;
  f.request={id:f.row.id,actionId:'a1',revision:1,intent:{}};
  f.options=[];f.editor.beginNavigationActivation=(rev,q,res,event,options)=>{f.options.push(options);return {token:'sel'};};
  return f;
}

test('an immediate global action is hosted on the editor with its picked action key',()=>{
  const f=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  f.s.activateGlobalAction(f.request);
  assert.equal(f.options[0].pickedActionKey,'contextual-search');
  assert.equal(f.calls.some(c=>c[0]==='super.global'),false);
  assert.equal(f.calls.some(c=>c[0]==='address-fallback'),false);
});

test('mode-entering, stale and unsupported global actions use the inherited path or hand back',()=>{
  const mode=globalActionFixture({providesSearchMode:true});
  mode.s.activateGlobalAction(mode.request);
  assert.ok(mode.calls.some(c=>c[0]==='super.global'));
  const inMode=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  inMode.input.searchMode={engineName:'Held'};
  inMode.s.activateGlobalAction(inMode.request);
  assert.ok(inMode.calls.some(c=>c[0]==='super.global'),'a pick inside a search mode keeps the inherited path');
  assert.equal(inMode.options.length,0);
  const stale=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  stale.request.revision=9;
  assert.equal(stale.s.activateGlobalAction(stale.request),'stale');
  const badIntent=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  badIntent.request.intent={ctrlKey:'yes'};
  assert.equal(badIntent.s.activateGlobalAction(badIntent.request),'stale');
  const rejected=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  rejected.editor.beginNavigationActivation=()=>null;
  rejected.s.activateGlobalAction(rejected.request);
  assert.ok(rejected.calls.some(c=>c[0]==='address-fallback'));
  const tip=globalActionFixture({providesSearchMode:true,immediateSearch:true});
  tip.request.id='s:1:1:0:ack:x';
  tip.s.activateGlobalAction(tip.request);
  assert.ok(tip.calls.some(c=>c[0]==='super.global'));
});

test('a dynamic provider pick inside a search mode uses the inherited pick',()=>{
  const f=fixture({heuristic:false});f.result.type=8;
  f.s.activate(1,f.row.id,{});
  assert.ok(f.calls.some(c=>c[0]==='super.activate'));
  assert.equal(f.s.constructor.counters.requests.at(-1),'dynamic-pick-in-mode');
  const plain=fixture({heuristic:false});plain.result.type=8;plain.input.searchMode=null;
  plain.s.activate(1,plain.row.id,{});
  assert.equal(plain.calls.some(c=>c[0]==='super.activate'),false,'outside a mode the editor hosts it');
});

test('exposures reach the editor that records the engagement, once per result',()=>{
  const f=fixture();f.result.exposureTelemetry=true;f.s.active.exposed=new Set();f.s.sequence=1;
  const request={session:'s',revision:1,sequence:1,ids:[f.row.id]};
  assert.equal(f.s.reportNativeVisibility(request),'visible');
  assert.equal(f.s.reportNativeVisibility(request),'visible');
  const exposures=f.calls.filter(c=>c[0]==='editor-exposure');
  assert.equal(exposures.length,1);
  assert.deepEqual(exposures[0].slice(1,3),[3,5]);
  assert.equal(f.calls.some(c=>c[0]==='retained-exposure'),false);
  f.s.exposureRecorder().addExposure({id:12},{id:5});
  assert.equal(f.calls.filter(c=>c[0]==='editor-exposure').length,2,'hidden exposures use the same recorder');
  const stale=fixture();stale.editor.exposureThrows=true;
  stale.s.exposureRecorder().addExposure({id:12},{id:5});
  assert.equal(stale.s.constructor.counters.visibilityErrors.at(-1),'stale');
  const inherited=fixture();inherited.s.active.source=undefined;
  inherited.s.exposureRecorder().addExposure({id:12},{id:5});
  assert.ok(inherited.calls.some(c=>c[0]==='retained-exposure'));
});

test('a provider-requested close of the current query closes the view as an element pick',()=>{
  const f=fixture();
  f.s.onEditorPresentationRequest({kind:'close',queryId:99,elementPicked:true});
  f.s.onEditorPresentationRequest({kind:'close',queryId:5,elementPicked:false});
  assert.equal(f.calls.some(c=>c[0]==='view.close'),false);
  f.s.onEditorPresentationRequest({kind:'close',queryId:5,elementPicked:true});
  assert.equal(JSON.stringify(f.calls.find(c=>c[0]==='view.close')[1]),JSON.stringify({elementPicked:true}));
  assert.equal(f.calls.some(c=>c[0]==='address-fallback'),false);
});

test('a mode row commits its mode, mirrors it and restarts the query in that mode',async()=>{
  const f=fixture();f.result.kind='mode';f.s.active.rows[0].modeDocument=2;f.s.editId=6;f.s.modeApplied=0;
  assert.equal(f.s.activate(1,f.row.id,{}),'activated');
  assert.equal(f.input.inputField.value,'@');
  const begin=f.calls.find(c=>c[0]==='mode-begin');
  assert.equal(begin[2].startQuery,true);assert.equal(begin[2].checkValue,false);assert.ok(begin[2].activation.event);
  assert.equal(JSON.stringify(f.calls.find(c=>c[0]==='mode-commit')[1]),JSON.stringify({persistedSearchTerms:false}));
  assert.equal(f.s.modeQueryRestart.text,'@');assert.equal(f.s.modeQueryRestart.editId,6);
  assert.equal(f.s.modeApplied,1);assert.deepEqual([...f.s.nativeVisible],[]);
  await new Promise(r=>setTimeout(r,0));
  const requery=f.calls.find(c=>c[0]==='mode-requery')[1];
  assert.equal(requery.searchMode.engineName,'E');
  f.s.onEditorUpdate({kind:'first-result',query:{id:88}});
  assert.ok(f.calls.some(c=>c[0]==='started'&&c[1]===88),'the mode query rebinds the edit');
});

test('mode state committed by the editor is mirrored into the original input for the selected browser only',()=>{
  const f=fixture();
  f.s.onEditorModeState({kind:'mode-state-changed',browserId:7,searchMode:{engineName:'E'}});
  assert.equal(f.calls.find(c=>c[0]==='mirror')[1].engineName,'E');
  const other=fixture();
  other.s.onEditorModeState({kind:'mode-state-changed',browserId:99,searchMode:{engineName:'E'}});
  other.s.onEditorModeState({kind:'something-else',browserId:7});
  assert.equal(other.calls.some(c=>c[0]==='mirror'),false);
});

test('mode rejections fall back, restore the typed text and keep the stale-row guards',()=>{
  const last=f=>f.s.constructor.counters.fallbacks.at(-1);
  const none=fixture({heuristic:false});none.result.kind='mode';none.s.active.rows[0].modeDocument=2;none.editor.modeProposal=null;
  assert.equal(none.s.activate(1,none.row.id,{}),'firefox-fallback');assert.equal(last(none),'unsupported-mode');
  const rejected=fixture({heuristic:false});rejected.result.kind='mode';rejected.s.active.rows[0].modeDocument=2;rejected.editor.modeAck=false;
  assert.equal(rejected.s.activate(1,rejected.row.id,{}),'firefox-fallback');assert.equal(last(rejected),'mode-rejected');
  assert.equal(rejected.input.inputField.value,'typed');
  const refused=fixture({heuristic:false});refused.result.kind='mode';refused.s.active.rows[0].modeDocument=2;refused.editor.modeCommit=null;
  assert.equal(refused.s.activate(1,refused.row.id,{}),'firefox-fallback');assert.equal(last(refused),'mode-commit-rejected');
  assert.equal(refused.s.modeQueryRestart??null,null);
  const wrongDocument=fixture({heuristic:false});wrongDocument.result.kind='mode';wrongDocument.s.active.rows[0].modeDocument=9;
  assert.equal(wrongDocument.s.activate(1,wrongDocument.row.id,{}),'stale');
});

test('an edit records the search string on the original input for its inherited pick paths',async()=>{
  const f=fixture();f.input.value='abc';f.editor.edit=async()=>null;
  f.input._lastSearchString='';
  await f.s.query('abc',null,{composing:false,selectionStart:3,selectionEnd:3,inputType:'insertText'});
  assert.equal(f.input._lastSearchString,'abc');
  assert.equal(f.input._valueOnLastSearch,'abc');
});

test('edits carry the input search mode so a confirmed mode survives typing',async()=>{
  const f=fixture();let state=null;
  f.editor.edit=async s=>{state=s;return null;};
  await f.s.query('abc',null,{composing:false,selectionStart:3,selectionEnd:3,inputType:'insertText'});
  assert.equal(state.searchMode.engineName,'Held');
});

test('an unsupported Enter on the heuristic or with no row is completed once by Firefox',()=>{
  for(const id of ['','row']){
    const f=fixture(),counters=f.s.constructor.counters,before=counters.replays;
    f.editor.beginNavigationActivation=()=>null;
    assert.equal(f.s.activate(1,id==='row'?f.row.id:'',{altKey:true}),'activated');
    assert.equal(counters.replays,before+1);
    assert.equal(counters.fallbacks.at(-1),'unsupported-plan','the reason is still recorded');
    const commands=f.calls.filter(c=>c[0]==='handleCommand');
    assert.equal(commands.length,1);
    assert.equal(commands[0][1].key,'Enter');assert.equal(commands[0][1].altKey,true);assert.equal('keyCode' in commands[0][1],false);
    assert.equal(f.input._lastSearchString,'typed','telemetry sees the typed text, not an older search');
    assert.deepEqual(f.engagements.at(-1),['discard']);
    assert.equal(f.s.active,null);assert.equal(f.s.revision,2);
  }
});

test('a non-heuristic row, a selected row and a consumed edit are never replayed',()=>{
  const row=fixture();row.editor.beginNavigationActivation=()=>null;row.s.active.context.heuristicResult=null;
  assert.equal(row.s.activate(1,row.row.id,{altKey:true}),'firefox-fallback');
  assert.equal(row.calls.some(c=>c[0]==='handleCommand'),false);
  const selected=fixture();selected.editor.beginNavigationActivation=()=>null;selected.s.nativeSelected=selected.result;
  assert.equal(selected.s.activate(1,'',{}),'firefox-fallback');
  assert.equal(selected.calls.some(c=>c[0]==='handleCommand'),false);
  const consumed=fixture();consumed.editor.commitAcknowledgedNavigation=()=>({attempted:false,reverted:false});
  assert.equal(consumed.s.activate(1,'',{}),'firefox-fallback');
  assert.equal(consumed.calls.some(c=>c[0]==='handleCommand'),false,'a load that was attempted must not run twice');
  assert.equal(consumed.s.active,null);
});

test('a result Firefox cannot activate is replayed for Enter without refocusing, and refocuses for a named non-heuristic row',()=>{
  const enter=fixture();enter.result.activatable=false;enter.input.focus=()=>enter.calls.push(['focus']);
  assert.equal(enter.s.activate(1,'',{}),'activated');
  assert.equal(enter.calls.some(c=>c[0]==='focus'),false);
  assert.equal(enter.s.constructor.counters.fallbacks.at(-1),'not-activatable');
});

test('the activation event carries no keyCode so Ctrl is not a canonization request',()=>{
  const f=fixture();let seen=null;
  f.editor.beginNavigationActivation=(r,q,res,event)=>{seen=event;return {token:'sel'};};
  f.s.activate(1,f.row.id,{ctrlKey:true});
  assert.equal(seen.key,'Enter');assert.equal(seen.ctrlKey,true);assert.equal('keyCode' in seen,false);
});

test('the external handoff reports the owned persisted-search-terms state, not a constant',()=>{
  const f=externalFixture();
  globalThis.r2Persisted=true;
  f.s.activate(1,f.row.id,{ctrlKey:true});
  const seen=f.calls.find(c=>c[0]==='external-revert')[2];
  globalThis.r2Persisted=false;
  assert.equal(seen.persistedSearchTerms,true);
});

test('a heuristic row the host has not yet reported visible is completed by Firefox, a suggestion row is not',()=>{
  const heuristic=fixture({visible:false});
  assert.equal(heuristic.s.activate(1,heuristic.row.id,{}),'activated');
  assert.equal(heuristic.s.constructor.counters.fallbacks.at(-1),'not-visible');
  assert.equal(heuristic.calls.filter(c=>c[0]==='handleCommand').length,1);
  const suggestion=fixture({visible:false,heuristic:false});
  assert.equal(suggestion.s.activate(1,suggestion.row.id,{}),'firefox-fallback');
});

test('an activation before the first streamed update, and a restart that throws, hand the surface back',async()=>{
  const early=fixture({heuristic:false});early.s.active.context=null;early.s.active.results=[early.result];early.s.active.resultIndexes=[0];
  early.s.invokeResultMenu(early.result,{command:'dismiss'},{});
  assert.equal(early.s.constructor.counters.fallbacks.at(-1),'no-context');
  assert.ok(early.calls.some(c=>c[0]==='address-fallback'));
  const f=fixture();f.s.tipQueryRestart=null;
  f.editor.acknowledgeRestartQuery=async()=>{throw Error('mode state mismatch');};
  f.s.onEditorPresentationRequest({kind:'restart-query',token:{proposal:{value:'x',edit:{text:'typed',selectionDirection:'none'}}}});
  await new Promise(r=>setTimeout(r,5));
  assert.equal(f.s.tipQueryRestart,null,'the admission does not outlive a failed restart');
  assert.ok(f.calls.some(c=>c[0]==='address-fallback'));
});

test('the fallback readout counts every fallback by reason and reports a rate per 200 activations',()=>{
  const f=fixture({visible:false,heuristic:false});
  const C=f.s.constructor,before=C.report();
  f.s.activate(1,f.row.id,{});
  C.counters.activations+=3;
  const after=C.report();
  assert.equal(after.fallbackTotal,before.fallbackTotal+1);
  assert.equal(after.fallbackReasons['not-visible'],(before.fallbackReasons['not-visible']??0)+1);
  assert.equal(after.fallbacksPer200Activations,Math.round(1/3*200*100)/100);
  assert.equal(after.errorFallbacks,before.errorFallbacks);
  assert.equal(JSON.stringify(Object.keys(after)),JSON.stringify(Object.keys(before)));
});

test('diagnostic lists keep only the most recent entries',()=>{
  const f=fixture();
  for(let i=0;i<450;i++)f.s.onEditorPresentationRequest({kind:'close'});
  assert.equal(f.s.constructor.counters.requests.length,200);
});
