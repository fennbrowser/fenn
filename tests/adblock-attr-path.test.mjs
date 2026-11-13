import test from 'node:test';import assert from 'node:assert/strict';import {readFileSync} from 'node:fs';
import {compileProceduralSelector,proceduralTarget,compileAttributeMatch} from '../upstream/firefox/browser/components/fennadblock/FennAdblockProcedural.sys.mjs';
const p=JSON.parse(readFileSync(new URL('../configs/adblock/capabilities.json',import.meta.url)));const win={CSS:{supports:()=>true}},css={type:'css-selector',arg:'.ad'};
const compile=(type,arg)=>compileProceduralSelector([css,{type,arg}],p,win);
function node(attrs={},path='/shop',search=''){const rows=Object.entries(attrs).map(([name,value])=>({name,value}));return {attributes:{length:rows.length,item:i=>rows[i]},ownerDocument:{defaultView:{location:{pathname:path,search}}}};}
const match=(n,type,arg,budget=1000000)=>proceduralTarget(n,compile(type,arg).operators,p,budget);
test('attribute exact, quoted, presence and bounded regex forms',()=>{
 const n=node({'data-ad':'Sponsored','trackid':'','onclick':'event()'});
 for(const arg of ['data-ad=Sponsored','data-ad="Sponsored"','data-ad=/^Sponsored$/','"/trackid$/"','/^on/=/event/','trackid'])assert.equal(match(n,'matches-attr',arg).target,n,arg);
 for(const arg of ['data-ad=sponsored','data-ad=Sponsor','missing'])assert.equal(match(n,'matches-attr',arg).target,null,arg);
});
test('attribute grammar rejects unbounded and malformed patterns',()=>{
 for(const arg of ['style','/^style$/','/STYLE/i','/[a-z]+/','id=/x*/','id="','id=""','id=/(a+)+$/','id="a\\"b"','data-ad=/x/g'])assert.equal(compileAttributeMatch(arg,p),null,arg);
});
test('equal signs in quoted values and regex attribute names are not separators',()=>{
 const n=node({'data-url':'a=b'});assert.equal(match(n,'matches-attr','data-url="a=b"').target,n);
 assert.ok(compileAttributeMatch('/a=b/=x',p));
});
test('attribute bounds fail without matching a truncated prefix',()=>{
 const many=node(Object.fromEntries(Array.from({length:65},(_,i)=>['a'+i,'x'])));assert.equal(match(many,'matches-attr','a0').truncated,true);
 assert.equal(match(node({id:'x'.repeat(4097)}),'matches-attr','id=/^x/').target,null);
 assert.equal(match(node({id:'x'}),'matches-attr','id',0).exhausted,true);
});
test('path literal substring, bounded regex, query and encoded Unicode',()=>{
 for(const [path,search,arg,expected] of [['/','','/',true],['/shop/item','','/shop',true],['/else','?next=/shop','/shop',true],['/shop','','/^\\/shop$/',true],['/shopper','','/^\\/shop$/',false],['/caf%C3%A9','','/caf\u00e9',true]]){
 const n=node({},path,search);assert.equal(!!match(n,'matches-path',arg).target,expected,arg);
 }
});
test('leading path gate consumes one following descendant CSS base only',()=>{
 const r=compileProceduralSelector([{type:'matches-path',arg:'/shop'},{type:'css-selector',arg:' .ad'}, {type:'has-text',arg:'Ad'}],p,win);assert.equal(r.selector,' .ad');assert.equal(r.operators[0].type,'matches-path');
 assert.equal(compileProceduralSelector([css,{type:'matches-path',arg:'/shop'},css],p,win),null);
});
test('path rejects unsupported grammar and oversized input',()=>{
 for(const arg of ['~/shop','/^a+$/','/shop/m','/shop/u','/shop/ii','"/shop"'])assert.equal(compile('matches-path',arg),null);
 assert.equal(match(node({},'/'+ 'a'.repeat(8192)),'matches-path','a').truncated,true);
 assert.equal(match(node({},'/shop'),'matches-path','/shop',1).exhausted,true);
});

test('path-only compile is bounded and adjacent selector stages remain rejected',()=>{
 assert.ok(compileProceduralSelector([{type:'matches-path',arg:'/shop'}],p,win));
 for(const arg of [':not(.ad)',' > .ad',' + .ad'])assert.equal(compileProceduralSelector([{type:'matches-path',arg:'/shop'},{type:'css-selector',arg}],p,win),null);
});

test('path polling tracks background changes, retains bounded keys and cancels',async()=>{
 const {FennAdblockDomRunner}=await import('../upstream/firefox/browser/components/fennadblock/FennAdblockDom.sys.mjs');
 let next=0;const timers=new Map(),w={document:{hidden:true},location:{pathname:'/shop',search:''}};
 const r=new FennAdblockDomRunner(w,{rules:[{operators:compile('matches-path','/shop').operators}],counts:{}},p,{setTimer:(f,d)=>{timers.set(++next,{f,d});return next},clearTimer:id=>timers.delete(id),current:()=>true});
 r.pollStyles();const id=r.styleTimer;timers.get(id).f();timers.delete(id);assert.equal(r.lastPath,'/shop');assert.notEqual(r.timer,null);
 w.location.pathname='/'+ 'x'.repeat(9000);const id2=r.styleTimer;timers.get(id2).f();timers.delete(id2);assert.equal(r.lastPath,'');r.stop();assert.equal(timers.size,0);assert.equal(r.lastPath,null);
});
test('path poll compares against the path the last pass evaluated',async()=>{
 const {FennAdblockDomRunner}=await import('../upstream/firefox/browser/components/fennadblock/FennAdblockDom.sys.mjs');
 let next=0;const timers=new Map();
 const w={document:{hidden:true,createTreeWalker:()=>({nextNode:()=>null})},location:{pathname:'/shop',search:''}};
 const r=new FennAdblockDomRunner(w,{rules:[{operators:compile('matches-path','/shop').operators}],counts:{}},p,{setTimer:(f,d)=>{timers.set(++next,{f,d});return next},clearTimer:id=>timers.delete(id),current:()=>true,now:()=>0});
 r.lastPath='/shop';w.location.pathname='/else';r.dirty=true;r.drain();assert.equal(r.lastPath,'/else');
 w.location.pathname='/shop';r.pollStyles();const id=r.styleTimer;timers.get(id).f();timers.delete(id);
 assert.equal(r.pollPending,true);r.stop();
});
