import assert from 'node:assert/strict';
import test from 'node:test';
import {inspect,differences} from '../scripts/Check-AddressModelDependencies.mjs';

test('inventory records private/optional paired calls, provider element parameters and aliases',()=>{
  const data=inspect(`class Consumer { #child; async picked(result, details = {}) {
    this.#child?.input.startQuery({searchString: result.payload.query});
    const {element: selected} = details;
    details.element?.closest('row');
  } }`);
  assert.deepEqual(data.methods[0],{name:'Consumer.picked',kind:'method',async:true,parameters:['result','details = {}']});
  const accesses=data.references.map(r=>r.access);
  assert.ok(accesses.includes('this.#child.input.startQuery'));
  assert.ok(accesses.includes('details.element.closest'));
  assert.ok(accesses.some(a=>a.includes('element: selected')));
});

test('inventory reads the actor allowlist without executing production code',()=>{
  assert.deepEqual(inspect(`const shared={INVOKABLE_CONTENT_ACTIONS:Object.freeze({
    input:['search','startQuery'],view:['close'],controller:['notifyFromWire']})};`).contentActions,
    {input:['search','startQuery'],view:['close'],controller:['notifyFromWire']});
});

test('changed, added, removed and non-interface source edits require review',()=>{
  const original={schema:1,firefoxCommit:'pin',sources:{a:{sha256:'old',methods:[]}}};
  assert.deepEqual(differences(original,original),[]);
  for(const sources of [{},{a:{sha256:'new',methods:[]}},{...original.sources,b:{sha256:'new'}}]) {
    assert.ok(differences({...original,sources},original).length);
  }
  assert.ok(differences({...original,firefoxCommit:'new'},original).length);
});
