import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const root=new URL('../upstream/firefox/browser/components/urlbar/content/',import.meta.url);
const c=vm.createContext({});
vm.runInContext(readFileSync(new URL('UrlbarShared.mjs',root),'utf8').replace(/^import .*;$/gm,'').replace('export const UrlbarShared','var UrlbarShared'),c);
vm.runInContext(readFileSync(new URL('UrlbarCompositionModel.mjs',root),'utf8').replace(/^import .*;$/gm,'').replace('export class UrlbarCompositionModel','this.Model=class UrlbarCompositionModel'),c);
const M=c.Model,S=c.UrlbarShared.COMPOSITION;

test('commit is consumed by exactly one input and selection clears before transition',()=>{
  const m=new M();m.begin();m.setPopupClosed(true);assert.equal(m.isComposing,true);
  const live=m.input(true);assert.equal(live.state,S.COMPOSING);
  let cleared=0;assert.equal(m.end(true,false,()=>{assert.equal(m.isComposing,true);cleared++;}),false);
  assert.equal(cleared,1);assert.equal(m.isComposing,false);
  const committed=m.input(false);assert.equal(committed.state,S.COMMIT);assert.equal(committed.closedPopup,true);
  const next=m.input(false);assert.equal(next.state,S.NONE);assert.equal(next.closedPopup,false);
  assert.equal(live.state,S.COMPOSING,'a prior input snapshot never mutates');
});

test('empty spurious composition reopens only if it closed a popup and had no input data',()=>{
  for(const closed of [false,true]) for(const hadText of [false,true]) for(const keep of [false,true]) {
    const m=new M();m.begin();m.setPopupClosed(closed);m.input(hadText);
    const reopen=closed && !hadText && !keep;
    assert.equal(m.end(false,keep,()=>{}),reopen);
    assert.equal(m.input(false).state,reopen?S.NONE:S.CANCELED);
  }
});

test('query and autofill gates preserve commit, cancel and keep-panel-open policy',()=>{
  for(const state of [S.NONE,S.COMPOSING,S.COMMIT,S.CANCELED]) for(const closedPopup of [false,true]) {
    const snapshot={state,closedPopup};
    assert.equal(M.allowsQuery(snapshot,true),true);
    assert.equal(M.allowsQuery(snapshot,false),state!==S.COMPOSING && (state!==S.CANCELED || closedPopup));
    assert.equal(M.allowsAutofill(snapshot,true),state!==S.COMPOSING);
    assert.equal(M.allowsAutofill(snapshot,false),true);
  }
});

test('a new composition forgets text from the preceding commit',()=>{
  const m=new M();m.begin();m.input(true);m.end(true,false,()=>{});m.input(false);
  m.begin();m.setPopupClosed(true);assert.equal(m.end(false,false,()=>{}),true);
});

test('invalid transitions reject before side effects and clear failures retain composing state',()=>{
  const m=new M();let cleared=0;
  assert.throws(()=>m.end(true,false,()=>cleared++),/non existing/);assert.equal(cleared,0);
  m.begin();assert.throws(()=>m.begin(),/nested/);
  assert.throws(()=>m.end(true,false,()=>{throw Error('clear failed');}),/clear failed/);
  assert.equal(m.isComposing,true);m.end(true,true,()=>{throw Error('should not clear');});
  assert.equal(m.isComposing,false);
});

test('both actual input bindings retain search-mode confirmation and popup ordering',()=>{
  let keep=false;c.UrlbarCompositionModel=M;c.UrlbarPrefs={get:()=>keep};
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) for(const empty of [false,true]) {
    const source=readFileSync(new URL(name,root),'utf8');
    const method=name=>{const a=source.indexOf('  '+name+'(');return source.slice(a,source.indexOf('\n  }\n',a)+4);};
    vm.runInContext('this.Binding=class {#composition=new UrlbarCompositionModel();get composing(){return this.#composition.isComposing;}'+
      method('_on_compositionstart')+method('_on_compositionend')+'}',c);
    const b=new c.Binding(),trace=[];Object.assign(b,{value:empty?'':'text',userTypedValue:'old',searchMode:{},
      confirmSearchMode(){assert.equal(this.composing,true);trace.push('confirm');},
      view:{isOpen:true,close(){trace.push('close');},clearSelection(){assert.equal(b.composing,true);trace.push('clear');}},
      startQuery(options){assert.equal(b.composing,false);assert.equal(options.resetSearchState,false);trace.push('query');}});
    keep=false;b._on_compositionstart();assert.deepEqual(trace,['confirm','close']);
    assert.equal(b.userTypedValue,empty?null:'old');b._on_compositionend({data:''});
    assert.deepEqual(trace,['confirm','close','clear','query']);assert.equal(b._resultForCurrentValue,null);
    trace.length=0;keep=true;b._on_compositionstart();b._on_compositionend({data:'done'});assert.deepEqual(trace,[]);
  }
});


