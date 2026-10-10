import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

function fixture() {
  const prefs = {'contextMenu.featureGate':true, 'privacy.userContext.enabled':true,
    'resultMenu.keyboardAccessible':false, 'groupLabels.enabled':true};
  const calls=[];
  const context=vm.createContext({
    UrlbarPrefs:{get:key=>prefs[key],toggleResultMenuKeyboardAccessible(){prefs['resultMenu.keyboardAccessible']=!prefs['resultMenu.keyboardAccessible'];}},
    UrlbarContentUtils:{getContainers:async()=>[{name:'Work',userContextId:3,iconURL:'icon',colorCode:'blue'}],getSupportUrl:topic=>'support:'+topic},
    UrlbarShared:{RESULT_TYPE:{URL:1,TIP:7,TAB_SWITCH:2},RESULT_SOURCE:{OTHER_LOCAL:1},getLoadRequestFromResult:r=>r.payload.url && {}},
    UrlbarResult:class {constructor(data){Object.assign(this,data);}},
  });
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarResultActions.mjs',import.meta.url),'utf8')
    .replace(/^import .*;\r?$/gm,'').replace('export class UrlbarResultActions','this.Actions = class UrlbarResultActions'),context);
  const input={handlesOpenInCommands:true,document:{l10n:{formatMessages:async specs=>specs.map(({id})=>({value:'Localized '+id,attributes:[{name:'accesskey',value:'L'}]}))}},
    window:{getHelpLinkURL:topic=>'https://help.test/'+topic},
    view:{updateMenuButtonKeyboardAccessibility(){calls.push('accessibility');}},
    controller:{parentController:{openContainerCreationPanel:arg=>calls.push(['create',arg]),openPreferences:(...args)=>calls.push(['preferences',...args])}},
    pickResult:options=>calls.push(options)};
  return {model:new context.Actions(input),Actions:context.Actions,input,prefs,calls};
}
test('shared menu preserves Firefox order, containers, policy and keyboard checkbox',async()=>{
  const f=fixture(),result={type:1,payload:{url:'https://example.test',isBlockable:true,helpUrl:'https://help.test',isManageable:true}};
  let items=await f.model.describeMenu(result);
  assert.deepEqual(Array.from(items.filter(i=>!i.separator),i=>i.action.openIn || i.action.command),
    ['tab','container-tab','window','private-window','dismiss','help','manage','toggle-keyboard-accessible']);
  assert.equal(items[1].children[0].action.userContextId,'3');
  assert.equal(items[1].children[0].label,'Work');
  assert.equal(items.at(-1).checked,true);
  f.input.isPrivate=true;
  items=await f.model.describeMenu(result);
  assert.ok(!items.some(i=>i.children));
  f.prefs['contextMenu.featureGate']=false;
  assert.equal(await f.model.describeMenu(result),null);
});
test('provider menu commands invalidate independently and button menus keep their identity',async()=>{
  const f=fixture(),result={type:7,payload:{buttons:[{menu:[{name:'custom',l10n:{id:'custom'}}]}]},commands:[{name:'first',l10n:{id:'first'}}]};
  assert.equal(f.model.getResultMenuCommands(result)[0].name,'first');
  result.commands=[{name:'second',l10n:{id:'second'}}];f.model.invalidate(result);
  assert.equal(f.model.getResultMenuCommands(result)[0].name,'second');
  f.model.buttons(result);
  assert.equal((await f.model.describeMenu(result,'0'))[0].action.command,'custom');
  assert.equal(await f.model.describeMenu(result,'absent'),null);
});
test('menu execution uses shared Firefox handlers without a node or event dispatch',()=>{
  const f=fixture(),result={payload:{}},event={type:'click'};
  f.model.invokeMenu(result,{command:'dismiss'},event,42);
  assert.equal(f.calls[0].result,result);assert.equal(f.calls[0].browserId,42);
  assert.equal(f.calls[0].element,undefined);
  f.model.invokeMenu(result,{command:'help'},event);
  assert.equal(f.calls[1].resultAction.url,'support:awesome-bar-result-menu');
  f.model.invokeMenu(result,{operation:'add-container'},event);
  f.model.invokeMenu(result,{operation:'manage-containers'},event);
  assert.equal(f.calls[2][0],'create');assert.equal(f.calls[3][0],'preferences');
  f.model.invokeMenu(result,{command:'toggle-keyboard-accessible'},event);
  assert.equal(f.prefs['resultMenu.keyboardAccessible'],true);
  assert.equal(f.calls.at(-1),'accessibility');
});
test('tip descriptors retain command, URL, input, disabled state and stable names',async()=>{
  const f=fixture(),result={type:7,payload:{buttons:[{l10n:{id:'first'},input:'rewritten',command:'rewrite'},
    {name:'visit',url:'https://example.test',l10n:{id:'second'},attributes:{disabled:true}}]}};
  const buttons=await f.model.describeButtons(result);
  assert.equal(buttons[0].name,'0');assert.equal(buttons[0].action.input,'rewritten');
  assert.equal(buttons[1].name,'visit');assert.equal(buttons[1].enabled,false);
  assert.equal(buttons[1].action.url,'https://example.test');
  result.payload.type='dismissalAcknowledgment';
  assert.equal(await f.model.describeButtons(result),null);
  assert.equal((await f.model.describeButtons(result,true)).length,2);
});
test('rich help keeps translated ordering and entities without parsing a DOM tree',()=>{
  const f=fixture(),result={payload:{descriptionLearnMoreTopic:'topic'}};
  const value=f.model.description(result,'&lt;before&gt; <a data-l10n-name="learn-more-link">A &amp; B</a> &#x1f600;');
  assert.equal(value.before,'<before> ');assert.equal(value.label,'A & B');assert.equal(value.after,' 😀');
  assert.equal(value.action.url,'https://help.test/topic');
  assert.equal(f.model.description(result,'<strong>unsupported</strong>').unsupported,true);
  assert.equal(f.model.description(result,'Text <a data-l10n-name="learn-more-link">&unknown;</a>').unsupported,true);
});
test('acknowledgment result shares Firefox semantics but never mutates query results',()=>{
  const f=fixture(),result={type:1,rowLabel:{id:'group'},payload:{}},context={results:[result]};
  const tip=f.model.createDismissalAcknowledgment(result,{id:'feedback'},context);
  assert.equal(tip.payload.type,'dismissalAcknowledgment');assert.equal(tip.rowLabel,result.rowLabel);
  assert.equal(context.results[0],result);assert.equal(context.results.length,1);
});

