import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),context);
function fixture() {
 const session=Object.create(context.Session.prototype), commands=[], queries=[];
 const field={value:'alpha beta',setSelectionRange(start,end){this.selectionStart=start;this.selectionEnd=end;}};
 field.controllers={getControllerForCommand:()=>({isCommandEnabled:()=>true,doCommand(command){
  commands.push(command);
  if(command==='cmd_cut'){field.value=field.value.slice(0,field.selectionStart)+field.value.slice(field.selectionEnd);field.selectionEnd=field.selectionStart;}
 }})};
 Object.assign(session,{sessionId:'s',editId:3,input:{inputField:field},window:{InputEvent:class{constructor(type,init){Object.assign(this,init);}}},
 changed(){},query(text,event){queries.push({text,event});return Promise.resolve();}});
 const request={version:1,session:'s',editId:3,text:'alpha beta',selectionStart:0,selectionEnd:5,composing:false,clipboard:'cmd_copy'};
 return {session,field,request,commands,queries};
}
test('copy uses the original address controller without changing text or querying',()=>{
 const f=fixture();assert.equal(f.session.clipboard(f.request),1);assert.deepEqual(f.commands,['cmd_copy']);
 assert.equal(f.field.value,'alpha beta');assert.equal(f.queries.length,0);
});
test('cut publishes caret scoped to the accepted edit',()=>{
 const f=fixture();assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_cut'}),1);
 assert.equal(f.field.value,' beta');assert.equal(f.session.clipboardResult.editId,3);
 assert.equal(f.session.clipboardResult.start,0);assert.equal(f.session.clipboardResult.end,0);
 assert.equal(f.queries[0].event.inputType,'deleteByCut');
});
test('stale sessions, edits, malformed ranges and arbitrary commands are refused',()=>{
 for(const patch of [{session:'old'},{editId:2},{selectionStart:-1},{selectionEnd:999},{text:'different'},
 {clipboard:'cmd_delete'},{composing:true}]){
  const f=fixture();assert.equal(f.session.clipboard({...f.request,...patch}),2);assert.equal(f.commands.length,0);
 }
});
test('disabled editor command does not run',()=>{
 const f=fixture();f.field.controllers.getControllerForCommand=()=>({isCommandEnabled:()=>false});
 assert.equal(f.session.clipboard(f.request),2);assert.equal(f.commands.length,0);
});

test('replacing selected text with identical clipboard text still publishes collapsed caret',()=>{
 const f=fixture();f.field.controllers.getControllerForCommand=()=>({isCommandEnabled:()=>true,
 doCommand(){f.field.setSelectionRange(5,5);}});
 assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_paste'}),1);
 assert.equal(f.session.clipboardResult.start,5);assert.equal(f.session.clipboardResult.end,5);
 assert.equal(f.queries[0].event.inputType,'insertFromPaste');
});
test('latest carried edit is accepted before invoking clipboard controller',()=>{
 const f=fixture();f.session.acceptEdit=request=>{f.session.editId=request.editId;f.field.value=request.text;return {status:'accepted'};};
 assert.equal(f.session.clipboard({...f.request,editId:4,text:'new typing'}),1);
 assert.equal(f.commands.length,1);assert.equal(f.session.editId,4);
});

test('presentation release detaches before cancellation can close the session',()=>{
  const s=Object.create(context.Session.prototype);
  const view={nativePresentation:s,visibleResults:['old']};
  s.input={view};s.nativeOpen=true;s.nativeSelected='old';s.nativeVisible=['old'];
  s.cancel=()=>{assert.equal(view.nativePresentation,null);s.input=null;};
  assert.doesNotThrow(()=>s.releaseNativePresentation());
  assert.equal(view.visibleResults.length,0);
  assert.equal(s.nativeOpen,false);
  assert.doesNotThrow(()=>s.releaseNativePresentation());
});
