import assert from 'node:assert/strict';
import test from 'node:test';
import {NAVIGATION_COMMANDS,NAVIGATION_STATE_READS,WinUINavigationState} from '../upstream/firefox/browser/base/content/WinUINavigationState.sys.mjs';

function node(attributes={}, children=[]) {
  const map=new Map(Object.entries(attributes));
  return {hidden:!!attributes.hidden,children,getAttribute:k=>map.get(k)??null,hasAttribute:k=>map.has(k)};
}
function fixture(elements={}, {percent=0,fallback=()=>''}={}) {
  const requested=[];
  const window={document:{getElementById:id=>{requested.push(id);return elements[id]??null;}},
    DownloadsIndicatorView:{_percentComplete:percent}};
  return {model:new WinUINavigationState(window,fallback),requested};
}

test('availability comes from the command and a missing element is unavailable',()=>{
  const {model}=fixture({'Browser:Back':node({}),'back-button':node({label:'Back'}),'Browser:Forward':node({disabled:'true'}),
    'forward-button':node({label:'Forward'}),'home-button':node({label:'Home'})});
  const state=model.snapshot();
  assert.equal(state.back.enabled,true);assert.equal(state.forward.enabled,false);
  assert.equal(state.home.enabled,true);assert.equal(state.history.enabled,false);
});

test('an active stop command makes the reload slot the stop control',()=>{
  const {model}=fixture({'Browser:Stop':node({}),'Browser:Reload':node({disabled:'true'}),'Browser:Back':node({}),
    'reload-button':node({label:'Reload'}),'stop-button':node({label:'Stop',tooltiptext:'Stop loading'})});
  const reload=model.snapshot().reload;
  assert.equal(reload.stopping,true);assert.equal(reload.label,'Stop');assert.equal(reload.tooltip,'Stop loading');
  const idle=fixture({'Browser:Stop':node({disabled:'true'}),'Browser:Reload':node({}),'reload-button':node({label:'Reload'})}).model.snapshot().reload;
  assert.equal(idle.stopping,false);assert.equal(idle.label,'Reload');assert.equal(idle.enabled,true);
});

test('labels fall back from the widget label to its accessible name, the localization and the tooltip',()=>{
  const labels={'Browser:Back':node({}),'back-button':node({'aria-label':'Go back'}),
    'home-button':node({tooltiptext:'Home page'})};
  const state=fixture(labels,{fallback:id=>id==='forward'?'Forward (l10n)':''}).model.snapshot();
  assert.equal(state.back.label,'Go back');
  assert.equal(state.home.label,'Home page');assert.equal(state.home.tooltip,'Home page');
  assert.equal(state.forward.label,'Forward (l10n)');
  assert.equal(state.profile.label,'');
});

test('downloads report visibility, clamped progress and attention',()=>{
  const shown=(attrs,percent)=>fixture({'downloads-button':node(attrs)},{percent}).model.snapshot().downloads;
  assert.deepEqual([shown({},50).shown,shown({},50).progress,shown({},50).attention],[true,-1,'']);
  assert.equal(shown({progress:'true'},250).progress,100);
  assert.equal(shown({progress:'true'},-5).progress,0);
  assert.equal(shown({progress:'true',attention:'warning'},30).attention,'warning');
  assert.equal(fixture({}).model.snapshot().downloads.shown,false);
  assert.equal(shown({hidden:true},0).shown,false);
});

test('overflow is enabled by any visible item in either list',()=>{
  const visible=node({}),hiddenItem=node({hidden:true});
  const {model}=fixture({'widget-overflow-list':node({},[hiddenItem])});
  assert.equal(model.snapshot().overflow.enabled,false);
  assert.equal(fixture({'widget-overflow-fixed-list':node({},[visible])}).model.snapshot().overflow.enabled,true);
});

test('the state reads only the elements it declares, and observes a subset of them',()=>{
  const {model,requested}=fixture({});
  model.snapshot();
  assert.deepEqual([...new Set(requested)].filter(id=>!NAVIGATION_STATE_READS.includes(id)),[]);
  assert.deepEqual(model.observedIds().filter(id=>!NAVIGATION_STATE_READS.includes(id)),[]);
});

test('the command table is frozen with unique ids and a known activation for each',()=>{
  assert.ok(Object.isFrozen(NAVIGATION_COMMANDS));
  assert.equal(new Set(NAVIGATION_COMMANDS.map(c=>c.id)).size,NAVIGATION_COMMANDS.length);
  for(const command of NAVIGATION_COMMANDS) {
    assert.ok(Object.isFrozen(command));
    assert.ok(command.menu || command.method,command.id);
  }
});