test('button boolean attributes match Firefox DOM attribute application',async()=>{
  const f=fixture();
  for(const [disabled,expected] of [[false,true],[null,true],[undefined,true],[true,false],['false',false]]) {
    const result={type:7,payload:{buttons:[{attributes:{disabled},l10n:{id:'label'}}]}};
    assert.equal((await f.model.describeButtons(result))[0].enabled,expected);
  }
});

test('semantic telemetry preserves command identity and original DOM action buttons',()=>{
  class MouseEvent {constructor(){this.type='click';this.target=null;}}
  const context=vm.createContext({MouseEvent,UrlbarShared:{isInstance:(event,type)=>event instanceof type}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarTelemetryUtils.mjs',import.meta.url),'utf8')
    .replace(/^import .*;\r?$/gm,'').replace('export class UrlbarTelemetryUtils','this.Telemetry = class UrlbarTelemetryUtils'),context);
  assert.equal(context.Telemetry.actionFromEvent(new MouseEvent(),{resultAction:{command:'custom'}}),'custom');
  assert.equal(context.Telemetry.actionFromEvent(new MouseEvent(),{resultAction:{command:'dismiss_autofill'}}),'dismiss');
  assert.equal(context.Telemetry.actionFromEvent(new MouseEvent(),{resultAction:{command:'help'}}),'click');
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  telemetryTypeFromElement('),end=source.indexOf('\n  /**',start);
  const view=vm.runInContext('({'+source.slice(start,end)+',telemetryTypeFromResult(){return "result";}})',context);
  const element={dataset:{},classList:{contains:name=>name==='urlbarView-action-btn'}};
  assert.equal(view.telemetryTypeFromElement({},element,{}),'action');
  assert.equal(view.telemetryTypeFromElement({},null,{help:true}),'help');
});

test('shared Firefox menu semantics do not inherit native presentation capacity limits',async()=>{
  const f=fixture();
  const commands=Array.from({length:129},(_,index)=>({name:String(index),l10n:{id:'item'}}));
  assert.equal((await f.model.describeCommands(commands)).length,129);
});
