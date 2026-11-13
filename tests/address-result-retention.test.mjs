import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const context = vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs', import.meta.url), 'utf8')
  .replace(/^import\b[^;]*;$/m, '').replace('export class WinUIAddressSession', 'this.Session = class WinUIAddressSession'), context);

function fixture() {
  const s = Object.create(context.Session.prototype);
  const browser = {currentURI:{spec:'https://page.test/'}, browsingContext:{currentWindowGlobal:{innerWindowId:7}}};
  const controller = {resultsOpen:true, visibleResults:[], cancelQuery(){}};
  const input = {view:{nativePresentation:s}, controller, inputField:{value:'old'}, searchMode:null};
  s.input = input;
  s.window = {gBrowser:{selectedBrowser:browser}};
  s._revision = 1; s._sequence = 1; s.sessionId = 's'; s.nativeVisible = [];
  s.active = {browser, page:browser.currentURI.spec, revision:1, text:'old', inputValue:'old', done:true,
    presentationDocument:7, presentationMode:'null', results:[], resultIndexes:[],
    rows:[0, 1, 2].map(i => ({id:`old-${i}`, title:`Old ${i}`, actions:['activate'],
      actionLabel:'Visit', tipButtons:[{id:'tip'}], helpLink:{id:'help'}}))};
  return {s, browser, input};
}

test('pending and streamed replacements retain the first row and full tail until successful finish', () => {
  const {s} = fixture();
  const previous = s.active.rows;
  const retained = s.retainNativeRows();
  assert.equal(retained.length, 3);
  assert.ok(retained.every(row => row.stale && !row.actions.length && row.tipButtons.length === 1 && row.helpLink));
  assert.equal(previous[0].actions.length, 1, 'retention cannot mutate old query objects');
  s.active = {...s.active, revision:2, text:'new', done:false, rows:[], retainedRows:retained, context:{id:2}};
  assert.deepEqual(Array.from(s.snapshot().rows, row => row.title), ['Old 0', 'Old 1', 'Old 2']);
  s.active.rows = [{id:'new-0', title:'New first', actions:['activate']}];
  assert.deepEqual(Array.from(s.snapshot().rows, row => row.title), ['New first', 'Old 1', 'Old 2']);
  assert.equal(s.active.rows.length, 1, 'stale tail never gains current-query authority');
  const serial = s.sequence;
  s.onQueryFinished({id:1});
  assert.equal(s.snapshot().rows.length, 3, 'a superseded finish cannot trim the current presentation');
  s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.snapshot().rows, row => row.title), ['New first']);
  assert.ok(s.sequence > serial, 'finishing publishes the final trim');
});

test('retained rich rows preserve presentation but remove command metadata across repeated edits', () => {
  const {s}=fixture();
  const row=s.active.rows[0];
  row.tipButtons=[{id:'button',label:'Action',enabled:true,menu:true,menuLabel:'Options',action:{command:'navigate'},signature:'secret',document:7}];
  row.helpLink={id:'help',before:'Read ',label:'Help',after:'.',document:7,signature:'secret',enabled:true};
  row.description='Read Help.';
  const retained=s.retainNativeRows();
  assert.deepEqual(JSON.parse(JSON.stringify(retained[0].tipButtons)),[{id:'button',label:'Action',enabled:true,menu:true,menuLabel:'Options'}]);
  assert.deepEqual(JSON.parse(JSON.stringify(retained[0].helpLink)),{id:'help',before:'Read ',label:'Help',after:'.'});
  assert.equal(retained[0].description,row.description);
  assert.notEqual(retained[0].tipButtons,row.tipButtons);
  s.active={...s.active,done:false,rows:[],retainedRows:retained};
  assert.deepEqual(JSON.parse(JSON.stringify(s.retainNativeRows())),JSON.parse(JSON.stringify(retained)));
});

