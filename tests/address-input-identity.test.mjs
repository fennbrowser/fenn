import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import {UrlbarInputIdentity as Identity} from '../upstream/firefox/browser/components/urlbar/content/UrlbarInputIdentity.mjs';

const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/UrlbarInputBase.mjs',import.meta.url),'utf8');

test('typed bookkeeping preserves exact text and clears result, trim and selection associations',()=>{
  for(const value of ['', '  words  ', 'https://www.example.test/', '漢字']) {
    const typed=Identity.typedValue(value);
    assert.equal(typed.userTypedValue,value);assert.equal(typed.untrimmedValue,value);
    assert.equal(typed.valueIsTyped,true);assert.equal(typed.protocolIsTrimmed,false);assert.equal(typed.wwwIsTrimmed,false);
    assert.equal(typed.resultForCurrentValue,null);assert.equal(typed.userSelectionBehavior,'none');assert.equal(Object.isFrozen(typed),true);
  }
  assert.throws(()=>Identity.typedValue(null),/Invalid/);
});

test('page proxy uses exact display comparison and never upgrades an invalid proxy',()=>{
  for(const [value,lastValidURL,changed] of [['example.test','example.test',false],
    ['https://example.test/','example.test',true],['Example.test','example.test',true],
    ['words ','words',true],['','',false],['',undefined,true]]) {
    const d=Identity.describe(value,{pageProxyValid:true,lastValidURL});
    assert.equal(d.invalidatePageProxy,changed);assert.equal(d.pageProxyValid,!changed);
    assert.equal(d.userTyping,!!value);assert.equal(d.actionType,null);
    const invalid=Identity.describe(value,{pageProxyValid:false,lastValidURL});
    assert.equal(invalid.pageProxyValid,false);assert.equal(invalid.invalidatePageProxy,false);
  }
  assert.throws(()=>Identity.describe('text',{}),/Invalid page proxy/);
});

test('retained typed-value binding applies shared fields before the browser setter and selection reset',()=>{
  const start=source.indexOf('  #updateTypedValue(value) {');
  const method=source.slice(start,source.indexOf('  #updateTypedIdentity(',start)).replace('#updateTypedValue','update');
  const Input=new Function('UrlbarInputIdentity',`return class {${method}}`)(Identity);
  const input=new Input(),events=[];
  input.controller={set userSelectionBehavior(v){events.push(['selection',v]);}};
  Object.defineProperty(input,'userTypedValue',{set(v){events.push(['typed',v,input.valueIsTyped,input._untrimmedValue,input._protocolIsTrimmed,input._wwwIsTrimmed,input._resultForCurrentValue]);}});
  input.update(' exact ');
  assert.deepEqual(events,[['typed',' exact ',true,' exact ',false,false,null],['selection','none']]);
});

test('retained identity uses live display after attributes and avoids reading invalid proxy text',()=>{
  const start=source.indexOf('  #updateTypedIdentity(value) {');
  const method=source.slice(start,source.indexOf('  _on_input(',start)).replace('#updateTypedIdentity','update');
  const Input=new Function('UrlbarInputIdentity',`return class {#isAddressbar=false;${method}}`)(Identity);
  const input=new Input(),events=[];let valid=true,reads=0;
  input.toggleAttribute=(name,value)=>events.push([name,value]);input.removeAttribute=name=>events.push(name);
  input.getAttribute=()=>valid?'valid':'invalid';input._lastValidURLStr='shown';
  Object.defineProperty(input,'value',{get(){reads++;if(!valid)throw Error('unneeded read');return 'shown';}});
  input.setPageProxyState=(...args)=>events.push(args);
  input.update('different argument');assert.equal(reads,1);assert.equal(events.length,2);
  input._lastValidURLStr='previous';input.update('');assert.deepEqual(events.at(-1),['invalid',true]);
  valid=false;input.update('text');assert.equal(reads,2);
});

test('independent identity descriptor is immutable and contains no caller-owned object',()=>{
  const facts={pageProxyValid:true,lastValidURL:'before'},d=Identity.describe('after',facts);
  facts.lastValidURL='after';assert.equal(d.invalidatePageProxy,true);assert.equal(Object.isFrozen(d),true);
  assert.ok(Object.values(d).every(value=>value===null || typeof value!=='object'));
});
