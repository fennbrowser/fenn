import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const sandbox = vm.createContext({console, UrlbarShared:{RESULT_TYPE:{SEARCH:2,TIP:7,RESTRICT:9},
  ICON:{HISTORY:'history'},getSpanForResult:r=>r.span || 1,deepEqual:(a,b)=>JSON.stringify(a)===JSON.stringify(b)}});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs',import.meta.url),'utf8')
  .replace(/^import\b[^;]*;$/m,'').replace('export class WinUIAddressSession','this.Session = class WinUIAddressSession'),sandbox);
const flush = async () => { for(let i=0;i<8;i++) await Promise.resolve(); };

test('publication preserves localized group metadata on every row, including suppressed headings', async () => {
  const {s,query,result,publish}=fixture();
  s.nativeResultPresentation=sandbox.Session.prototype.nativeResultPresentation;
  const history={type:1,source:1,providerName:'History',payload:{url:'https://example.test/'}};
  const second={...history,payload:{url:'https://example.test/second'}};
  query.results=[result,history,second];
  s.resultPresentation.candidates=()=>query.results;
  s.resultPresentation.icon=()=> 'history';
  s.resultActions.rowLabel=r=>r.heuristic?null:{id:'localized-history'};
  s.window.document.l10n.formatMessages=async()=>[{attributes:[{name:'label',value:'Localized Suggest'}]}];
  publish();await flush();
  const rows=s.active.rows;
  assert.equal(rows[1].sectionLabel,'Localized Suggest');
  assert.equal(rows[2].sectionLabel,'');
  assert.equal(rows[2]._groupLabel,'Localized Suggest');
  assert.equal(rows[2]._groupLabelKey,rows[1]._groupLabelKey);
  // Replacing the heading owner cannot require another localization round trip.
  s.active.retainedRows=rows.map(r=>({...r,stale:true,actions:[]}));
  s.active.rows=[rows[0],{id:'search',provider:'History',searchSuggestion:true,actions:[]}];
  // Same-provider search replacement is permitted after a prior suggestion.
  s.active.retainedRows[1].searchSuggestion=true;
  const displayed=s.nativePresentationRows();
  assert.equal(displayed.at(-1).sectionLabel,'Localized Suggest');
  assert.equal(rows[2].sectionLabel,'');
});
function fixture(tip=false) {
  const s=Object.create(sandbox.Session.prototype), images=[], labels=[];
  const browser={currentURI:{spec:'page'},browsingContext:{currentWindowGlobal:{innerWindowId:7}}};
  const result={type:tip?7:2,source:1,providerName:'fixture',heuristic:true,
    payload:tip?{text:'Tip',buttons:[{}],descriptionLearnMoreTopic:'help'}:{engine:'Engine'}};
  const query={id:1,results:[result],heuristicResult:result};
  class Image {
    constructor(){images.push(this);this.naturalWidth=1;this.naturalHeight=1;}
    decode(){return new Promise(resolve=>this.finish=resolve);}
  }
  Object.assign(s,{sessionId:'s',_sequence:1,changed(){},imageLoads:new Set(),
    input:{view:{nativePresentation:s},controller:{resultsOpen:true},inputField:{value:'query'}},
    window:{gBrowser:{selectedBrowser:browser},Image,document:{
      l10n:{formatValue:()=>new Promise(resolve=>labels.push(resolve))},
      createElementNS:()=>({getContext:()=>({drawImage(){},getImageData:()=>({data:[1,2,3,255]})})})}},
    active:{context:query,browser,page:'page',revision:1,text:'query',inputValue:'query',rows:[],results:[],resultIndexes:[]},
    captureAutofill(){},exposureRecorder:()=>({discardTentativeExposures(){}}),
    resultActions:{invalidate(){},buttons:r=>{r.payload.buttons.forEach((b,i)=>b.name??=String(i));return r.payload.buttons;},
      describeButtons:async()=>[{name:'0',label:'Act',enabled:true,action:{name:'0'}}],
      describeDescription:async()=>({before:'Read ',label:'Help',after:'.'})},
    resultPresentation:{candidates:()=>[result],displayText:()=>({title:tip?'Tip':'query',displayUrl:''}),allowEmptySelection:()=>false},
    nativeResultPresentation:()=>[{size:16,symbol:'search',icon:'test-icon'}],
    nativeKeywordResult:()=>false,nativeOmniboxResult:()=>false,canActivate:()=>true,
    nativeActionPresentation:()=>null,nativeCopyProvider:()=>false,nativeModeResult:()=>false,
    loadNativeDetails:async()=>{},loadNativeCopyResult:async()=>{},loadNativeModeResult:async()=>{}});
  s.nativeOpen=true;
  return {s,query,result,images,labels,browser,publish:()=>s.onQueryResults(query)};
}