test('coalesced rapid queries carry Firefox selection appearance independently of displayed IDs', () => {
  const {s, input} = fixture();
  const selected = {};
  s.active.results = [selected]; s.active.resultIndexes = [0];
  input.view.selectedResult = selected;
  const first = s.retainNativeRows();
  assert.equal(first[0].retainedSelected, true);
  assert.equal(first[1].retainedSelected, false);
  s.active = {...s.active, done:false, rows:[], retainedRows:first};
  input.view.selectedResult = null;
  assert.equal(s.retainNativeRows()[0].retainedSelected, true, 'another edit preserves historical selection');
  s.active.rows = [{id:'unseen-query', actions:[]}];
  input.view.selectedResult = selected;
  const unseen = s.retainNativeRows();
  assert.equal(unseen[0].id, 'unseen-query');
  assert.equal(unseen[0].retainedSelected, true, 'selection survives a skipped native publication');
  assert.deepEqual(Array.from(unseen[0].actions), []);
  input.view.selectedResult = null;
  assert.equal(s.retainNativeRows()[0].retainedSelected, false, 'Firefox empty selection remains authoritative');
});

test('rapid edits retain the complete displayed set, while empty completion clears it', () => {
  const {s} = fixture();
  s.active.retainedRows = s.retainNativeRows(); s.active.rows = []; s.active.done = false;
  const retained = s.retainNativeRows();
  assert.equal(retained.length, 3);
  s.active.retainedRows = retained; s.active.context = {id:3};
  s.onQueryFinished({id:3});
  assert.equal(s.snapshot().rows.length, 0);
});

test('mixed old and new images stay within the native snapshot budget without shrinking rows', () => {
  const {s} = fixture();
  s.active.rows = Array.from({length:7}, (_, i) => ({id:`old-${i}`, image:{width:i ? 104 : 1, height:i ? 104 : 1}}));
  s.active.retainedRows = s.retainNativeRows();
  s.active.rows = [{id:'new', image:{width:104, height:104}}]; s.active.done = false;
  const rows = s.nativePresentationRows();
  assert.equal(rows.length, 7);
  assert.ok(rows.reduce((sum, row) => sum + (row.image?.width || 0) * (row.image?.height || 0), 0) <= 65536);
  assert.equal(rows[0], s.active.rows[0]);
  assert.ok(rows.slice(1).some(row => row.image === null));
});

test('retention is bounded and cannot cross page, document, mode, browser or dismissal', () => {
  for (const mutate of [f => f.browser.currentURI.spec = 'https://other.test/',
    f => f.browser.browsingContext.currentWindowGlobal.innerWindowId++,
    f => f.input.searchMode = {engineName:'Other'},
    f => f.s.window.gBrowser.selectedBrowser = {...f.browser},
    f => f.s.nativeOpen = false, f => f.s.closed = true]) {
    const f = fixture(); mutate(f);
    assert.equal(f.s.retainNativeRows().length, 0);
  }
  const {s} = fixture();
  s.active.rows = Array.from({length:150}, (_, i) => ({id:`row-${i}`}));
  assert.equal(s.retainNativeRows().length, 100);
  s.active.retainedRows = s.retainNativeRows(); s.active.rows = []; s.active.done = false;
  s.suspendNativePresentation();
  assert.equal(s.active.retainedRows.length, 0);
});

test('legacy query preserves display rows before cancellation, without retaining activation objects', async () => {
  const {s, input} = fixture();
  let finish;
  input.lastQueryContextPromise = new Promise(resolve => finish = resolve);
  input.startQuery = () => {};
  input.setNativeInputValue = text => input.inputField.value = text;
  s.cancel = () => {s.active = null; s.revision++;};
  const pending = s.legacyQuery('new', {});
  assert.equal(s.active.rows.length, 0);
  assert.equal(s.snapshot().rows.length, 3);
  assert.equal(s.snapshot().selectedId, null);
  const request = {session:'s', revision:s.revision, sequence:s.sequence, ids:['old-0']};
  assert.equal(s.legacyReportNativeVisibility(request), 'stale');
  finish(); await pending;
  assert.equal(s.snapshot().rows.length, 0, 'suppressed queries cannot retain rows indefinitely');
});

