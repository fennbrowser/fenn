import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {compileProceduralSelector, proceduralTarget, compileTextRegex} from "../upstream/firefox/browser/components/fennadblock/FennAdblockProcedural.sys.mjs";
import {FennAdblockDomRunner} from "../upstream/firefox/browser/components/fennadblock/FennAdblockDom.sys.mjs";
const policy=JSON.parse(readFileSync(new URL("../configs/adblock/capabilities.json",import.meta.url)));
const win={CSS:{supports:s=>!s.includes("{")}},css={type:"css-selector",arg:".ad"};
function node(chunks=[]) {return {parentElement:null,matches:s=>s===".card",ownerDocument:{createTreeWalker:()=>{let i=0;return {nextNode:()=>i<chunks.length?{length:chunks[i].length,substringData:(_,n)=>chunks[i++].slice(0,n)}:null};}}};}
test("literal text and upward forms compile",()=>{for(const arg of ["2",".card"])assert.ok(compileProceduralSelector([css,{type:"has-text",arg:"Sponsored"},{type:"upward",arg}],policy,win));});
test("unsupported arguments and selector stages fail closed",()=>{
 for(const part of [{type:"has-text",arg:"/(a+)+$/"},{type:"has-text",arg:'"foo"'},{type:"has-text",arg:"a\\b"},{type:"upward",arg:"0"},{type:"upward",arg:"33"},{type:"xpath",arg:"//a"}])assert.equal(compileProceduralSelector([css,part],policy,win),null);
 assert.equal(compileProceduralSelector([css,{type:"has-text",arg:"ad"},css],policy,win),null);
});
test("literal matching crosses nodes and remains case sensitive",()=>{const n=node(["Spon","sored"]);assert.equal(proceduralTarget(n,[{type:"has-text",text:"Sponsored"}],policy,100).target,n);assert.equal(proceduralTarget(n,[{type:"has-text",text:"sponsored"}],policy,100).target,null);});
test("bounded text and work stop before full subtree read",()=>{const n=node(["a".repeat(20000)+"needle"]),r=proceduralTarget(n,[{type:"has-text",text:"needle"}],policy,100000);assert.equal(r.target,null);assert.equal(r.truncated,true);assert.ok(r.work<=8193);assert.equal(proceduralTarget(n,[{type:"has-text",text:"needle"}],policy,10).exhausted,true);});
test("upward starts at parent and stops at ancestry limit",()=>{const n=node(),p=node(),g=node();n.parentElement=p;p.parentElement=g;assert.equal(proceduralTarget(n,[{type:"upward",count:2}],policy,100).target,g);assert.equal(proceduralTarget(n,[{type:"upward",selector:".card"}],policy,100).target,p);assert.equal(proceduralTarget(n,[{type:"upward",count:3}],policy,100).target,null);});
test("cleanup restores owned display and preserves page edits",()=>{
 const runner=new FennAdblockDomRunner({}, {rules:[],counts:{}},policy,{clearTimer:()=>{}});
 function styled(value,priority){return {style:{getPropertyValue:()=>value,getPropertyPriority:()=>priority,setProperty:(k,v,p)=>{value=v;priority=p;},removeProperty:()=>{value="";priority="";}}};}
 const owned=styled("none","important"),edited=styled("grid","");runner.hidden.set(owned,{value:"flex",priority:""});runner.hidden.set(edited,{value:"block",priority:""});runner.stop();assert.equal(owned.style.getPropertyValue(),"flex");assert.equal(edited.style.getPropertyValue(),"grid");assert.equal(runner.hidden.size,0);assert.equal(runner.found.size,0);
});
test("admitted regex syntax agrees with native regex",()=>{
 for(const pattern of ["/Ad/","/^Ad$/","/Ad|Sponsored/i","/^(?:Ad|Sponsored)$/","/^$/","/sponsorisé/i","/watch|🎟/","/a\\.b/","/a\\|b/"]){
  const compiled=compileTextRegex(pattern,policy);assert.ok(compiled,pattern);const native=new RegExp(pattern.slice(1,pattern.lastIndexOf("/")),pattern.slice(pattern.lastIndexOf("/")+1));
  for(const text of ["","Ad","AD","Ad\n","Sponsored","news Sponsored here","SPONSORISÉ","a.b","a|b","🎟"])assert.equal(compiled.regex.test(text),native.test(text),pattern+":"+text);
 }
});
test("unbounded syntax and unknown flags fail closed",()=>{
 for(const pattern of ["/(a+)+$/","/a*/","/a{2}/","/a?/","/[ab]/","/a.b/","/(?=Ad)/","/(a)\\1/","/Ad/g","/Ad/ii","/a(b|c)/","/a\\d/","/"+"a".repeat(513)+"/","/"+Array(34).fill("a").join("|")+"/"])assert.equal(compileTextRegex(pattern,policy),null,pattern);
});
test("regex requires complete text and charges bounded matching work",()=>{
 const compiled=compileTextRegex("/^Sponsored$/i",policy),op=[{type:"has-text",...compiled}];
 assert.ok(proceduralTarget(node(["Spon","sored"]),op,policy,1000).target);
 assert.equal(proceduralTarget(node(["Sponsored","x"]),op,policy,1000).target,null);
 assert.equal(proceduralTarget(node(["a".repeat(9000)]),[{type:"has-text",...compileTextRegex("/^a$/",policy)}],policy,100000).truncated,true);
 assert.equal(proceduralTarget(node(["Sponsored"]),op,policy,10).exhausted,true);
 assert.ok(proceduralTarget(node([]),[{type:"has-text",...compileTextRegex("/^$/",policy)}],policy,100).target);
});

test("insufficient work prevents native regex evaluation",()=>{
 const compiled=compileTextRegex("/Sponsored/i",policy);
 compiled.regex.test=()=>{throw new Error("must not evaluate");};
 assert.equal(proceduralTarget(node(["Sponsored"]),[{type:"has-text",...compiled}],policy,20).exhausted,true);
});