test('current and explicit empty selection retire historical selection across rapid edits',()=>{
  for (const empty of [false,true]) {
    const f=fixture();
    f.s.active.retainedRows=[0,1,2].map(i=>({id:`old-${i}`,stale:true,retainedSelected:i===2,actions:[]}));
    if(empty) {
      f.s.resultPresentation.allowEmptySelection=()=>true;
    }
    f.publish();
    assert.ok(f.s.active.retainedRows.every(row=>!row.retainedSelected));
    assert.equal(f.s.nativeSelected,empty?null:f.result);
    f.s.input.searchMode=null;
    f.s.input.view.selectedResult=f.s.nativeSelected;
    const next=f.s.retainNativeRows();
    assert.equal(next.filter(row=>row.retainedSelected).length,empty?0:1);
    assert.ok(next.every(row=>row.stale && !row.actions.length));
    f.s.active={...f.s.active,rows:[],retainedRows:next,done:false};
    f.s.input.view.selectedResult=null;
    assert.equal(f.s.retainNativeRows().filter(row=>row.retainedSelected).length,empty?0:1,
      'successive typing/backspacing cannot resurrect the old tail selection');
  }
});

test('early non-heuristic results preserve pending historical selection until Firefox decides',()=>{
  const f=fixture();
  f.query.heuristicResult=null;
  f.s.resultPresentation.allowEmptySelection=()=>true;
  f.s.active.retainedRows=[{id:'old',stale:true,retainedSelected:true,actions:[]}];
  f.publish();
  assert.equal(f.s.active.retainedRows[0].retainedSelected,true);
  f.query.heuristicResult=f.result;
  f.publish();
  assert.equal(f.s.active.retainedRows[0].retainedSelected,false);
});

test('unchanged result retains decoded presentation and pending work but rotates authority',async()=>{
  const f=fixture();f.publish();
  const row=f.s.active.rows[0],id=row.id,presentationId=row.presentationId;
  f.publish();
  assert.equal(f.s.active.rows[0],row);
  assert.notEqual(row.id,id);assert.equal(row.presentationId,presentationId);
  assert.equal(f.images.length,1);assert.equal(f.labels.length,1);
  assert.equal(f.images[0].src,'test-icon','unchanged pending decode was not cancelled');
  f.images[0].finish();f.labels[0]('Search with Engine');await flush();
  const image=row.image;
  assert.ok(image);assert.equal(row.actionLabel,'Search with Engine');
  f.publish();
  assert.equal(row.image,image);assert.equal(row.actionLabel,'Search with Engine');
  assert.equal(f.images.length,1);assert.equal(f.s.active.imagePixels,image.width*image.height);
  assert.deepEqual(Array.from(f.s.nativeVisible),[],'new IDs need a new exposure report');
});

