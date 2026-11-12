import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {compileStyleActions} from "../upstream/firefox/browser/components/fennadblock/FennAdblockStyles.sys.mjs";
const policy = JSON.parse(readFileSync(new URL("../configs/adblock/capabilities.json", import.meta.url)));
// Deliberately permissive CSS mock: these tests isolate the pre-parser boundary.
// Real Gecko syntax, serialization and stylesheet effects are covered by the browser suite.
const win = {CSS:{supports:()=>true},document:{createElement:()=>({style:{length:0,value:"",get cssText(){return this.value;},set cssText(v){this.value=v;this.length=v?1:0;}}})}};
const action = style => JSON.stringify({selector:[{type:"css-selector",arg:".ad"}],action:{type:"style",arg:style}});
test("allowed styles preserve priority",()=>{
  const r=compileStyleActions([action("display:none!important;color:rgb(1,2,3)")],policy,win);
  assert.equal(r.counts.applied,1);assert.match(r.css,/!important/);
});
test("unsafe property, function, escape and rule injection are rejected before CSS parsing",()=>{
  for(const style of ["background-image:url(https://example.com)","color:var(--remote)","color:attr(x)","display:none;}body{color:red","color:r\\65d","color:red/*x*/","--custom:red","color:paint(x)","color:red; @import x",'color:"red"']){
    const r=compileStyleActions([action(style)],policy,win);assert.equal(r.css,"",style);assert.equal(r.counts.rejected,1,style);
  }
});
test("whole action fails on a rejected declaration",()=>{
  assert.equal(compileStyleActions([action("display:none;background-image:url(x)")],policy,win).css,"");
});
test("procedural styles are unsupported and DOM actions are delegated",()=>{
  const entries=[JSON.stringify({selector:[{type:"has-text",arg:"ad"}],action:{type:"style",arg:"display:none"}}),JSON.stringify({selector:[{type:"css-selector",arg:".ad"}],action:{type:"remove"}})];
  const r=compileStyleActions(entries,policy,win);assert.equal(r.counts.unsupported,1);assert.equal(r.css,"");
});
test("action, declaration and sheet bounds are enforced",()=>{
  assert.equal(compileStyleActions(Array(1001).fill(action("color:red")),policy,win).counts.truncated,1);
  assert.equal(compileStyleActions([action(Array(33).fill("color:red").join(";"))],policy,win).counts.rejected,1);
  assert.equal(compileStyleActions([action("color:red")],{...policy,style_max_sheet_chars:1},win).counts.truncated,1);
});
test("malformed data and unknown policy cannot create styles",()=>{
  assert.equal(compileStyleActions(["{",null],policy,win).counts.rejected,2);
  assert.equal(compileStyleActions([action("color:red")],{...policy,id:"unknown"},win).css,"");
});
