import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const sessionContext=vm.createContext({console});
vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs',import.meta.url),'utf8')
  .replace(/^import .*;$/m,'').replace('export class WinUIUrlbarSession','this.Session = class WinUIUrlbarSession'),sessionContext);

test('a new native owner can suppress DOM results without opening suggestions',()=>{
  const s=Object.create(sessionContext.Session.prototype),calls=[];
  const view={close(){calls.push('close');},clearSelection(){calls.push('clear');},visibleResults:['old']};
  s.input={view,controller:{visibleResults:['old']}};s.beginNativePresentation({open:false});
  assert.equal(view.nativePresentation,s);assert.equal(s.nativeOpen,false);
  assert.deepEqual(calls,['close','clear']);assert.equal(s.input.controller.visibleResults.length,0);
  s.beginNativePresentation();
  assert.equal(s.nativeOpen,false,'ownership checks must not reopen a dismissed list');
  assert.equal(calls.length,2);
  s.nativeOpen=true;
  s.beginNativePresentation({open:false});assert.equal(s.nativeOpen,true,'existing interaction is not reset by rebinding');
});

function fixture(expectNative = true) {
  const events=[],publications=[],view={nativePresentation:null};
  const tab={},browser={currentURI:{spec:'new-page'},browsingContext:{currentWindowGlobal:{innerWindowId:2}}};
  class Session {
    get engine(){return 'retained';}
    constructor(){events.push('construct');this.sessionId='new';}
    beginNativePresentation(options){events.push('bind');view.nativePresentation=this;this.nativeOpen=options.open;}
    snapshot(){assert.equal(view.nativePresentation,expectNative ? this : null);events.push('snapshot');return null;}
    nativeModes(){return {};}
    nativeRowsWithAcknowledgments(rows){return rows;}
    preparePublication(state){return state;}
  }
  const sandbox=vm.createContext({
    Services:{prefs:{getBoolPref:()=>true}},
    ChromeUtils:{importESModule:()=>({WinUIIndependentUrlbarSession:Session})},
    window:{fullScreen:false,windowUtils:{syncWinUIAddress:state=>publications.push(JSON.parse(state))}},
    document:{fullscreenElement:null,documentElement:{matches:()=>false}},
    gBrowser:{selectedTab:tab,selectedBrowser:browser},
    gURLBar:{view,inputField:{value:'new-page',getAttribute:()=>''},searchModeVisibility:{}},
  });
  const source=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-winui-pane.js',import.meta.url),'utf8');
  const start=source.indexOf('  syncAddress() {'),end=source.indexOf('\n  // One native address intent',start);
  vm.runInContext('this.syncAddress = ({'+source.slice(start,end)+'}).syncAddress;',sandbox);
  const old={engine:'independent',close(){events.push('close-old');view.nativePresentation=null;}};
  const owner={attached:true,addressSession:old,addressTab:tab,addressDocument:1,
    addressToolbar:{update:()=>true,activate(){events.push('activate');}},
    nativeLabels:()=>({}),addressAvailable:()=>true,invalidateAddress(){}};
  view.nativePresentation=old;
  return {owner,sandbox,view,events,publications};
}

test('document replacement binds the new owner before publishing closed suggestions',()=>{
  const f=fixture();f.sandbox.syncAddress.call(f.owner);
  assert.deepEqual(f.events,['close-old','construct','bind','snapshot','activate']);
  assert.equal(f.view.nativePresentation,f.owner.addressSession);
  assert.equal(f.publications[0].open,false);assert.equal(f.publications[0].rows.length,0);
  f.events.length=0;f.sandbox.syncAddress.call(f.owner);
  assert.deepEqual(f.events,['snapshot','activate']);
});

test('unsupported presentation releases the old owner and never creates a replacement',()=>{
  const f=fixture();f.owner.addressToolbar.update=()=>false;f.sandbox.syncAddress.call(f.owner);
  assert.deepEqual(f.events,['close-old']);assert.equal(f.view.nativePresentation,null);
  assert.equal(f.owner.addressSession,null);assert.equal(f.publications[0].enabled,false);
});

