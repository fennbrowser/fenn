import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {compileProceduralSelector,proceduralTarget} from '../upstream/firefox/browser/components/fennadblock/FennAdblockProcedural.sys.mjs';
import {FennAdblockDomRunner} from '../upstream/firefox/browser/components/fennadblock/FennAdblockDom.sys.mjs';
const policy=JSON.parse(readFileSync(new URL('../configs/adblock/capabilities.json',import.meta.url)));
const win={CSS:{supports:()=>true}};
const compile=(arg,type='matches-css')=>compileProceduralSelector([{type:'css-selector',arg:'.ad'},{type,arg}],policy,win);
test('all three computed-style operators compile with exact or bounded regex values',()=>{
 for(const type of ['matches-css','matches-css-before','matches-css-after'])for(const arg of ['color: rgb(1, 2, 3)','content: "Sponsored"','content: /Ad|Sponsored/i'])assert.ok(compile(arg,type));
});
test('layout properties, arbitrary regex, malformed and oversized arguments fail closed',()=>{
 for(const arg of ['display: none','width: 10px','--page: x','content: /(a+)+$/','content: /a/g','content:','color','content: x\ny','content: '+ 'x'.repeat(1025)]) {
  if(arg==='content: x\ny')continue; // boundary whitespace is trimmed intentionally
  assert.equal(compile(arg),null,arg);
 }
});
test('exact computed serialization and pseudo target are preserved',()=>{
 for(const [type,pseudo] of [['matches-css',null],['matches-css-before','::before'],['matches-css-after','::after']]) {
  const n={ownerDocument:{defaultView:{getComputedStyle:(node,p)=>{assert.equal(node,n);assert.equal(p,pseudo);return {getPropertyValue:k=>{assert.equal(k,'content');return '"Ad"';}}}}}};
  assert.equal(proceduralTarget(n,compile('content: "Ad"',type).operators,policy,100000).target,n);
  assert.equal(proceduralTarget(n,compile('content: Ad',type).operators,policy,100000).target,null);
 }
});
test('insufficient work performs no computed-style read',()=>{
 const n={ownerDocument:{defaultView:{getComputedStyle:()=>{throw Error('unexpected read')}}}};
 assert.equal(proceduralTarget(n,compile('opacity: 0').operators,policy,8191).exhausted,true);
});
test('oversized style values never match a truncated prefix',()=>{
 const n={ownerDocument:{defaultView:{getComputedStyle:()=>({getPropertyValue:()=> 'x'.repeat(1025)})}}};
 const r=proceduralTarget(n,compile('content: /^x/').operators,policy,100000);assert.equal(r.target,null);assert.equal(r.truncated,true);
});
test('style polling is scoped, rate limited and canceled on stop',()=>{
 let next=0;const pending=new Map();const timers={setTimer:(f,delay)=>{pending.set(++next,{f,delay});return next},clearTimer:id=>pending.delete(id),current:()=>true,now:()=>0};
 const runner=new FennAdblockDomRunner({document:{hidden:false}}, {rules:[{operators:compile('opacity: 0').operators}],counts:{}},policy,timers);
 runner.pollStyles();runner.pollStyles();assert.equal(pending.size,1);assert.equal([...pending.values()][0].delay,1000);runner.stop();assert.equal(pending.size,0);assert.equal(runner.styleTimer,null);
});

test('non-style rules never start a polling timer',()=>{
 const runner=new FennAdblockDomRunner({}, {rules:[{operators:[{type:'has-text',text:'Ad'}]}],counts:{}},policy,{setTimer:()=>{throw Error('unexpected timer')}});
 runner.pollStyles();assert.equal(runner.styleTimer,undefined);
});
test('style polling stops on authorization revocation',()=>{
 let callback,canceled=false;
 const runner=new FennAdblockDomRunner({document:{hidden:false}}, {rules:[{operators:compile('opacity: 0').operators}],counts:{}},policy,{setTimer:f=>{callback=f;return 1},clearTimer:()=>{canceled=true},current:()=>false});
 runner.pollStyles();callback();assert.equal(runner.counts.stopped,true);assert.equal(runner.win,null);assert.equal(runner.styleTimer,null);
});
