import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {compileNestedText, proceduralTarget} from "../upstream/firefox/browser/components/fennadblock/FennAdblockProcedural.sys.mjs";
const policy=JSON.parse(readFileSync(new URL("../configs/adblock/capabilities.json",import.meta.url)));
const win={CSS:{supports:s=>!s.includes(":has-text(")}};
test("one terminal nested text predicate compiles",()=>{
 for(const selector of ['article:has(span:has-text(Sponsored))','article:has(> span.badge:has-text(/^Ad$/i))','[data-label=":has(fake)"]:has([title="a b"]:has-text(Ad))'])assert.ok(compileNestedText(selector,policy,win),selector);
});
test("lists, siblings, deeper nesting and trailing operators fail closed",()=>{
 for(const selector of ['a,b:has(span:has-text(Ad))','article:has(+ span:has-text(Ad))','article:has(div span:has-text(Ad))','article:has(div:has(span:has-text(Ad)))','article:has(span:has-text(Ad)):upward(1)','article:not(:has(span:has-text(Ad)))','article:has(:scope:has-text(Ad))','article:has(span:has-text(/(a+)+/))'])assert.equal(compileNestedText(selector,policy,win),null,selector);
});
function tree(labels){
 const root={};const nodes=labels.map(text=>({text,parentElement:root,matches:s=>s==='span'}));
 const doc={createTreeWalker:(n,kind)=>{let i=0;return {nextNode:()=>kind===1?(nodes[i++]||null):i++===0?{length:n.text.length,substringData:(_,len)=>n.text.slice(0,len)}:null};}};
 root.ownerDocument=doc;for(const n of nodes)n.ownerDocument=doc;return {root,nodes};
}
test("nested matching preserves outer target and bounds traversal",()=>{
 const op=compileNestedText('article:has(span:has-text(Ad))',policy,win).operators;
 const {root}=tree(['Content','Ad']);assert.equal(proceduralTarget(root,op,policy,100).target,root);
 const big=tree([...Array(256).fill('Content'),'Ad']).root;
 const r=proceduralTarget(big,op,policy,100000);assert.equal(r.target,null);assert.equal(r.truncated,true);
 assert.equal(proceduralTarget(root,op,policy,1).exhausted,true);
});
test("direct-child predicates exclude grandchildren",()=>{
 const {root,nodes}=tree(['Ad']);nodes[0].parentElement={};
 const op=compileNestedText('article:has(> span:has-text(Ad))',policy,win).operators;
 assert.equal(proceduralTarget(root,op,policy,100).target,null);
});
