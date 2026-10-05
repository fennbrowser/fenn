import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import vm from "node:vm";
const context=vm.createContext({console});
vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs",import.meta.url),"utf8")
  .replace(/^import .*;$/m,"").replace("export class WinUIUrlbarSession","this.Session = class WinUIUrlbarSession"),context);
function fixture() {
  const s=Object.create(context.Session.prototype);
  const field={value:"example.test/",selectionStart:3,selectionEnd:13,
    setSelectionRange(start,end){this.selectionStart=start;this.selectionEnd=end;}};
  Object.assign(s,{sessionId:"s",editId:1,selectionId:0,_revision:2,_sequence:1,changed(){},
    input:{inputField:field,_autofillPlaceholder:{value:field.value}},
    active:{text:"exa",inputValue:"exa",nativeEdit:true,context:{id:1}}});
  return {s,field};
}
test("completion carries Firefox value and range once per edit",()=>{
  const {s,field}=fixture();s.captureAutofill();
  assert.equal(s.active.inputValue,field.value);assert.equal(s.autofillResult.start,3);
  assert.equal(s.autofillResult.before,"exa");assert.equal(s.autofillResult.editId,1);
  const serial=s.autofillResult.serial;s.captureAutofill();assert.equal(s.autofillResult.serial,serial);
  s.editId++;s.captureAutofill();assert.equal(s.autofillResult.serial,serial+1);
});
test("old queries, focus queries and collapsed selections cannot publish completion",()=>{
  const {s,field}=fixture();s.captureAutofill({nativeEdit:true});assert.equal(s.autofillResult,undefined);
  s.active.nativeEdit=false;s.captureAutofill();assert.equal(s.autofillResult,null);
  s.active.nativeEdit=true;field.selectionStart=field.selectionEnd;s.captureAutofill();assert.equal(s.autofillResult,null);
  s.onFirstResult({id:99});assert.equal(s.autofillResult,null);
});
test("caret acceptance validates displayed completion and delegates selection semantics",()=>{
  const {s,field}=fixture();s.captureAutofill();let selected=0;s.input._on_selectionchange=()=>selected++;
  s.editState={editId:1,revision:2,text:"exa"};
  const request={version:1,session:"s",revision:2,editId:1,selectionId:1,start:13,end:13,direction:"none",text:field.value};
  assert.equal(s.updateSelection({...request,text:"old"}).status,"stale");
  assert.equal(s.updateSelection(request).status,"selected");assert.equal(selected,1);
  assert.equal(s.autofillResult,null);assert.equal(field.selectionStart,13);
});
test("native edits run Firefox input handling with selection set before the event",async()=>{
  for (const inputType of ["insertText","deleteContentBackward","deleteWordBackward","deleteWordForward","historyUndo"]) {
    const {s,field}=fixture();const calls=[];
    s.window={gBrowser:{selectedBrowser:{currentURI:{spec:"about:blank"}}}};
    s.cancel=()=>{s.active=null;};s.snapshot=()=>({});
    s.input.view={nativePresentation:s};s.input._autofillPlaceholder=null;
    s.input._on_input=e=>calls.push([e.inputType,field.value,field.selectionStart,field.selectionEnd]);
    await s.query("exa",{inputType},{inputType,selectionStart:3,selectionEnd:3});
    assert.deepEqual(calls,[[inputType,"exa",3,3]]);
  }
});

test("Firefox input handling that starts no query settles the edit",async()=>{
  const {s}=fixture();s.window={gBrowser:{selectedBrowser:{currentURI:{spec:"about:blank"}}}};
  s.cancel=()=>{s.active=null;};s.snapshot=()=>null;
  s.input.view={nativePresentation:s};s.input._on_input=()=>{};
  await s.query("",{inputType:"deleteContentBackward"},{inputType:"deleteContentBackward",selectionStart:0,selectionEnd:0});
  assert.equal(s.active.done,true);
});