test('payload and document changes replace presentation and refuse old completions',async()=>{
  for(const change of [f=>{f.result.payload.engine='Other';},f=>{f.browser.browsingContext.currentWindowGlobal.innerWindowId++;}]) {
    const f=fixture();f.publish();const old=f.s.active.rows[0];change(f);f.publish();
    const row=f.s.active.rows[0];assert.notEqual(row,old);assert.notEqual(row.presentationId,old.presentationId);
    assert.equal(f.images[0].src,'');
    f.images[0].finish();f.labels[0]('Old label');await flush();
    assert.equal(row.image,undefined);assert.equal(row.actionLabel,undefined);
    f.images[1].finish();f.labels[1]('Current label');await flush();
    assert.ok(row.image);assert.equal(row.actionLabel,'Current label');
  }
});

test('reused tip buttons and help get new IDs and reject old or unexposed requests',async()=>{
  const f=fixture(true);f.publish();await flush();
  const row=f.s.active.rows[0],button=row.tipButtons[0],help=row.helpLink;
  const request=actionId=>({version:1,session:'s',revision:1,id:row.id,actionId});
  const oldButton=request(button.id),oldHelp=request(help.id);
  f.s.nativeVisible=[row.id];assert.ok(f.s.resolveTipButton(oldButton));assert.ok(f.s.resolveTipButton(oldHelp));
  f.publish();await flush();
  assert.equal(f.s.active.rows[0],row);assert.equal(row.tipButtons[0],button);assert.equal(row.helpLink,help);
  assert.equal(f.s.resolveTipButton(oldButton),null);assert.equal(f.s.resolveTipButton(oldHelp),null);
  assert.equal(f.s.resolveTipButton(request(button.id)),null);
  f.s.nativeVisible=[row.id];assert.ok(f.s.resolveTipButton(request(button.id)));assert.ok(f.s.resolveTipButton(request(help.id)));
  f.result.payload.buttons[0].disabled=true;
  assert.equal(f.s.resolveTipButton(request(button.id)),null,'in-place provider mutation revokes action signature');
});

test('provider enabled-state changes invalidate reuse even without a payload change',()=>{
  const f=fixture();let enabled=true;
  f.s.nativeActionPresentation=()=>({enabled,label:'Provider action',icon:'action-icon'});
  f.publish();const old=f.s.active.rows[0];assert.equal(old.enabled,true);
  enabled=false;f.publish();
  assert.notEqual(f.s.active.rows[0],old);
  assert.equal(f.s.active.rows[0].enabled,false);
});

vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarResultPresentation.mjs',import.meta.url),'utf8')
  .replace(/^import\b[^;]*;$/gm,'').replace('export class UrlbarResultPresentation','this.Presentation = class UrlbarResultPresentation'),sandbox);
