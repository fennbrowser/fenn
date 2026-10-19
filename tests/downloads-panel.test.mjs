import {readFileSync} from "node:fs";
import vm from "node:vm";
import test from "node:test";
import assert from "node:assert/strict";

function fixture(downloads=[]) {
  const calls=[],views=new Set(),kinds=[],files=new Set(["C:/test.txt"]);
  const list={async addView(view){views.add(view);for(const d of downloads)view.onDownloadAdded(d);},removeView:view=>views.delete(view),async getAll(){return [...downloads];}};
  const prototype={
    _updateStateInner(){this._updateState();},
    _updateState(){this.showDisplayNameAndIcon(this.download.name);this.showStatus(this.download.status);this.showProgress(this.download.hasProgress?"normal":"undetermined",this.download.progress,this.download.canceled);this.showButton(this.download.button || "cancel");},
    isCommandEnabled(command){return this.download.enabled.has(command);},
    doCommand(command){if(typeof this[command]==="function")return this[command]();calls.push([this.download.name,command]);},
    confirmUnblock(window,kind){calls.push([this.download.name,kind,window]);},
    get currentDefaultCommandName(){return this.download.default || "";},
  };
  const window={clearTimeout(){},gDialogBox:{isOpen:false},private:false,
    document:{createXULElement:()=>({})},BrowserCommands:{downloadsUI:()=>calls.push("all")},
    Localization:class {formatMessagesSync(messages){return messages.map(m=>({value:m.id,attributes:["label","value","aria-label"].map(name=>({name,value:m.id}))}));}}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,
    owner:{addressSession:{sessionId:"address"},addressTab:{},addressDocument:1,invalidateAddress(){}},
    customization:{nativeOverflow:new Set(),editorOpen:false},
    navigationState:()=>({downloads:{shown:true}}),menuAnchor:()=>({})};
  const context=vm.createContext({console,Services:{obs:{addObserver(){},removeObserver(){}}},ChromeUtils:{importESModule:path=>path.includes("DownloadIntegration")?{DownloadIntegration:{}}:path.includes("FileUtils")?{FileUtils:{File:class {constructor(path){this.path=path;}exists(){return files.has(this.path);}}}}:path.includes("PrivateBrowsingUtils")?
    {PrivateBrowsingUtils:{isWindowPrivate:w=>w.private}}:path.includes("DownloadsViewUI")?
    {DownloadsViewUI:{DownloadElementShell:{prototype}}}:path.includes("DownloadsCommon")?{DownloadsCommon:{stateOfDownload:d=>d.status,copyDownloadLink:d=>calls.push(["copy",d.name])}}:{Downloads:{PUBLIC:"public",PRIVATE:"private",getList:async kind=>{kinds.push(kind);return list;}}}}});
  vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIDownloadsPanel.sys.mjs",import.meta.url),"utf8").replace("export class WinUIDownloadsPanel","this.Panel=class WinUIDownloadsPanel"),context);
  const panel=new context.Panel(toolbar);
  const execute=(session,revision,id)=>panel.execute({command:`downloads-panel:invoke:${session}:${revision}:${id}`});
  return {panel,toolbar,window,list,views,calls,kinds,downloads,execute,files};
}
const download=(name,properties={})=>({name,status:"Downloading",startTime:1,stopped:false,hasPartialData:true,progress:25,hasProgress:true,
  target:{path:"C:/test.txt",exists:true},enabled:new Set(["downloadsCmd_cancel","downloadsCmd_pauseResume"]),refresh:()=>Promise.resolve(),...properties});

