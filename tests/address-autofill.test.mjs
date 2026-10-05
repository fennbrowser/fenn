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


test("Firefox alias transition revokes old rows and retains the transformed edit",()=>{
  const {s,field}=fixture();
  const browser={currentURI:{spec:"about:blank"}};
  s.window={gBrowser:{selectedBrowser:browser}};
  Object.assign(s.active,{browser,page:"about:blank",rows:[{id:"old"}],results:[{}],resultIndexes:[0],done:true});
  s.input.view={nativePresentation:s,visibleResults:[{}]};
  s.nativeVisible=["old"];s.nativeSelected={};s.cancelImages=()=>{};
  field.value="";
  const active=s.active;
  s.onSearchModeFromResult({id:99});assert.equal(active.rows.length,1);
  s.onSearchModeFromResult({id:1,results:[{payload:{engine:"test"}}]});
  assert.equal(s.active,active);assert.equal(active.inputValue,"");
  assert.equal(active.done,false);assert.equal(active.rows.length,0);
  assert.equal(s.nativeVisible.length,0);assert.equal(s.nativeSelected,null);
  s.onQueryCancelled({id:1});assert.equal(s.active,active);
  s.onQueryStarted({id:2,searchString:""});assert.equal(active.context.id,2);
  s.onQueryCancelled({id:2});assert.equal(s.active,null);
});

test("alias transition cannot transfer another document's query",()=>{
  const {s,field}=fixture();
  const browser={currentURI:{spec:"about:newtab"}};
  s.window={gBrowser:{selectedBrowser:browser}};
  s.active.browser=browser;s.active.page="about:blank";
  s.input.view={nativePresentation:s};field.value="";
  s.onSearchModeFromResult({id:1});
  assert.equal(s.active.context.id,1);assert.equal(s.active.inputValue,"exa");
});


test("pending alias edits rebase only against the presentation that still showed the alias",()=>{
  for (const [epoch,page,expected] of [[0,"about:blank","parity"],[1,"about:blank","@test parity"],[0,"about:newtab","@test parity"]]) {
    const {s}=fixture();
    const browser={currentURI:{spec:page}};
    s.window={gBrowser:{selectedBrowser:browser},InputEvent:class {}};
    s.input.searchMode={engineName:"test"};
    s.nativeTextRewrite={epoch:1,prefix:"@test ",engine:"test",browser,page:"about:blank"};
    let text;s.query=value=>{text=value;return Promise.resolve(null);};
    const result=s.acceptEdit({version:1,session:"s",revision:2,editId:2,textEpoch:epoch,
      text:"@test parity",selectionStart:12,selectionEnd:12,composing:false,inputType:"insertText"});
    assert.equal(result.status,"accepted");assert.equal(text,expected);
    assert.equal(s.editState.selectionStart,expected.length);
  }
});


test("composition updates never query; committed Unicode starts one Firefox query",()=>{
  const {s,field}=fixture();
  s.window={InputEvent:class {constructor(type,init){Object.assign(this,init);}}};
  s.input.view={nativePresentation:s};s.input.setNativeInputValue=value=>{field.value=value;};
  s.cancel=()=>{s.active=null;s.editState=null;};
  const queries=[];s.query=(text,event)=>{queries.push([text,event.inputType]);return Promise.resolve(null);};
  const request={version:1,session:"s",revision:2,editId:2,text:"\u306b",selectionStart:1,selectionEnd:1,composing:true,inputType:"insertCompositionText"};
  assert.equal(s.acceptEdit(request).status,"accepted");
  assert.equal(field.value,"\u306b");assert.equal(queries.length,0);
  s.acceptEdit({...request,editId:3,text:"\u65e5\u672c",selectionStart:2,selectionEnd:2});
  assert.equal(queries.length,0);
  s.acceptEdit({...request,editId:4,text:"\u65e5\u672c",selectionStart:2,selectionEnd:2,composing:false});
  assert.deepEqual(queries,[["\u65e5\u672c","insertCompositionText"]]);
});