test('an early non-heuristic cannot displace the retained first selection or lift its group heading',()=>{
  const {s}=fixture();s.active.rows[0].heuristic=true;
  s.active.retainedRows=s.retainNativeRows();s.active.done=false;s.active.context={id:2};
  s.active.rows=[{id:'history',title:'History',sectionLabel:'Suggest',actions:['activate']}];
  let rows=s.nativePresentationRows();
  assert.deepEqual(Array.from(rows,r=>r.id),['old-0','history','old-2']);
  assert.equal(rows[0].stale,true);assert.deepEqual(Array.from(rows[0].actions),[]);
  assert.equal(rows[1].sectionLabel,'Suggest');assert.equal(s.active.rows.length,1);
  s.active.rows.unshift({id:'heuristic',heuristic:true,title:'New first',actions:['activate']});
  rows=s.nativePresentationRows();
  assert.deepEqual(Array.from(rows,r=>r.id),['heuristic','history','old-2']);
  s.active.rows.shift();s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['history'],'completed heuristic-free queries use their real layout');
});

test('URL heuristics and early history preserve search-suggestion slots until replacement or completion',()=>{
  const {s}=fixture();
  s.active.rows=[{id:'old-heuristic',heuristic:true,actions:[]},
    {id:'search-1',searchSuggestion:true,actions:[]},
    {id:'search-2',searchSuggestion:true,actions:[]},
    {id:'old-history',sectionLabel:'Suggest',actions:[]}];
  s.active.retainedRows=s.retainNativeRows();s.active.done=false;s.active.context={id:2};
  const history={id:'history',sectionLabel:'Suggest',actions:['activate']};
  s.active.rows=[history];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['old-heuristic','search-1','search-2','history']);
  const heuristic={id:'url-heuristic',heuristic:true,actions:['activate']};
  s.active.rows=[heuristic,history];
  let rows=s.nativePresentationRows();
  assert.deepEqual(Array.from(rows,r=>r.id),['url-heuristic','search-1','search-2','history']);
  assert.ok(rows.slice(1,3).every(r=>r.stale && !r.actions.length));
  // A further keystroke/backspace retains the displayed mixed generation.
  s.active.retainedRows=s.retainNativeRows();s.active.rows=[];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['url-heuristic','search-1','search-2','history']);
  s.active.rows=[heuristic,{id:'new-search',searchSuggestion:true,actions:[]},history];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['url-heuristic','new-search','search-2','history']);
  s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['url-heuristic','new-search','history']);
});

function assertStale(s, rows) {
  for (const row of rows.filter(row => row.stale)) {
    assert.deepEqual(Array.from(row.actions), []);
    assert.equal(s.legacyActivate(s.active.revision, row.id), 'stale');
    assert.equal(s.legacyReportNativeVisibility({session:s.sessionId, revision:s.active.revision,
      sequence:s.sequence, ids:[row.id]}), 'stale');
  }
}

test('fast provider cannot overwrite slower provider slots through repeated edits and completion', () => {
  const {s}=fixture();
  s.active.rows=[{id:'old-h',heuristic:true,actions:[]},
    {id:'old-b',provider:'B',actions:['activate'],description:'Rich result'},
    {id:'old-a',provider:'A',actions:['activate']}];
  s.active.retainedRows=s.retainNativeRows();
  s.active.done=false;s.active.context={id:2};
  const h={id:'h',heuristic:true,actions:[]}, a={id:'a',provider:'A',actions:[]};
  s.active.rows=[h,a];
  let rows=s.nativePresentationRows();
  assert.deepEqual(Array.from(rows,r=>r.id),['h','old-b','a']);
  assertStale(s,rows);
  s.active.retainedRows=s.retainNativeRows();
  s.active.rows=[];
  assertStale(s,s.nativePresentationRows());
  s.active.rows=[h,{id:'b',provider:'B',actions:[]},a];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','b','a']);
  s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','b','a']);
  assert.equal(s.legacyActivate(s.active.revision,'old-b'),'stale');
});