test("downloads connect only on open, isolate private data and detach on close",async()=>{
  const f=fixture();assert.equal(f.panel.snapshot().enabled,true);assert.deepEqual(f.kinds,[]);
  f.window.private=true;await f.panel.open(1);assert.deepEqual(f.kinds,["private"]);assert.equal(f.views.size,1);
  const state=f.panel.snapshot();assert.equal(state.items.length,0);assert.ok(state.title);assert.ok(state.all);
  f.panel.close();assert.equal(f.panel.fingerprint,"");assert.equal(f.panel.context,null);assert.equal(f.views.size,0);assert.equal(f.panel.snapshot().closed,1);
});
test("the panel bounds its recent list, retains row identity and rejects stale descriptions",async()=>{
  const f=fixture(Array.from({length:30},(_,i)=>download(`file-${i}`,{startTime:i})));
  await f.panel.open(1);const state=f.panel.snapshot();assert.equal(state.items.length,8);assert.equal(state.items[0].name,"file-29");
  assert.equal(state.items[0].main,0,"a running filename must never cancel its download");
  const row=state.items[0],d=f.downloads[29];d.progress=50;
  for(const v of f.views)v.onDownloadChanged(d);
  const updated=f.panel.snapshot();assert.equal(updated.items[0].id,row.id);assert.ok(updated.revision>state.revision);
  assert.equal(updated.actionRevision,state.actionRevision,"progress cannot revoke a queued command");
  assert.equal(f.execute(1,state.actionRevision,row.primary),1);
  assert.equal(f.execute(1,updated.actionRevision,row.primary),2,"duplicate action is consumed");
  f.panel.snapshot();assert.equal(f.execute(1,updated.actionRevision,row.primary),2,"an unchanged publication does not renew the action");
  assert.deepEqual(f.calls,[["file-29","downloadsCmd_cancel"]]);
});
test("live eligibility, removal, modal state and document changes revoke commands",async()=>{
  const d=download("file");const f=fixture([d]);await f.panel.open(1);let state=f.panel.snapshot();
  d.enabled.clear();assert.equal(f.execute(1,state.actionRevision,state.items[0].primary),2);assert.deepEqual(f.calls,[]);
  f.toolbar.owner.addressDocument=2;assert.equal(f.execute(1,state.actionRevision,255),2);
  assert.equal(f.panel.snapshot().closed,1);assert.equal(f.views.size,0);
  f.toolbar.owner.addressDocument=1;await f.panel.open(2);state=f.panel.snapshot();
  f.window.gDialogBox.isOpen=true;assert.equal(f.execute(2,state.actionRevision,255),2);f.panel.snapshot();assert.equal(f.views.size,0);
});
test("blocked files use Firefox confirmation and never expose a stale open action",async()=>{
  const d=download("blocked",{error:{becauseBlockedByReputationCheck:true},hasBlockedData:true,stopped:true,
    button:"subviewOpenOrRemoveFile",default:"downloadsCmd_showBlockedInfo",enabled:new Set(["downloadsCmd_chooseUnblock","downloadsCmd_confirmBlock","downloadsCmd_open"])});
  const f=fixture([d]);await f.panel.open(1);const state=f.panel.snapshot(),row=state.items[0];
  assert.equal(row.warning,true);assert.equal(row.commands.some(c=>c.icon==="file"),false);
  assert.equal(f.execute(1,state.actionRevision,row.primary),1);
  assert.equal(f.calls[0][1],"chooseUnblock");assert.equal(f.calls[0][2],f.window);assert.equal(f.views.size,0);
});
test("completed-file opening rechecks success and invokes the original shell",async()=>{
  const d=download("complete",{succeeded:true,stopped:true,button:"show",default:"downloadsCmd_open",enabled:new Set(["downloadsCmd_open","downloadsCmd_show"])});
  const f=fixture([d]);await f.panel.open(1);const state=f.panel.snapshot();
  d.succeeded=false;assert.equal(f.execute(1,state.actionRevision,state.items[0].main),2);
  d.succeeded=true;f.files.clear();assert.equal(f.execute(1,state.actionRevision,state.items[0].main),2);
  f.files.add("C:/test.txt");assert.equal(f.execute(1,state.actionRevision,state.items[0].main),1);
  assert.deepEqual(f.calls,[["complete","downloadsCmd_open"]]);assert.equal(f.views.size,0);
});
test("late loading cannot reopen a dismissed or replaced session",async()=>{
  const f=fixture([download("late")]);let finish;
  f.list.addView=view=>{f.views.add(view);return new Promise(resolve=>finish=resolve);};
  const pending=f.panel.open(1);await new Promise(resolve=>setTimeout(resolve,0));f.panel.dismiss();finish();await pending;
  assert.equal(f.views.size,0);assert.equal(f.panel.snapshot().session,undefined);
});
test("native intent grammar and open prerequisites are bounded and fresh",async()=>{
  const f=fixture();const open=command=>f.panel.execute({command});
  assert.equal(open("downloads-panel:open:4294967296"),2);
  assert.equal(open("downloads-panel:open:1:2:3"),2);
  assert.equal(open("downloads-panel:invoke:1:0:255"),2);
  f.toolbar.customization.nativeOverflow.add("downloads-button");assert.equal(open("downloads-panel:open:1"),2);
  f.toolbar.customization.nativeOverflow.clear();assert.equal(open("downloads-panel:open:1"),1);
  await new Promise(resolve=>setTimeout(resolve,0));assert.equal(open("downloads-panel:open:1"),2);
  const state=f.panel.snapshot();assert.equal(f.execute(1,state.actionRevision,255),1);assert.deepEqual(f.calls,["all"]);
});