test('replacement does not take ownership back from the original presenter',()=>{
  const f=fixture(false);f.view.nativePresentation=null;f.sandbox.syncAddress.call(f.owner);
  assert.deepEqual(f.events,['close-old','construct','snapshot','activate']);
  assert.equal(f.view.nativePresentation,null);
});


test('controller open state survives hidden query retention and rejects a released owner',()=>{
  const s=Object.create(sessionContext.Session.prototype);
  const browser={currentURI:{spec:'page'}};
  const controller={resultsOpen:false,visibleResults:[],engagementEvent:{addExposure(){}}};
  const view={close(){controller.resultsOpen=false;},clearSelection(){}};
  s.input={view,controller,inputField:{value:'query'}};
  s.window={gBrowser:{selectedBrowser:browser}};s._sequence=0;
  s.beginNativePresentation();assert.equal(controller.resultsOpen,true);
  const active={browser,page:'page',inputValue:'query',results:[],exposed:new Set()};
  s.active=active;s.suspendNativePresentation();
  assert.equal(controller.resultsOpen,false);assert.equal(s.active,active);
  browser.currentURI.spec='other';s.resumeNativePresentation();
  assert.equal(controller.resultsOpen,false,'a different document cannot reopen retained results');
  browser.currentURI.spec='page';s.resumeNativePresentation();
  assert.equal(controller.resultsOpen,true);assert.equal(s.sequence,1);
  s.cancel=()=>{assert.equal(view.nativePresentation,null);assert.equal(controller.resultsOpen,false);};
  s.releaseNativePresentation();
  const successor=Object.create(sessionContext.Session.prototype);successor.input=s.input;
  successor.beginNativePresentation();assert.equal(controller.resultsOpen,true);
  assert.equal(s.nativeOpen,false);s.nativeOpen=false;s.releaseNativePresentation();
  assert.equal(controller.resultsOpen,true,'old owners cannot close a successor');
});

test('picked close captures exposure and closes logical presentation after cancellation',()=>{
  const s=Object.create(sessionContext.Session.prototype),visible=[{}];
  const controller={resultsOpen:true,visibleResults:visible};
  s.input={controller,view:{nativePresentation:s}};
  s.cancel=()=>{assert.equal(controller.resultsOpen,true);controller.visibleResults=[];};
  s.onNativeViewClosed(true);
  assert.equal(controller.resultsOpen,false);assert.equal(controller.visibleResults,visible);
});

test('the pane always constructs the one session class and shows the native address by preference',()=>{
  const f=fixture();
  f.sandbox.syncAddress.call(f.owner);
  assert.deepEqual(f.events.slice(0,3),['close-old','construct','bind']);
  const source=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-winui-pane.js',import.meta.url),'utf8');
  assert.doesNotMatch(source,/address\.independent|addressEngine|addressIndependentPref/);
  const start=source.indexOf('  addressAvailable() {'),end=source.indexOf('  syncAddress() {',start);
  const context=vm.createContext({window:{},PrivateBrowsingUtils:{isWindowPrivate:win=>win.private},
    Services:{prefs:{getBoolPref:(name,fallback)=>context.prefs[name] ?? fallback}},prefs:{}});
  vm.runInContext('this.addressAvailable = ({'+source.slice(start,end).replace(/,\s*$/,'')+'}).addressAvailable;',context);
  const owner={addressPrivatePref:'fenn.winui.address.private'};
  assert.equal(context.addressAvailable.call(owner),true,'a normal window is native');
  context.window.private=true;
  assert.equal(context.addressAvailable.call(owner),true,'private windows are native by default');
  context.prefs['fenn.winui.address.private']=false;
  assert.equal(context.addressAvailable.call(owner),false,'a private window keeps Firefox\'s URL bar when the preference is off');
  context.window.private=false;
  assert.equal(context.addressAvailable.call(owner),true,'the private preference does not affect normal windows');
});