test('composition start policy covers open, mode, empty and keep-open facts without mutation',()=>{
  for(const keepPanelOpen of [false,true]) for(const viewIsOpen of [false,true])
    for(const hasSearchMode of [false,true]) for(const value of ['', ' ', 'text']) {
      const facts={keepPanelOpen,viewIsOpen,hasSearchMode,value},policy=M.startPolicy(facts);
      assert.equal(policy.closePopup,!keepPanelOpen && viewIsOpen);
      assert.equal(policy.confirmSearchMode,!keepPanelOpen && viewIsOpen && hasSearchMode);
      assert.equal(policy.clearUserTypedValue,policy.confirmSearchMode && value==='');
      assert.equal(Object.isFrozen(policy),true);assert.equal(Object.keys(facts).length,4);
    }
});

test('composition start policy rejects malformed observations',()=>{
  const valid={keepPanelOpen:false,viewIsOpen:true,hasSearchMode:true,value:''};
  for(const key of Object.keys(valid))assert.throws(()=>M.startPolicy({...valid,[key]:null}),/Invalid composition start/);
});

test('both retained start bindings preserve lazy mode/value reads when popup or mode is absent',()=>{
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) for(const open of [false,true]) for(const mode of [false,true]) {
    const source=readFileSync(new URL(name,root),'utf8'),start=source.indexOf('  _on_compositionstart(');
    const method=source.slice(start,source.indexOf('\n  }\n',start)+4);
    let keep=false;c.UrlbarCompositionModel=M;c.UrlbarPrefs={get:()=>keep};
    vm.runInContext('this.Binding=class {#composition=new UrlbarCompositionModel();'+method+'}',c);
    const b=new c.Binding(),reads=[];
    Object.defineProperty(b,'view',{get(){reads.push('view');if(keep)throw Error('unneeded view');return {isOpen:open,close(){reads.push('close');}};}});
    Object.defineProperty(b,'searchMode',{get(){reads.push('mode');if(!open)throw Error('unneeded mode');return mode?{}:null;}});
    Object.defineProperty(b,'value',{get(){reads.push('value');if(!open || !mode)throw Error('unneeded value');return 'text';}});
    b.confirmSearchMode=()=>reads.push('confirm');b._on_compositionstart();
    assert.equal(reads.includes('mode'),open);assert.equal(reads.includes('value'),open && mode);
    assert.equal(reads.includes('close'),open);
    const other=new c.Binding();Object.defineProperty(other,'view',{get(){throw Error('keep-open view read');}});
    keep=true;other._on_compositionstart();
  }
});

test('retained confirmation failure prevents popup closure after the empty typed reset',()=>{
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) {
    const source=readFileSync(new URL(name,root),'utf8'),start=source.indexOf('  _on_compositionstart(');
    c.UrlbarPrefs={get:()=>false};c.UrlbarCompositionModel=M;
    vm.runInContext('this.Binding=class {#composition=new UrlbarCompositionModel();'+source.slice(start,source.indexOf('\n  }\n',start)+4)+'}',c);
    const b=new c.Binding();Object.assign(b,{value:'',userTypedValue:'old',searchMode:{},view:{isOpen:true,close(){assert.fail('closed after failure');}},
      confirmSearchMode(){assert.equal(this.userTypedValue,null);throw Error('confirmation failed');}});
    assert.throws(()=>b._on_compositionstart(),/confirmation failed/);
  }
});


test('end policy matches the transition model for every data/history/popup/preference combination',()=>{
  for(const hasData of [false,true]) for(const hadText of [false,true])
    for(const closedPopup of [false,true]) for(const keepPanelOpen of [false,true]) {
      const policy=M.endPolicy({hasData,hadText,closedPopup,keepPanelOpen});
      const m=new M();m.begin();m.setPopupClosed(closedPopup);m.input(hadText);let clears=0;
      assert.equal(m.end(hasData,keepPanelOpen,()=>clears++),policy.reopen);
      assert.equal(clears,policy.clearSelection?1:0);assert.equal(policy.clearResultForCurrentValue,!keepPanelOpen);
      assert.equal(policy.resetSearchState,false);assert.equal(Object.isFrozen(policy),true);
      assert.equal(Object.isFrozen(policy.nextInput),true);
      assert.deepEqual({...m.input(false)},{...policy.nextInput});
    }
});

test('malformed end facts fail before clearing selection or ending composition',()=>{
  const facts={hasData:false,hadText:false,closedPopup:true,keepPanelOpen:false};
  for(const key of Object.keys(facts))assert.throws(()=>M.endPolicy({...facts,[key]:null}),/Invalid composition end/);
  const m=new M();m.begin();let clears=0;
  assert.throws(()=>m.end(null,false,()=>clears++),/Invalid composition end/);
  assert.equal(m.isComposing,true);assert.equal(clears,0);
});


test('end transition observes composition history changed by selection-clear reentry',()=>{
  for(const change of ['input','popup']) {
    const m=new M();m.begin();m.setPopupClosed(true);
    assert.equal(m.end(false,false,()=>{
      assert.equal(m.isComposing,true);
      if(change==='input')m.input(true);else m.setPopupClosed(false);
    }),false);
    const next=m.input(false);assert.equal(next.state,S.CANCELED);
    assert.equal(next.closedPopup,change==='input');
  }
});
