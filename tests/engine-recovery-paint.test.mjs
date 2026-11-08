import assert from "node:assert/strict";
import { test } from "node:test";

test("fresh engine uses Firefox initial painting; later shell loss rebuilds the retained compositor", async () => {
  const names = ["Services", "ChromeUtils", "Ci", "Cc"];
  const saved = new Map(names.map(name => [name, globalThis[name]]));
  const observers = new Set(), windows = [];
  let gpu = 10, restarts = 0;
  globalThis.Services = { env: {get: () => "1"}, obs: {
    addObserver: callback => observers.add(callback), removeObserver: callback => observers.delete(callback)
  }};
  globalThis.ChromeUtils = {importESModule: () => ({watchRecoveryIdentity: () => ({bind(){}, completeRestore(){}, disconnect(){}, close(){}})})};
  globalThis.Ci = {nsIBaseWindow: {}, nsIGfxInfo: {}};
  globalThis.Cc = {"@mozilla.org/gfx/info;1": {getService: () => ({getFeatures: () => ({compositor: "webrender_software"})})}};
  const {watchProcessWindow} = await import("../upstream/firefox/browser/base/content/WinUIProcessStartup.sys.mjs?fresh-engine-test");
  function make(id) {
    const handlers = new Map(), paints = [];
    let finishPaint, restoring=false;
    const window = {closed:false,
      SessionStore:{isTabRestoring:()=>restoring},gBrowser:{selectedTab:{}},
      gWinUIPane: {element:true, schedule(){}, publish(){}, uninit(){this.element=false;}, init(){this.element=true;}},
      docShell: {treeOwner: {QueryInterface: () => ({nativeHandle:id})}},
      document: {documentElement: {setAttribute(){}, removeAttribute(){}}},
      gBrowserInit: {firstContentWindowPaintPromise: new Promise(resolve => finishPaint=resolve)},
      windowUtils: {get gpuProcessPid(){return gpu;}, terminateGPUProcess(){++restarts;}, syncWinUITabMenu: value => paints.push(JSON.parse(value))},
      addEventListener: (name, callback) => handlers.set(name,callback), removeEventListener: name => handlers.delete(name),
      close(){this.closed=true;handlers.get("unload")();}
    };
    windows.push(window); watchProcessWindow(window);
    return {window, paints, finishPaint, paint:()=>handlers.get("MozAfterPaint")?.(),
      restoring:value=>{restoring=value;}, restored:()=>handlers.get("SSTabRestored")?.(),
      notify: kind => {for(const callback of observers)callback(null,"fenn-process-window",`${kind}:${id}:shell-${id}`);},
      close:()=>{window.closed=true;handlers.get("unload")();}};
  }
  try {
    const a=make("a"), b=make("b"), c=make("c"), suppressed=make("suppressed");
    a.notify("1"); a.finishPaint(); await Promise.resolve();
    assert.equal(a.paints.length,0,"wait for all surviving endpoints before readiness");
    b.notify("1"); c.notify("1"); await Promise.resolve();
    assert.equal(a.paints.length,0,"unbound restored window still owns recovery completion");
    suppressed.notify("9"); await Promise.resolve();
    assert.equal(suppressed.window.closed,true,"explicit suppression closes only the unbound restored window");
    a.notify("9");assert.equal(a.window.closed,false,"late suppression cannot close a bound survivor");
    assert.equal(restarts,0,"fresh engine must keep its newly initialized compositor");
    assert.equal(a.paints.length,1);
    b.paint(); assert.equal(b.paints.length,0,"chrome paint cannot replace the initial content paint barrier");
    b.restoring(true);b.finishPaint(); await Promise.resolve(); b.paint();
    assert.equal(b.paints.length,0,"initial paint must not admit edits into the provisional restored-tab document");
    b.restoring(false);b.restored();b.paint();
    assert.equal(b.paints.length,1,"initial barrier and paint event cannot double publish");
    c.close(); c.finishPaint(); await Promise.resolve();
    assert.equal(c.paints.length,0,"a closed window cannot publish a late initial-content acknowledgement");
    a.notify("5"); b.notify("5"); a.notify("1"); b.notify("1");
    assert.equal(restarts,1,"shell loss still recreates the retained engine compositor");
    a.paint(); b.paint(); assert.equal(a.paints.length,1); assert.equal(b.paints.length,1);
    ++gpu; a.paint(); b.paint(); assert.equal(a.paints.length,2); assert.equal(b.paints.length,2);
    a.close(); b.close(); assert.equal(observers.size,0);
  } finally {
    for(const name of names) {if(saved.get(name)===undefined)delete globalThis[name];else globalThis[name]=saved.get(name);}
  }
});
