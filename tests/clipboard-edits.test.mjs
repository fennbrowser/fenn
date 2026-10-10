import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {planUrlbarCut,planUrlbarPaste} from '../upstream/firefox/browser/components/urlbar/content/UrlbarClipboardText.mjs';

test('cut plans an exact UTF-16 replacement without owning editor state',()=>{
  const state={text:'a😀b',start:1,end:3};
  assert.deepEqual(planUrlbarCut(state),{before:'a😀b',replaceStart:1,replaceEnd:3,
    replacement:'',text:'ab',start:1,end:1});
  assert.equal(state.text,'a😀b');
  assert.equal(planUrlbarCut({...state,readOnly:true}),null);
  assert.equal(planUrlbarCut({...state,end:1}),null);
  for(const patch of [{start:-1},{end:8},{start:2.5},{start:3,end:2},{text:null}])
    assert.equal(planUrlbarCut({...state,...patch}),null);
});

test('paste skips fixup for empty data, invalid selection and a non-whitespace prefix',()=>{
  const services=new Proxy({}, {get(){throw Error('Fixup must remain lazy');}});
  for(const [state,text] of [[{text:'prefix',start:6,end:6},'javascript:hello'],
    [{text:'',start:0,end:0},''],[{text:'abc',start:-1,end:2},'paste']]) {
    assert.equal(planUrlbarPaste(state,text,services),null);
  }
});

test('sanitization retains private fixup context, whitespace prefix and unselected suffix',()=>{
  const calls=[],fixup={preferredURI:'opaque'};
  const edit=planUrlbarPaste({text:'  old tail',start:2,end:5,isPrivate:true},'javascript:new',{
    fixup(text,isPrivate){calls.push([text,isPrivate]);return fixup;},
    sanitize(text,options){assert.equal(options,fixup);assert.equal(text,'javascript:new');return 'new';},
  });
  assert.deepEqual(calls,[['javascript:new',true]]);
  assert.deepEqual(edit,{before:'  old tail',replaceStart:2,replaceEnd:5,replacement:'new',
    text:'  new tail',start:5,end:5});
});

test('unchanged clipboard text delegates all newline and undo behavior to the trusted editor',()=>{
  const original='a\r\nb\n';
  assert.equal(planUrlbarPaste({text:'selected',start:0,end:8},original,{
    fixup:()=>({}),sanitize:text=>text,
  }),null);
});

// Execute both actual Firefox paste handlers, without constructing an input.
// The host fixtures provide only the operations that apply the planned edit.
for(const name of ['UrlbarInputBase','SmartbarInput']) {
  test(`${name} paste adapter preserves event cancellation, caret and query policy`,()=>{
    const source=readFileSync(new URL(`../upstream/firefox/browser/components/urlbar/content/${name}.mjs`,import.meta.url),'utf8');
    const begin=source.indexOf('  _on_paste(event) {'),end=source.indexOf('\n  /**',begin);
    const context=vm.createContext({planUrlbarPaste,
      UrlbarShared:{sanitizeTextFromClipboard:()=> 'safe'},
      UrlbarContentUtils:{getFixupPrimitives:()=>({})}});
    const handle=vm.runInContext(`({${source.slice(begin,end)}})._on_paste`,context);
    const calls=[],input={value:'old',selectionStart:0,selectionEnd:3,isPrivate:true,
      setValue(value,options){this.value=value;this._untrimmedValue=value;calls.push(['value',options.valueIsTyped]);},
      getAttribute:()=> 'valid',setPageProxyState:value=>calls.push(['proxy',value]),
      toggleAttribute:(...args)=>calls.push(['attribute',...args]),
      setSelectionRange:(...args)=>calls.push(['caret',...args]),
      startQuery:options=>calls.push(['query',options]),
    };
    input.inputField=input;
    const event={clipboardData:{getData:()=> 'javascript:safe'},
      preventDefault:()=>calls.push(['prevent']),stopImmediatePropagation:()=>calls.push(['stop'])};
    handle.call(input,event);
    assert.equal(input.value,'safe');assert.equal(input.userTypedValue,'safe');
    assert.deepEqual(calls.slice(0,3),[['prevent'],['stop'],['value',true]]);
    assert.ok(calls.some(c=>c[0]==='proxy'&&c[1]==='invalid'));
    assert.deepEqual(calls.find(c=>c[0]==='caret'),['caret',4,4]);
    const query=calls.find(c=>c[0]==='query')[1];
    assert.equal(query.allowAutofill,false);assert.equal(query.resetSearchState,false);assert.equal(query.event,event);
    calls.length=0;event.clipboardData.getData=()=> 'safe';handle.call(input,event);
    assert.deepEqual(calls,[],'unchanged paste leaves default editor behavior intact');
  });
}
