import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {planUrlbarCut} from '../upstream/firefox/browser/components/urlbar/content/UrlbarClipboardText.mjs';
const context=vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIAddressSession.sys.mjs',import.meta.url),'utf8')
 .replace(/^import .*;$/m,'').replace('export class WinUIAddressSession','this.Session = class WinUIAddressSession'),context);
function fixture() {
 const session=Object.create(context.Session.prototype), commands=[], queries=[], copies=[];
 context.Cc={'@mozilla.org/widget/clipboardhelper;1':{getService:()=>({copyString:value=>copies.push(value)})}};
 context.Ci={nsIClipboardHelper:{}};
 const field={value:'alpha beta',setSelectionRange(start,end){this.selectionStart=start;this.selectionEnd=end;}};
 field.controllers={getControllerForCommand:()=>({isCommandEnabled:()=>true,doCommand(command){
  commands.push(command);
  if(command==='cmd_cut'){field.value=field.value.slice(0,field.selectionStart)+field.value.slice(field.selectionEnd);field.selectionEnd=field.selectionStart;}
 }})};
 Object.assign(session,{sessionId:'s',editId:3,input:{inputField:field,
 getCutEdit(selection){return planUrlbarCut({...selection,readOnly:this.readOnly});},
 _getSelectedValueForClipboard:({text,start,end})=>text.slice(start,end)},window:{InputEvent:class{constructor(type,init){Object.assign(this,init);}}},
 changed(){},query(text,event,edit){queries.push({text,event,edit});field.value=text;
 if(edit)field.setSelectionRange(edit.selectionStart,edit.selectionEnd);return Promise.resolve();}});
 const request={version:1,session:'s',editId:3,text:'alpha beta',selectionStart:0,selectionEnd:5,composing:false,clipboard:'cmd_copy'};
 return {session,field,request,commands,queries,copies};
}
test('copy formats the native selection without a DOM command controller or query',()=>{
 const f=fixture();f.field.controllers={getControllerForCommand(){throw new Error('DOM controller forbidden');}};
 f.session.input._getSelectedValueForClipboard=selection=>{
  assert.equal(selection.text,'alpha beta');assert.equal(selection.start,0);assert.equal(selection.end,5);
  return 'Firefox formatted selection';
 };
 assert.equal(f.session.clipboard(f.request),1);assert.deepEqual(f.commands,[]);
 assert.deepEqual(f.copies,['Firefox formatted selection']);
 assert.equal(f.field.value,'alpha beta');assert.equal(f.queries.length,0);
});
test('cut bypasses the DOM controller and publishes one replacement scoped to the accepted edit',()=>{
 const f=fixture();f.field.controllers={getControllerForCommand(){throw new Error('DOM controller forbidden');}};
 assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_cut'}),1);
 assert.equal(f.field.value,' beta');assert.equal(f.session.clipboardResult.editId,3);
 assert.equal(f.session.clipboardResult.start,0);assert.equal(f.session.clipboardResult.end,0);
 assert.equal(f.queries[0].event.inputType,'deleteByCut');
 assert.equal(f.queries.length,1);assert.equal(f.queries[0].edit.selectionStart,0);
 assert.deepEqual(f.copies,['alpha']);assert.equal(f.session.clipboardResult.replacement,'');
 assert.equal(f.session.clipboardResult.before,'alpha beta');
});

test('read-only and empty native cuts do not mutate text, clipboard or query',()=>{
 for(const readOnly of [true,false]) {
  const f=fixture();f.session.input.readOnly=readOnly;
  assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_cut',selectionEnd:readOnly?5:0}),2);
  assert.equal(f.field.value,'alpha beta');assert.deepEqual(f.copies,[]);assert.deepEqual(f.queries,[]);
 }
});

test('cut copies Firefox-formatted text but removes only the selected display range',()=>{
 const f=fixture();f.session.input._getSelectedValueForClipboard=()=> 'https://alpha/';
 assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_cut'}),1);
 assert.deepEqual(f.copies,['https://alpha/']);assert.equal(f.field.value,' beta');
});

test('clipboard write failure cannot delete the native selection',()=>{
 const f=fixture();context.Cc['@mozilla.org/widget/clipboardhelper;1'].getService=()=>({copyString(){throw Error('clipboard busy');}});
 assert.throws(()=>f.session.clipboard({...f.request,clipboard:'cmd_cut'}),/clipboard busy/);
 assert.equal(f.field.value,'alpha beta');assert.deepEqual(f.queries,[]);
});
test('stale sessions, edits, malformed ranges and arbitrary commands are refused',()=>{
 for(const patch of [{session:'old'},{editId:2},{selectionStart:-1},{selectionEnd:999},{text:'different'},
 {clipboard:'cmd_delete'},{composing:true}]){
  const f=fixture();assert.equal(f.session.clipboard({...f.request,...patch}),2);assert.equal(f.commands.length,0);
 }
});
test('disabled editor command does not run',()=>{
 const f=fixture();f.field.controllers.getControllerForCommand=()=>({isCommandEnabled:()=>false});
 assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_paste'}),2);assert.equal(f.commands.length,0);
});

test('empty native copy does not change clipboard contents',()=>{
 const f=fixture();assert.equal(f.session.clipboard({...f.request,selectionEnd:0}),2);
 assert.deepEqual(f.copies,[]);
});

test('replacing selected text with identical clipboard text still publishes collapsed caret',()=>{
 const f=fixture();f.field.controllers.getControllerForCommand=()=>({isCommandEnabled:()=>true,
 doCommand(){f.field.setSelectionRange(5,5);}});
 assert.equal(f.session.clipboard({...f.request,clipboard:'cmd_paste'}),1);
 assert.equal(f.session.clipboardResult.start,5);assert.equal(f.session.clipboardResult.end,5);
 assert.equal(f.queries[0].event.inputType,'insertFromPaste');
});
test('latest carried edit is accepted before formatting the copy selection',()=>{
 const f=fixture();f.session.acceptEdit=request=>{f.session.editId=request.editId;f.field.value=request.text;return {status:'accepted'};};
 assert.equal(f.session.clipboard({...f.request,editId:4,text:'new typing'}),1);
 assert.equal(f.commands.length,0);assert.equal(f.session.editId,4);assert.deepEqual(f.copies,['new t']);
});

test('presentation release detaches before cancellation can close the session',()=>{
  const s=Object.create(context.Session.prototype);
  const view={nativePresentation:s,visibleResults:['old']};
  const controller={visibleResults:['old']};
  s.input={view,controller};s.nativeOpen=true;s.nativeSelected='old';s.nativeVisible=['old'];
  s.cancel=()=>{assert.equal(view.nativePresentation,null);s.input=null;};
  assert.doesNotThrow(()=>s.releaseNativePresentation());
  assert.equal(controller.visibleResults.length,0);
  assert.equal(s.nativeOpen,false);
  assert.doesNotThrow(()=>s.releaseNativePresentation());
});