function navigationFixture() {
  const f=fixture();
  const second={...f.result,payload:{engine:'Second'}};
  f.query.results.push(second);
  f.s.resultPresentation.candidates=()=>f.query.results;
  f.s.resultPresentation.arrowSelection=sandbox.Presentation.prototype.arrowSelection;
  f.s.nativeResultPresentation=()=>f.query.results.map(()=>({size:16,symbol:'search'}));
  f.publish();f.s.nativeVisible=f.s.active.rows.map(r=>r.id);
  f.s.input.controller.parentController={onBeforeSelection(){},onSelection(){}};
  f.s.input.setValueFromResult=({result})=>{f.s.input.inputField.value=result?.payload.engine||'query';};
  f.cancellations=0;
  f.s.input.controller.cancelQuery=()=>{f.cancellations++;f.s.onQueryCancelled(f.query);};
  f.request=()=>({version:1,session:'s',revision:1,id:f.s.active.rows[1].id,traverse:1});
  return f;
}
test('native arrows freeze the live query, retain selection authority, and reject late publications',()=>{
  const f=navigationFixture(), oldRows=f.s.active.rows;
  assert.equal(f.s.selectResult(f.request()).status,'selected');
  assert.equal(f.cancellations,1);assert.equal(f.s.active.done,true);
  assert.equal(f.s.nativeSelected,f.query.results[1]);assert.equal(f.s.active.inputValue,'Second');
  f.query.results=[{...f.result,payload:{engine:'Late'}}];f.publish();
  assert.equal(f.s.active.rows,oldRows,'late provider publication cannot replace the frozen display');
  assert.equal(f.s.selectResult({...f.request(),id:'retired'}).status,'stale');
  assert.equal(f.cancellations,1);
});
test('native arrow policy preserves reverse traversal, empty selection, and deferred-event exception',()=>{
  const f=navigationFixture();f.s.input.isDeferringEvents=true;
  f.s.active.retainedRows=[{id:'retired',stale:true,retainedSelected:true,actions:[]}];
  f.s.resultPresentation.allowEmptySelection=()=>true;
  assert.equal(f.s.selectResult({...f.request(),id:'',traverse:-1}).status,'selected');
  assert.equal(f.s.nativeSelected,null);assert.equal(f.s.active.inputValue,'query');
  assert.equal(f.s.active.retainedRows[0].retainedSelected,false,'explicit empty traversal retires old appearance without freezing');
  assert.equal(f.cancellations,0);assert.equal(f.s.active.frozen,undefined);
  f.s.active.retainedRows[0].retainedSelected=true;
  assert.equal(f.s.selectResult({...f.request(),traverse:-1}).status,'selected');
  assert.equal(f.s.nativeSelected,f.query.results[1]);
  assert.equal(f.s.active.retainedRows[0].retainedSelected,false,'current traversal retires old appearance without freezing');
  f.s.input.isDeferringEvents=false;
  assert.equal(f.s.selectResult({...f.request(),id:'retired'}).status,'stale');
  assert.equal(f.cancellations,0,'stale gestures cannot freeze a newer query');
});

test('published arrow destinations use Firefox wrapping and empty-selection policy',()=>{
  const f=navigationFixture(), rows=f.s.active.rows;
  let targets=f.s.nativeArrowTargets(rows);
  assert.equal(targets.forward,rows[0].id);assert.equal(targets.backward,rows[1].id);
  assert.equal(targets.rows[0].forward,rows[1].id);assert.equal(targets.rows[0].backward,rows[1].id);
  f.s.resultPresentation.allowEmptySelection=()=>true;
  targets=f.s.nativeArrowTargets(rows);
  assert.equal(targets.rows[0].backward,'');assert.equal(targets.rows[1].forward,'');
});

test('a cancelled provider cannot mutate a frozen result into a new activation target',()=>{
  const f=navigationFixture();f.s.selectResult(f.request());
  f.s.active.results[1].payload.engine='Mutated after cancellation';
  assert.equal(f.s.selectResult(f.request()).status,'stale');
  assert.equal(f.s.activateWithReplay(1,f.s.active.rows[1].id),'stale');
});

test('native selection freezes the independent editor that owns the running provider',()=>{
  const f=navigationFixture();f.s.active.source='independent';f.s.editorRevision=9;
  f.s.editor={freezeResults(revision,id){assert.equal(revision,9);assert.equal(id,f.query.id);f.cancellations++;}};
  f.s.input.controller.cancelQuery=()=>assert.fail('the retained input does not own this query');
  assert.equal(f.s.selectResult(f.request()).status,'selected');
  assert.equal(f.cancellations,1);assert.equal(f.s.active.done,true);
});

test('matching group headings are available immediately across query replacements',()=>{
  const f=fixture();const label={id:'group-heading'};
  f.s.nativeResultPresentation=()=>[{size:16,symbol:'search',label}];
  f.s.window.document.l10n.formatMessages=()=>new Promise(()=>{});
  f.s.active.retainedRows=[{_sectionLabelKey:JSON.stringify(label),sectionLabel:'Localized heading'}];
  f.publish();assert.equal(f.s.active.rows[0].sectionLabel,'Localized heading');
  f.result.payload.engine='New';f.publish();
  assert.equal(f.s.active.rows[0].sectionLabel,'Localized heading');
  label.id='different-group';f.publish();
  assert.equal(f.s.active.rows[0].sectionLabel,'','a different heading never inherits old text');
});
