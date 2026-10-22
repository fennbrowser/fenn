import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const context = vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs', import.meta.url), 'utf8')
  .replace(/^import .*;$/m, '').replace('export class WinUIAddressSession', 'this.Session = class WinUIAddressSession'), context);

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
  assert.ok(retained.every(row => row.stale && !row.actions.length && !row.tipButtons.length && !row.helpLink));
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