test("copy uses Firefox's helper and its completion renews the action",async()=>{
  const f=fixture([download("copy",{source:{url:"https://example.test/file"}})]);await f.panel.open(1);
  let state=f.panel.snapshot();const id=state.items[0].commands.find(c=>c.icon==="copy").id;
  assert.equal(f.execute(1,state.actionRevision,id),1);assert.equal(f.execute(1,state.actionRevision,id),2);state=f.panel.snapshot();
  const renewed=state.items[0].commands.find(c=>c.icon==="copy").id;assert.notEqual(renewed,id);assert.equal(f.execute(1,state.actionRevision,id),2);assert.equal(f.execute(1,state.actionRevision,renewed),1);
  assert.deepEqual(f.calls,[["copy","copy"],["copy","copy"]]);
  f.downloads[0].source.isDataURICleared=true;assert.equal(f.execute(1,state.actionRevision,renewed),2);
});
test("native automatic presentation yields to the per-window spam controller",async()=>{
  const f=fixture();await f.panel.open(1);assert.equal(f.panel.ownsAutomaticPresentation(),true);
  f.panel.integration={downloadSpamProtection:{getSpamListForWindow:()=>({})}};
  assert.equal(f.panel.ownsAutomaticPresentation(),false);assert.equal(f.views.size,0);assert.equal(f.panel.snapshot().closed,1);
});

test("unrelated eligibility changes cannot revoke an unchanged download capability",async()=>{
  const target=download("target"),other=download("other");const f=fixture([target,other]);await f.panel.open(1);
  const before=f.panel.snapshot(),row=before.items.find(r=>r.name==="target");
  other.enabled.clear();other.status="Canceled";for(const view of f.views)view.onDownloadChanged(other);
  const next=f.panel.snapshot();assert.ok(next.actionRevision>before.actionRevision);
  assert.equal(f.execute(1,before.actionRevision,row.primary),1);
});
test("command meaning changes revoke old capability IDs even if the slot is reused",async()=>{
  const d=download("meaning");const f=fixture([d]);await f.panel.open(1);const before=f.panel.snapshot(),pause=before.items[0].commands.find(c=>c.icon==="pause");
  d.stopped=d.canceled=true;d.status="Paused";d.default="downloadsCmd_pauseResume";for(const view of f.views)view.onDownloadChanged(d);
  const next=f.panel.snapshot(),resume=next.items[0].commands.find(c=>c.icon==="resume");assert.notEqual(resume.id,pause.id);
  assert.equal(f.execute(1,before.actionRevision,pause.id),2);assert.equal(f.execute(1,next.actionRevision,resume.id),1);
});

test("a live pause transition revokes a queued command before the next publication",async()=>{
  const d=download("racing"),f=fixture([d]);await f.panel.open(1);const before=f.panel.snapshot();
  const pause=before.items[0].commands.find(c=>c.icon==="pause");d.stopped=d.canceled=true;
  assert.equal(f.execute(1,before.actionRevision,pause.id),2);assert.deepEqual(f.calls,[]);
});