test('extra search suggestion transfers the localized heading to retained history without gaining authority', () => {
  const {s}=fixture();
  const group={_groupLabelKey:'history-label',_groupLabel:'Localized Suggest',_hideRowLabel:false};
  s.active.rows=[{id:'old-h',heuristic:true,actions:[]},
    {id:'old-search',searchSuggestion:true,actions:[]},
    {id:'history-1',sectionLabel:'Localized Suggest',...group,actions:['activate']},
    {id:'history-2',sectionLabel:'',...group,actions:['activate']}];
  s.active.retainedRows=s.retainNativeRows();s.active.done=false;s.active.context={id:2};
  const h={id:'h',heuristic:true,actions:[]};
  const searches=[1,2].map(i=>({id:`search-${i}`,searchSuggestion:true,actions:[]}));
  s.active.rows=[h,...searches];
  let rows=s.nativePresentationRows();
  assert.deepEqual(Array.from(rows,r=>r.id),['h','search-1','search-2','history-2']);
  assert.equal(rows[3].sectionLabel,'Localized Suggest');
  assert.equal(s.active.retainedRows[3].sectionLabel,'','presentation must not mutate retained source rows');
  assertStale(s,rows);
  // Another keystroke/backspace keeps the transferred label and stale identity.
  s.active.retainedRows=s.retainNativeRows();s.active.rows=[];
  assert.equal(s.nativePresentationRows()[3].sectionLabel,'Localized Suggest');
  assertStale(s,s.nativePresentationRows());
  s.active.rows=[h,...searches,{id:'history-new',sectionLabel:'Localized Suggest',...group,actions:[]}];
  assert.equal(s.nativePresentationRows().filter(r=>r.sectionLabel).length,1);
  s.onQueryFinished({id:2});
  assert.equal(s.nativePresentationRows().filter(r=>r.sectionLabel).length,1);
  assert.equal(s.legacyActivate(s.active.revision,'history-2'),'stale');
});

test('incompatible suggested-index placement waits for completion instead of jumping slots', () => {
  const {s}=fixture();
  s.active.rows=[{id:'old-h',heuristic:true,actions:[]},
    {id:'indexed',provider:'B',hasSuggestedIndex:true,suggestedIndex:1,actions:[]}];
  s.active.retainedRows=s.retainNativeRows();s.active.done=false;s.active.context={id:2};
  s.active.rows=[{id:'h',heuristic:true,actions:[]},{id:'a',provider:'A',actions:[]}];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','indexed']);
  assertStale(s,s.nativePresentationRows());
  s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','a']);
});

test('retained provider slots respect Firefox rich-row span limits and cannot advertise hidden arrow targets', () => {
  const {s}=fixture();
  s.active.rows=[{id:'old-h',heuristic:true,actions:[]},
    {id:'rich',provider:'B',_span:2,actions:[]}];
  s.active.retainedRows=s.retainNativeRows();s.active.done=false;
  s.active.context={id:2,maxResults:3};
  s.active.rows=[{id:'h',heuristic:true,actions:[]},{id:'a',provider:'A',actions:[]}];
  s.active.results=[{},{}];s.active.resultIndexes=[0,1];
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','rich']);
  s.resultPresentation={allowEmptySelection:()=>false,
    arrowSelection:({rowCount})=>({kind:'row',index:rowCount-1})};
  s.canActivate=()=>true;
  assert.deepEqual(Array.from(s.nativeArrowTargets(s.active.rows).rows,r=>r.id),['h']);
  assertStale(s,s.nativePresentationRows());
  s.onQueryFinished({id:2});
  assert.deepEqual(Array.from(s.nativePresentationRows(),r=>r.id),['h','a']);
  assert.deepEqual(Array.from(s.nativeArrowTargets(s.active.rows).rows,r=>r.id),['h','a']);
});
