import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const root=new URL('../upstream/firefox/browser/components/urlbar/content/',import.meta.url);
const context=vm.createContext({URL,UrlbarContentUtils:{},UrlbarPrefs:{}});
const shared=readFileSync(new URL('UrlbarShared.mjs',root),'utf8');
vm.runInContext(shared.replace(/^import .*;$/gm,'').replace('export const UrlbarShared','var UrlbarShared'),context);
vm.runInContext(readFileSync(new URL('UrlbarAutofillModel.mjs',root),'utf8').replace(/^import .*;$/gm,'')
  .replace('export class UrlbarAutofillModel','this.Model=class UrlbarAutofillModel'),context);
const M=context.Model;
const placeholder={value:'Example.test/path/',type:'url',selectionStart:3,selectionEnd:18,untrimmedValue:'https://Example.test/path/'};
const facts={searchString:'Example.test/p',value:'Example.test/p',selectionStart:14,selectionEnd:14,placeholder,enabled:true};
const plan=changes=>M.planPlaceholder({...facts,...changes});

test('first-result admission preserves caret, selected-placeholder and override rules',()=>{
  const base={searchString:'Exa',selectionStart:3,selectionEnd:3};
  assert.equal(M.acceptsResult(base),true);
  for(const [start,end] of [[0,3],[2,2],[4,4]]) assert.equal(M.acceptsResult({...base,selectionStart:start,selectionEnd:end}),false);
  assert.equal(M.acceptsResult({...base,placeholder,selectionEnd:placeholder.value.length}),true);
  assert.equal(M.acceptsResult({...base,placeholder:{value:'else.test/'},selectionEnd:10}),false);
  assert.equal(M.acceptsResult({...base,selectionStart:0,ignoresSelection:true}),true);
  assert.equal(M.acceptsResult({...base,ignoresSelection:true,handlingMentions:true}),false);
});

test('remote mode, caret and mentions clear displayed completion; local modes allow it',()=>{
  for(const change of [{selectionEnd:2},{searchMode:{engineName:'engine'}},
    {searchMode:{source:context.UrlbarShared.RESULT_SOURCE.SEARCH}},{handlingMentions:true}]) {
    assert.equal(plan(change).operation,'clear');assert.equal(plan(change).allowAutofill,false);
  }
  assert.equal(plan({searchMode:{source:context.UrlbarShared.RESULT_SOURCE.HISTORY}}).operation,'apply');
});

test('ordinary URL matching retains Firefox slash, case and fragment policy',()=>{
  assert.equal(plan({}).operation,'apply');
  assert.equal(plan({placeholder:{value:'localhost:12345/',type:'url'},searchString:'localho',value:'localho',selectionEnd:7}).operation,'discard','keep Firefox scheme-less host/port matching');
  assert.equal(plan({placeholder:{value:'http://localhost:12345/',type:'url'},searchString:'http://localho',value:'http://localho',selectionEnd:14}).operation,'apply');
  for(const text of ['Other','Example.test/','Example.test/path/']) {
    assert.equal(plan({searchString:text,value:text,selectionEnd:text.length}).operation,'discard');
  }
  assert.equal(plan({searchString:'eXample.test/P',value:'eXample.test/P',selectionEnd:14}).autofill.value,'eXample.test/Path/');
  const hash={...placeholder,value:'example.test/path#Case'};
  for(const [text,expected] of [['example.test/path#C','apply'],['example.test/path#c','discard']]) {
    assert.equal(plan({placeholder:hash,searchString:text,value:text,selectionEnd:text.length}).operation,expected);
  }
});

test('adaptive matches use typed-history threshold and preserve metadata without mutation',()=>{
  for(const type of ['adaptive_url','adaptive_origin']) {
    const p=Object.freeze({...placeholder,type,adaptiveHistoryInput:'Exam'});
    assert.equal(plan({placeholder:p,searchString:'Exa',value:'Exa',selectionEnd:3}).operation,'discard');
    const result=plan({placeholder:p,searchString:'exAM',value:'exAM',selectionEnd:4});
    assert.equal(result.autofill.value,'exAMple.test/path/');
    assert.equal(result.autofill.untrimmedValue,p.untrimmedValue);
    assert.equal(result.autofill.type,type);assert.equal(result.autofill.adaptiveHistoryInput,'Exam');
    assert.equal(result.autofill.selectionStart,4);assert.equal(result.autofill.selectionEnd,p.value.length);
  }
});

