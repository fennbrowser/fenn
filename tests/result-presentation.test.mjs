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
    .replace(/^import\b[^;]*;\r?$/gm,'').replace('export class UrlbarResultPresentation','this.Model = class UrlbarResultPresentation'),context);
  const model=new context.Model({async formatMessages(specs){requests.push(...specs);return specs.map(s=>({value:`Localized ${s.id}`}));}});
  return {model,prefs,requests,shared};
}

test('contextual opt-in selection announcement belongs to the view and preserves rendered text',()=>{
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const start=source.indexOf('  announceSelection(result, element) {');
  const method=source.slice(start,source.indexOf('\n  #selectElement(',start));
  const View=new Function(`return class {${method}}`)(),view=new View(),announcements=[];
  let copies=0,removed=false;
  const description={textContent:'Learn more Description',cloneNode(deep){
    assert.equal(deep,true);copies++;return {firstElementChild:{remove(){removed=true;}},
      get textContent(){return removed?'Description':description.textContent;}};
  }};
  const row={querySelector:selector=>selector.endsWith('-title')?{textContent:'Title'}:description,
    ariaNotify:text=>announcements.push(text)};
  const element={getAttribute:name=>{assert.equal(name,'name');return 'learn_more';},
    closest:selector=>{assert.equal(selector,'.urlbarView-row');return row;}};
  const result={providerName:'UrlbarProviderQuickSuggestContextualOptIn'};
  view.announceSelection(result,element);
  assert.deepEqual(announcements,['Title. Description']);assert.equal(copies,1);
  assert.equal(description.textContent,'Learn more Description');
  view.announceSelection(result,null);view.announceSelection(null,element);
  view.announceSelection({providerName:'Other'},element);
  view.announceSelection(result,{getAttribute:()=> 'allow'});
  assert.equal(announcements.length,1);
  const selection=source.slice(source.indexOf('  #selectElement('));
  assert.ok(selection.indexOf('this.announceSelection(result, element)') < selection.indexOf('parentController.onBeforeSelection'));
  const provider=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/UrlbarProviderQuickSuggestContextualOptIn.sys.mjs',import.meta.url),'utf8');
  assert.doesNotMatch(provider,/onBeforeSelection\(|#a11yAlertRow|\.querySelector\(/);
});

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

test('URL row text uses Firefox display values and its titled-autofill versus visit policy',()=>{
  const {model}=fixture(), calls=[];
  const result={type:1,heuristic:true,providerName:'UrlbarProviderAutofill',autofill:{noVisitAction:true},
    payload:{title:'raw title',url:'https://www.example.test/'},
    getDisplayableValueAndHighlights(field,options){calls.push([field,options]);return {value:field==='title'?'Known site title':'example.test'};}};
  const tokens=[{value:'example'}];
  const text=model.displayText(result,{tokens});
  assert.equal(text.title,'Known site title');assert.equal(text.displayUrl,'example.test');
  assert.equal(text.inlineUrl,true);assert.equal(text.isVisitAction,false);
  assert.deepEqual(calls.map(([field])=>field),['title','url']);
  assert.equal(calls[0][1].tokens,tokens);assert.equal(calls[1][1].isURL,true);
  result.autofill.noVisitAction=false;
  const visit=model.displayText(result,{tokens});
  assert.equal(visit.displayUrl,'');assert.equal(visit.isVisitAction,true);assert.equal(visit.inlineUrl,false);
  result.providerName='UrlbarProviderHistoryUrlHeuristic';
  assert.equal(model.displayText(result,{tokens}).displayUrl,'example.test');
  result.heuristic=false;
  assert.equal(model.displayText(result,{tokens}).inlineUrl,true);
  result.type=5;
  assert.equal(model.displayText(result,{tokens}).inlineUrl,true,'switch-to-tab keeps an inline destination');
  result.isRichSuggestion=true;
  assert.equal(model.displayText(result,{tokens}).inlineUrl,false);
  result.isRichSuggestion=false;
  result.payload.description='Useful detail';
  assert.equal(model.displayText(result,{tokens}).inlineUrl,false);
  delete result.payload.description;
  result.payload.descriptionL10n={id:'pending-description'};
  assert.equal(model.displayText(result,{tokens}).inlineUrl,false,'late localization must not change the row template');
});

test('native emphasis uses the exact formatted title and URL ranges without mutating Firefox caches',()=>{
  const {model}=fixture();
  const title={value:'A Café guide',highlights:[[2,4]]};
  const url={value:'example.test/Café',highlights:[[13,4]]};
  const result={type:1,payload:{title:'raw',url:'https://www.example.test/Caf%C3%A9'},
    getDisplayableValueAndHighlights(field){return field==='title'?title:url;}};
  const text=model.displayText(result,{tokens:[]});
  const plain=value=>JSON.parse(JSON.stringify(value));
  assert.deepEqual(plain(model.highlightRuns(text.title,text.titleHighlights)),[
    {text:'A ',matched:false},{text:'Café',matched:true},{text:' guide',matched:false}]);
  assert.deepEqual(plain(model.highlightRuns(text.displayUrl,text.urlHighlights)),[
    {text:'example.test/',matched:false},{text:'Café',matched:true}]);
  text.titleHighlights.highlights[0][0]=0;
  assert.deepEqual(title.highlights,[[2,4]]);
  result.payload.titleL10n={id:'localized'};
  assert.equal(model.displayText(result,{}).titleHighlights,null);
  delete result.payload.titleL10n;result.payload.text='Extension text';
  assert.equal(model.displayText(result,{}).titleHighlights,null);
});

test('emphasis preserves UTF-16 text, multiple matches and the existing display bound',()=>{
  const {model}=fixture(),plain=value=>JSON.parse(JSON.stringify(value));
  const value='🦊 Fenn / Fenn';
  assert.deepEqual(plain(model.highlightRuns(value,{value,highlights:[[3,4],[10,4]]})),[
    {text:'🦊 ',matched:false},{text:'Fenn',matched:true},{text:' / ',matched:false},{text:'Fenn',matched:true}]);
  const long='a'.repeat(1020)+'matching tail';
  const runs=model.highlightRuns(long,{value:long,highlights:[[1020,8]]});
  assert.equal(runs.map(r=>r.text).join(''),long.slice(0,1024));
  assert.deepEqual(plain(runs[1]),{text:'matc',matched:true});
  assert.equal(model.highlightRuns('new label',{value:'old label',highlights:[[0,3]]}).length,0);
  for(const highlights of [[[0,0]],[[-1,2]],[[0,99]],[[0.5,1]],[[0,3],[2,1]],[[2,1],[0,1]],Array(65).fill([0,1])])
    assert.equal(model.highlightRuns('example',{value:'example',highlights}).length,0);
});

test('URL visibility keeps clipboard, search and sponsored-result exceptions',()=>{
  const {model}=fixture();
  const result={type:1,payload:{url:'https://example.test/'},providerName:'UrlbarProviderClipboard'};
  assert.equal(model.urlPresentation(result).showUrl,false);
  result.providerName='UrlbarProviderQuickSuggest';
  assert.equal(model.urlPresentation(result).showUrl,false);
  result.payload.shouldShowUrl=true;assert.equal(model.urlPresentation(result).showUrl,true);
  result.type=2;assert.equal(model.urlPresentation(result).showUrl,false);
  result.type=5;assert.equal(model.urlPresentation(result).showUrl,true);
  result.type=1;result.payload.providesSearchMode=true;
  assert.equal(model.urlPresentation(result).showUrl,false);
});

test('original renderer and native adapter share policy and delete native view helpers',()=>{
  const view=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const session=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs',import.meta.url),'utf8');
  for(const method of ['title','icon','allowEmptySelection','shouldShowHeuristic','defaultSelection','arrowSelection']) assert.ok(view.includes(`this.input.resultPresentation.${method}(`));
  for(const method of ['searchModeTitle','icon','allowEmptySelection','candidates']) assert.ok(session.includes(`this.resultPresentation.${method}(`));
  assert.doesNotMatch(view,/prepareNativeSearchModeTitle|nativePresentationResults|nativeResultPresentation/);
});

test('arrow traversal preserves boundaries, empty selection, amount clamping and lazy eligibility',()=>{
  const {model}=fixture();
  const cases=[
    // selected, count, amount, reverse, empty, eligible, descriptor, inspected
    [2,3,1,false,true,[true,true,true],{kind:'clear'},[]],
    [2,3,1,false,false,[false,true,true],{kind:'row',index:0},[]],
    [0,3,1,true,true,[true,true,true],{kind:'clear'},[]],
    [0,3,1,true,false,[true,true,false],{kind:'row',index:2},[]],
    [-1,3,9,false,true,[false,true,true],{kind:'row',index:1},[0,1]],
    [-1,3,9,true,true,[true,false,false],{kind:'row',index:0},[2,1,0]],
    [0,4,2,false,false,[true,false,true,true],{kind:'row',index:2},[2]],
    [0,4,99,false,false,[true,true,true,true],{kind:'row',index:3},[3]],
    [3,4,99,true,false,[true,true,true,true],{kind:'row',index:0},[0]],
    [0,3,1,false,false,[true,false,false],{kind:'row',index:-1},[1,2]],
    [2,3,1,true,false,[false,false,true],{kind:'row',index:-1},[1,0]],
    [-1,1,1,false,false,[true],{kind:'row',index:0},[0]],
    [-1,0,1,false,true,[],{kind:'clear'},[]],
    [-1,0,1,true,true,[],{kind:'row',index:-1},[]],
  ];
  for(const [selectedRowIndex,rowCount,amount,reverse,allowEmptySelection,eligible,expected,inspected] of cases){
    const seen=[];
    const result=model.arrowSelection({selectedRowIndex,rowCount,amount,reverse,allowEmptySelection,
      isSelectable:index=>{seen.push(index);return eligible[index];}});
    assert.deepEqual({...result},expected,JSON.stringify({selectedRowIndex,rowCount,amount,reverse,allowEmptySelection}));
    assert.deepEqual(seen,inspected);assert.ok(Object.isFrozen(result));
  }
});

test('original arrow binding freezes before policy and preserves distinct selection application paths',()=>{
  const {model}=fixture();
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarView.mjs',import.meta.url),'utf8');
  const method=source.slice(source.indexOf('  selectBy(amount,'),source.indexOf('    // Tab key handling below.'))+'\n  }';
  const Binding=new Function(`return class {
    #selectElement(value){this.calls.push(['clear',value]);this.index=-1;}
    #isRowArrowSelectable(index){this.calls.push(['eligible',index]);return this.eligible[index];}
    get selectedRowIndex(){return this.index;}
    set selectedRowIndex(value){this.calls.push(['row',value]);this.index=value;}
    ${method}
  }`)();
  const binding=new Binding();binding.calls=[];binding.index=0;binding.isOpen=true;
  binding.visibleRowCount=3;binding.eligible=[true,false,true];binding.allowEmptySelection=true;
  binding.input={resultPresentation:model,eventBufferer:{isDeferringEvents:false}};
  binding.controller={cancelQuery:()=>binding.calls.push(['freeze'])};
  binding.selectBy(1);
  assert.deepEqual(binding.calls,[['freeze'],['eligible',1],['eligible',2],['row',2]]);
  binding.calls=[];binding.selectBy(1);
  assert.deepEqual(binding.calls,[['freeze'],['clear',null]]);
  binding.calls=[];binding.input.eventBufferer.isDeferringEvents=true;binding.selectBy(1,{reverse:true});
  assert.deepEqual(binding.calls,[['eligible',2],['row',2]]);
  binding.calls=[];binding.isOpen=false;assert.throws(()=>binding.selectBy(1),/isn't open/);
  assert.deepEqual(binding.calls,[]);
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
    vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarUrlValue.mjs',import.meta.url),'utf8')
      .replace(/^import\b[^;]*;$/gm,'').replaceAll('export function ','function '),context);
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
