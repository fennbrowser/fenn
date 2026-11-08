import assert from "node:assert/strict";
import { test } from "node:test";
import { watchRecoveryIdentity, createRecoveryEmptyWindows, RECOVERY_WINDOW_KEY } from "../upstream/firefox/browser/base/content/WinUIRecoveryIdentity.sys.mjs";

test("recovery identity waits for restore, survives rebind, and never accesses private session state", async () => {
  const saved = globalThis.Services, savedChrome = globalThis.ChromeUtils;
  globalThis.ChromeUtils = {importESModule:()=>({SessionStartup:{sessionType:1,NO_SESSION:0,onceInitialized:Promise.resolve()}})};
  let generated = 0;
  globalThis.Services = { uuid: { generateUUID() { ++generated; return "{12345678-1234-1234-1234-123456789abc}"; } } };
  function make(privateWindow = false, restoring = false) {
    const handlers = new Map(), frames = [], writes = [];
    let resolve, key = "";
    const promise = new Promise(done => resolve = done);
    const window = {
      closed: false,
      isPrivate: privateWindow,
      PrivateBrowsingUtils: { isWindowPrivate: w => w.isPrivate },
      get SessionStore() {
        assert.equal(privateWindow, false, "private windows must never access session keys");
        return { promiseAllWindowsRestored: promise,
          getCustomWindowValue(w, name) { assert.equal(name, RECOVERY_WINDOW_KEY); return key; },
          setCustomWindowValue(w, name, value) { assert.equal(name, RECOVERY_WINDOW_KEY); writes.push(value); key = value; } };
      },
      addEventListener(name, callback) { handlers.set(name, callback); },
      removeEventListener(name) { handlers.delete(name); },
      windowUtils: { syncWinUITabMenu(frame) { frames.push(JSON.parse(frame).recoveryWindow); } },
    };
    const watcher = watchRecoveryIdentity(window, { restoring });
    return { window, watcher, frames, writes, handlers, resolve, setKey(value) { key = value; } };
  }
  try {
    const normal = make(); normal.watcher.bind();
    assert.deepEqual(normal.frames, [{ revision: 1, privacy: 1, key: "" }]);
    assert.equal(generated, 0);
    normal.setKey("abcdefabcdefabcdefabcdefabcdefab"); normal.resolve(); await Promise.resolve();
    assert.equal(normal.frames.at(-1).key, "abcdefabcdefabcdefabcdefabcdefab");
    assert.equal(generated, 0, "restored identity must not be replaced");
    normal.watcher.disconnect(); normal.handlers.get("SSWindowStateBusy")();
    assert.equal(normal.frames.length, 2);
    normal.handlers.get("SSWindowStateReady")(); normal.watcher.bind();
    assert.equal(normal.frames.at(-1).revision, 3);
    assert.equal(normal.frames.at(-1).key, normal.frames[1].key);
    normal.handlers.get("SSWindowStateBusy")();
    assert.equal(normal.frames.at(-1).key, "", "in-progress state replacement revokes mapping");
    normal.setKey("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"); normal.handlers.get("SSWindowStateReady")();
    assert.equal(normal.frames.at(-1).key, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    normal.watcher.close(); assert.equal(normal.handlers.size, 0);

    const fresh = make(); fresh.watcher.bind(); fresh.resolve(); await Promise.resolve();
    assert.equal(fresh.writes.length, 1); assert.equal(generated, 1);
    fresh.watcher.bind(); assert.equal(fresh.writes.length, 1); fresh.watcher.close();

    const invalid = make(); invalid.setKey("corrupt"); invalid.watcher.bind(); invalid.resolve(); await Promise.resolve();
    assert.equal(invalid.frames.at(-1).key, ""); assert.equal(invalid.writes.length, 0); invalid.watcher.close();

    const privateWindow = make(true); privateWindow.watcher.bind(); privateWindow.watcher.disconnect(); privateWindow.watcher.bind();
    assert.ok(privateWindow.frames.every(frame => frame.privacy === 2 && frame.key === ""));
    assert.equal(privateWindow.handlers.size, 0); privateWindow.watcher.close();

    const closed = make(); closed.watcher.bind(); closed.watcher.close(); closed.resolve(); await Promise.resolve();
    assert.equal(closed.frames.length, 1); assert.equal(closed.writes.length, 0);

    const first = make(false, true), second = make(false, true), hidden = make(true, true);
    Services.wm = { getEnumerator: () => [first.window, second.window, hidden.window] };
    for (const item of [first, second]) {
      item.setKey(item === first ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" : "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
      item.watcher.bind();
      assert.equal(item.frames.at(-1).restoreCount, 0, "pre-bind metadata cannot claim a complete restore");
      assert.equal(item.frames.at(-1).key, "");
    }
    first.resolve(); second.resolve(); await Promise.resolve();
    for (const item of [first, second]) {
      assert.equal(item.frames.at(-1).restoreCount, 2, "complete inventory excludes private windows");
      assert.notEqual(item.frames.at(-1).key, "");
      item.handlers.get("SSWindowStateBusy")();
      assert.equal(item.frames.at(-1).restoreCount, 0);
      item.handlers.get("SSWindowStateReady")();
      assert.equal(item.frames.at(-1).restoreCount, 2);
      item.watcher.completeRestore(); item.watcher.bind();
      assert.equal("restoreCount" in item.frames.at(-1), false, "ordinary publications drop the completed restore inventory");
      item.watcher.close();
    }
    hidden.watcher.close();
  } finally { if (saved === undefined) delete globalThis.Services; else globalThis.Services = saved; if(savedChrome===undefined)delete globalThis.ChromeUtils;else globalThis.ChromeUtils=savedChrome; }
});


test("missing checkpoint requests create only empty normal windows with stable identities", async () => {
  const saved = {Services:globalThis.Services, Cc:globalThis.Cc, Ci:globalThis.Ci};
  const opened = [], frames = [];
  globalThis.Services = {env:{get:()=>"1"},wm:{getEnumerator:()=>opened.map(item=>item.window)}};
  globalThis.Cc = {"@mozilla.org/supports-string;1":{createInstance:()=>({data:""})}};
  globalThis.Ci = {nsISupportsString:{}};
  const owner = {closed:false, PrivateBrowsingUtils:{isWindowPrivate:w=>!!w.isPrivate},
    OpenBrowserWindow(options) {
      assert.equal(options.private,false);
      assert.equal(options.args.data,"about:blank","home page and draft must never become a destination");
      let key="";
      const window={closed:false,delayedStartupPromise:Promise.resolve(),PrivateBrowsingUtils:this.PrivateBrowsingUtils,
        SessionStore:{promiseAllWindowsRestored:Promise.resolve(),getCustomWindowValue:()=>key,setCustomWindowValue:(w,n,value)=>key=value},
        windowUtils:{syncWinUITabMenu:value=>frames.push(JSON.parse(value))},addEventListener(){},removeEventListener(){}};
      opened.push({window,options});return window;
    }};
  try {
    for(const raw of ['[]','["bad"]',JSON.stringify(["d".repeat(32),"d".repeat(32)])])assert.throws(()=>createRecoveryEmptyWindows(owner,raw));
    assert.equal(opened.length,0,"validate whole batch before creating anything");
    const keys=["e".repeat(32),"f".repeat(32)];
    createRecoveryEmptyWindows(owner,JSON.stringify(keys));
    assert.equal(opened.length,2);
    for(const {window} of opened){watchRecoveryIdentity(window,{restoring:true}).bind();}
    assert.deepEqual(frames.filter(f=>f.recoveryWindow.key).map(f=>f.recoveryWindow.key),keys,"requested identities bind before new-window delayed startup");
    await new Promise(resolve=>setImmediate(resolve));
    assert.deepEqual(opened.map(({window})=>window.SessionStore.getCustomWindowValue()),keys);
    assert.throws(()=>createRecoveryEmptyWindows(owner,JSON.stringify(keys)),"duplicate request cannot recreate windows");
    createRecoveryEmptyWindows({...owner,isPrivate:true},JSON.stringify(["c".repeat(32)]));
    createRecoveryEmptyWindows({...owner,closed:true},JSON.stringify(["b".repeat(32)]));
    assert.equal(opened.length,2);
  } finally {
    for(const [name,value] of Object.entries(saved)){if(value===undefined)delete globalThis[name];else globalThis[name]=value;}
  }
});


test("only Firefox's no-session startup window is a suppressible placeholder", async () => {
  const saved={Services:globalThis.Services,ChromeUtils:globalThis.ChromeUtils};
  const frames=[], writes=[];let type=0;
  globalThis.Services={wm:{getEnumerator:()=>[window]},uuid:{generateUUID:()=>"12345678-1234-1234-1234-123456789abc"}};
  globalThis.ChromeUtils={importESModule:()=>({SessionStartup:{sessionType:type,NO_SESSION:0,onceInitialized:Promise.resolve()}})};
  const window={closed:false,PrivateBrowsingUtils:{isWindowPrivate:()=>false},SessionStore:{promiseAllWindowsRestored:Promise.resolve(),getCustomWindowValue:()=>"",setCustomWindowValue:(...args)=>writes.push(args)},windowUtils:{syncWinUITabMenu:raw=>frames.push(JSON.parse(raw).recoveryWindow)},addEventListener(){},removeEventListener(){}};
  try {
    const watcher=watchRecoveryIdentity(window,{restoring:true});watcher.bind();
    assert.equal(frames.at(-1).restoreCount,0);
    await Promise.resolve();
    assert.deepEqual(frames.at(-1),{revision:3,privacy:1,key:"",placeholder:true,restoreCount:1});
    assert.equal(writes.length,0,"do not persist an identity for the disposable startup placeholder");
    watcher.close();
    type=1;const extra={...window};const other=watchRecoveryIdentity(extra,{restoring:true});other.bind();await Promise.resolve();
    assert.equal(frames.at(-1).placeholder,undefined,"unkeyed restored windows remain real windows");
    assert.equal(writes.length,1);other.close();
  } finally {for(const [name,value] of Object.entries(saved)){if(value===undefined)delete globalThis[name];else globalThis[name]=value;}}
});