test('valid suppressed completion is retained; missing or mismatched placeholder is discarded',()=>{
  assert.equal(plan({enabled:false}).operation,'retain');
  assert.equal(plan({value:'Example.test/path/'}).operation,'retain');
  assert.equal(plan({placeholder:null}).operation,'discard');
  const p={...placeholder,type:'adaptive_url',adaptiveHistoryInput:'Example.test/path/'};
  const full={placeholder:p,searchString:p.value,value:p.value,selectionStart:p.value.length,selectionEnd:p.value.length};
  assert.equal(plan(full).operation,'apply','original editor still applies equal adaptive value');
  assert.equal(plan({...full,suppressRedundantEdit:true}).operation,'retain','Smartbar avoids redundant synchronous selection event');
});

test('both actual input bindings apply plans through their existing editor methods',()=>{
  context.UrlbarAutofillModel=M;
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) {
    const source=readFileSync(new URL(name,root),'utf8');
    const method=name=>{const start=source.indexOf('  '+name+'(');return source.slice(start,source.indexOf('\n  }\n',start)+4);};
    vm.runInContext('this.Binding=class {#clearAutofill(){this.cleared=true;}'+method('_maybeAutofillPlaceholder')+method('_autofillFirstResult')+'}',context);
    const b=new context.Binding();Object.assign(b,{value:facts.value,selectionStart:14,selectionEnd:14,
      _autofillPlaceholder:placeholder,_enableAutofillPlaceholder:true,inputField:{isHandlingMentions:false},
      _lastSearchString:facts.searchString,_autofillValue(v){this.applied=v;},setValueFromResult(v){this.result=v;}});
    assert.equal(b._maybeAutofillPlaceholder(facts.searchString),true);assert.equal(b.applied.value,placeholder.value);
    const result={autofill:{value:placeholder.value}};b._autofillFirstResult(result);assert.equal(b.result.result,result);
    b.result=null;b._autofillFirstResult({});assert.equal(b.result,null);
    b.selectionEnd=0;assert.equal(b._maybeAutofillPlaceholder(facts.searchString),false);assert.equal(b.cleared,true);
    if(name==='SmartbarInput.mjs') {
      b.selectionEnd=14;b.inputField.isHandlingMentions=true;b.result=null;
      b._autofillFirstResult(result);assert.equal(b.result,null);
      assert.equal(b._maybeAutofillPlaceholder(facts.searchString),false);
    }
  }
});


test('explicit autofill deselection requires the displayed placeholder and changed range',()=>{
  const base={placeholder,value:placeholder.value,selectionStart:placeholder.selectionStart,selectionEnd:placeholder.selectionEnd};
  assert.equal(M.confirmsSelection(base),false);
  assert.equal(M.confirmsSelection({...base,selectionStart:0}),true);
  assert.equal(M.confirmsSelection({...base,selectionEnd:placeholder.selectionEnd-1}),true);
  assert.equal(M.confirmsSelection({...base,value:'other',selectionStart:0}),false);
  assert.equal(M.confirmsSelection({...base,placeholder:null,selectionStart:0}),false);
});

test('retained selection confirmation clears the placeholder before committing displayed text',()=>{
  const source=readFileSync(new URL('UrlbarInputBase.mjs',root),'utf8');
  const start=source.indexOf('  _on_selectionchange() {');const method=source.slice(start,source.indexOf('  _on_select()',start));
  const Input=new Function('UrlbarAutofillModel',`return class {${method}}`)(M),input=new Input(),writes=[];
  input._autofillPlaceholder=placeholder;input.value=placeholder.value;
  input.selectionStart=placeholder.selectionStart;input.selectionEnd=placeholder.selectionEnd;
  Object.defineProperty(input,'userTypedValue',{set:value=>writes.push([value,input._autofillPlaceholder])});
  input._on_selectionchange();assert.equal(writes.length,0);
  input.selectionStart=0;input._on_selectionchange();assert.deepEqual(writes,[[placeholder.value,null]]);
  Object.defineProperty(input,'value',{get:()=>assert.fail('no placeholder must not read the field')});
  input._on_selectionchange();assert.equal(writes.length,1);
});
