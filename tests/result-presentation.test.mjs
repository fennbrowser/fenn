import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

function fixture() {
  const prefs = {'experimental.hideHeuristic':false, 'searchRestrictKeywords.featureGate':true};
  const requests=[];
  const shared={RESULT_TYPE:{URL:1,SEARCH:2,KEYWORD:3,OMNIBOX:4,TAB_SWITCH:5,DYNAMIC:6,TIP:7,AI_CHAT:8,RESTRICT:9},
    RESULT_SOURCE:{HISTORY:1}, ICON:{HISTORY:'history',TRENDING:'trending',SEARCH_GLASS:'search',DEFAULT:'page'},
    getSpanForResult:r=>r.span || 1};
  const context=vm.createContext({UrlbarShared:shared,
    UrlbarPrefs:{get:key=>prefs[key],getScotchBonnetPref:key=>prefs[key]}});
  vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarResultPresentation.mjs',import.meta.url),'utf8')
    .replace(/^import .*;\r?$/gm,'').replace('export class UrlbarResultPresentation','this.Model = class UrlbarResultPresentation'),context);
  const model=new context.Model({async formatMessages(specs){requests.push(...specs);return specs.map(s=>({value:`Localized ${s.id}`}));}});
  return {model,prefs,requests,shared};
}

test('candidate spans exclude hidden exposures and respect live heuristic policy',()=>{
  const {model,prefs}=fixture();
  const heuristic={heuristic:true,type:2},hidden={isHiddenExposure:true,span:99},tip={type:7,span:3},tail={type:2};
  const context={results:[hidden,heuristic,tip,tail],heuristicResult:heuristic,maxResults:4};
  assert.deepEqual(model.candidates(context),[heuristic,tip]);
  assert.equal(model.allowEmptySelection(context),false);
  prefs['experimental.hideHeuristic']=true;
  assert.deepEqual(model.candidates(context),[tip,tail]);
  assert.equal(model.allowEmptySelection(context),true);
  assert.equal(model.shouldShowHeuristic({heuristic:true,type:7}),true);
  assert.equal(model.allowEmptySelection(null),true);
  assert.throws(()=>model.shouldShowHeuristic(tip),/heuristic/);
  assert.equal(context.results.length,4,'policy does not mutate controller results');
});

test('overflow never admits later rows after an oversized candidate',()=>{
  const {model}=fixture();
  assert.deepEqual(model.candidates({maxResults:2,results:[{span:3},{span:1}]}),[]);
});

test('local modes and engine aliases retain Firefox localization arguments',async()=>{
  const {model,prefs,requests}=fixture();
  const local={type:9,payload:{providesSearchMode:true,l10nRestrictKeywords:['History','Historique']}};
  assert.equal(await model.searchModeTitle(local),'Localized urlbar-result-search-with-local-search-mode');
  assert.equal(requests[0].args.keywords,'@history, @historique');
  assert.equal(requests[0].args.localSearchMode,'history');
  const engine={type:2,providerName:'UrlbarProviderTokenAliasEngines',payload:{providesSearchMode:true,engine:'Engine',keywords:'@e'}};
  await model.searchModeTitle(engine);
  assert.equal(requests[1].id,'urlbar-result-search-with-engine-keywords');
  assert.equal(requests[1].args.engine,'Engine');
  prefs['searchRestrictKeywords.featureGate']=false;
  await model.searchModeTitle(engine);
  assert.equal(requests[2].id,'urlbar-result-action-search-w-engine');
});

test('explicit localization and legacy text take precedence without constructing nodes',async()=>{
  const {model,requests}=fixture();
  const result={type:9,payload:{providesSearchMode:true,titleL10n:{id:'explicit'},text:'legacy'}};
  assert.equal(await model.searchModeTitle(result),'Localized explicit');
  delete result.payload.titleL10n;
  assert.equal(await model.searchModeTitle(result),'legacy');
  assert.equal(requests.length,1);
  assert.equal(model.title({payload:{title:'ordinary'}}),null);
  assert.equal(await model.searchModeTitle({payload:{}}),'');
});

test('icon precedence and blob lifetime stay with the presenter',()=>{
  const {model}=fixture(),blob={opaque:true};
  const result={type:2,source:1,payload:{icon:'explicit',iconBlob:blob,trending:true}};
  assert.equal(model.icon(result,'override'),'history');
  result.source=0;
  assert.equal(model.icon(result,'override'),'override');
  assert.equal(model.icon(result),'explicit');
  delete result.payload.icon;
  assert.equal(model.icon(result),blob);
  assert.equal(model.icon(result,null,(owner,value)=>{assert.equal(owner,result);assert.equal(value,blob);return 'blob:url';}),'blob:url');
  assert.equal(model.icon(result,null,()=>null),'trending');
  delete result.payload.iconBlob;delete result.payload.trending;
  assert.equal(model.icon(result),'search');
  result.type=1;assert.equal(model.icon(result),'page');
});

test('original renderer and native adapter share policy and delete native view helpers',()=>{
  const view=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const session=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8');
  for(const method of ['title','icon','allowEmptySelection','shouldShowHeuristic','defaultSelection']) assert.ok(view.includes(`this.input.resultPresentation.${method}(`));
  for(const method of ['searchModeTitle','icon','allowEmptySelection','candidates']) assert.ok(session.includes(`this.input.resultPresentation.${method}(`));
  assert.doesNotMatch(view,/prepareNativeSearchModeTitle|nativePresentationResults|nativeResultPresentation/);
});


