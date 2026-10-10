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
  s.input={view};s.beginNativePresentation({open:false});
  assert.equal(view.nativePresentation,s);assert.equal(s.nativeOpen,false);
  assert.deepEqual(calls,['close','clear']);assert.equal(view.visibleResults.length,0);
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
    constructor(){events.push('construct');this.sessionId='new';}
    beginNativePresentation(options){events.push('bind');view.nativePresentation=this;this.nativeOpen=options.open;}
    snapshot(){assert.equal(view.nativePresentation,expectNative ? this : null);events.push('snapshot');return null;}
    nativeModes(){return {};}
    nativeRowsWithAcknowledgments(rows){return rows;}
    preparePublication(state){return state;}
  }
  const sandbox=vm.createContext({
    Services:{prefs:{getBoolPref:()=>true}},
    ChromeUtils:{importESModule:()=>({WinUIUrlbarSession:Session})},
    window:{fullScreen:false,windowUtils:{syncWinUIAddress:state=>publications.push(JSON.parse(state))}},
    document:{fullscreenElement:null,documentElement:{matches:()=>false}},
    gBrowser:{selectedTab:tab,selectedBrowser:browser},
    gURLBar:{view,inputField:{value:'new-page',getAttribute:()=>''},searchModeVisibility:{}},
  });
  const source=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-winui-pane.js',import.meta.url),'utf8');
  const start=source.indexOf('  syncAddress() {'),end=source.indexOf('\n  // One native address intent',start);
  vm.runInContext('this.syncAddress = ({'+source.slice(start,end)+'}).syncAddress;',sandbox);
  const old={close(){events.push('close-old');view.nativePresentation=null;}};
  const owner={attached:true,addressSession:old,addressTab:tab,addressDocument:1,
    addressToolbar:{update:()=>true,activate(){events.push('activate');}},
    nativeLabels:()=>({}),invalidateAddress(){}};
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