test('default selection separates visible heuristic selection from hidden value association',()=>{
  const {model,prefs}=fixture(),first={heuristic:true,type:2,payload:{}};
  const query={results:[first],trimmedSearchString:'words'};
  let plan=model.defaultSelection(query);
  assert.equal(plan.kind,'select-first');assert.equal(plan.valueResult,null);assert.ok(Object.isFrozen(plan));
  prefs['experimental.hideHeuristic']=true;
  plan=model.defaultSelection(query);assert.equal(plan.kind,'associate-first');assert.equal(plan.valueResult,first);
  first.type=7;assert.equal(model.defaultSelection(query).kind,'select-first','heuristic tips remain visible');
  assert.equal(query.results[0],first);
});

test('filtered search-mode offers associate input while bare at-sign, ordinary and empty queries do not',()=>{
  const {model}=fixture(),offer={payload:{providesSearchMode:true}};
  assert.equal(model.defaultSelection({results:[offer],trimmedSearchString:'@eng'}).valueResult,offer);
  assert.equal(model.defaultSelection({results:[offer],trimmedSearchString:'@'}).kind,'none');
  for(const query of [null,{}, {results:[]},{results:[{payload:{}}],trimmedSearchString:'words'}]) {
    const plan=model.defaultSelection(query);assert.equal(plan.kind,'none');assert.equal(plan.valueResult,null);
  }
});


test('result input text preserves search aliases, suggestions and provider text precedence',()=>{
  const {model,shared}=fixture(),T=shared.RESULT_TYPE;
  for(const [type,payload,value] of [
    [T.KEYWORD,{input:'keyword input'},'keyword input'],
    [T.SEARCH,{keyword:'@engine',suggestion:'suggested',query:'typed'},'@engine suggested'],
    [T.SEARCH,{suggestion:'',query:'typed'},'typed'],
    [T.OMNIBOX,{content:'extension input'},'extension input'],
    [T.RESTRICT,{autofillKeyword:'@history'},'@history '],
    [T.AI_CHAT,{query:'question'},'question'],[T.AI_CHAT,{},''],
  ]) {
    const text=model.inputText({type,payload});assert.equal(text.value,value);assert.ok(Object.isFrozen(text));
  }
  assert.equal(model.actionType({type:T.OMNIBOX}),'extension');
  assert.equal(model.actionType({type:T.TAB_SWITCH}),'switchtab');
  assert.equal(model.actionType({type:T.SEARCH}),undefined);
});

test('dynamic and tip text consume explicit facts and distinguish empty text from URL fallback',()=>{
  const {model,shared}=fixture(),T=shared.RESULT_TYPE;
  const result={type:T.DYNAMIC,payload:{input:'input',query:'query'}};
  assert.equal(model.inputText(result,{dynamicQuery:'child query',dynamicUrl:'child url'}).value,'child query');
  assert.equal(model.inputText(result,{dynamicQuery:'',dynamicUrl:'child url'}).value,'child url');
  assert.equal(model.inputText(result).value,'input');delete result.payload.input;
  assert.equal(model.inputText(result).value,'query');delete result.payload.query;
  assert.equal(model.inputText(result).value,'');
  const tip={type:T.TIP,payload:{url:'fallback'}};
  assert.equal(model.inputText(tip,{resultAction:{url:'action url',input:'action input'}}).value,'action url');
  assert.equal(model.inputText(tip,{resultAction:{input:'action input'}}).value,'action input');
  assert.equal(model.inputText(tip,{resultAction:{url:'',input:''}}),null);
  for(const type of [T.URL,T.TAB_SWITCH,999])assert.equal(model.inputText({type,payload:{url:'https://example.test'}}),null);
});

test('both original input bindings use the model before their retained URL fallback',()=>{
  const {model,shared}=fixture();
  for(const name of ['UrlbarInputBase','SmartbarInput']) {
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${name}.mjs`,import.meta.url),'utf8');
    const start=source.indexOf('  #getValueFromResult('),end=source.indexOf('\n  }',start)+4;
    const method=source.slice(start,end);
    const context=vm.createContext({URL,UrlbarShared:shared,UrlbarResultActions:{fromElement:e=>e?.action},
      losslessDecodeURL:url=>url.href,losslessDecodeURI:()=>{throw Error('unexpected URL fallback');},
      UrlbarContentUtils:{getFixupPrimitives:()=>({})},SCHEMELESS_INPUT_SCHEMELESS:2});
    vm.runInContext(`this.Binding=class {#isAddressbar=false; #getSchemelessInput(){return 2;}
      ${method}
      value(result,options){return this.#getValueFromResult(result,options);}
    }`,context);
    const binding=new context.Binding();binding.resultPresentation=model;
    assert.equal(binding.value({type:shared.RESULT_TYPE.SEARCH,payload:{query:'terms'}},{urlOverride:'https://override.test/'}),'terms');
    assert.equal(binding.value({type:shared.RESULT_TYPE.DYNAMIC,payload:{}},{element:{dataset:{query:'child'}}}),'child');
    assert.equal(binding.value({type:shared.RESULT_TYPE.TIP,payload:{}},{element:{action:{input:'tip'}}}),'tip');
    assert.equal(binding.value({type:shared.RESULT_TYPE.URL,payload:{}},{urlOverride:''}),'');
    assert.equal(binding.value({type:shared.RESULT_TYPE.URL,payload:{url:'invalid URL'}}),'');
  }
});
