import assert from "node:assert/strict";
import { test } from "node:test";
import { watchProcessWindow } from "../upstream/firefox/browser/base/content/WinUIProcessStartup.sys.mjs";

test("recovery waits for surviving windows and never replays startup", async () => {
  const saved = { Services: globalThis.Services, ChromeUtils: globalThis.ChromeUtils, Ci: globalThis.Ci, Cc: globalThis.Cc };
  const observers = new Set(), windows = [];
  let gpuRestarts = 0, startups = 0, startupPaints = 0, gpu = 10;
  const paints = [], tasks = [];
  globalThis.Services = { tm: { dispatchToMainThread: task => tasks.push(task) }, env: { get: name => name === "FENN_NATIVE_SHELL_PROCESS" ? "1" : "" },
    obs: { addObserver: observer => observers.add(observer), removeObserver: observer => observers.delete(observer) } };
  globalThis.ChromeUtils = { importESModule: () => ({ beginAutomaticStartup: () => ++startups, revealStartupPage: () => ++startupPaints }) };
  globalThis.Ci = { nsIBaseWindow: {}, nsIGfxInfo: {} };
  globalThis.Cc = { "@mozilla.org/gfx/info;1": { getService: () => ({ getFeatures: () => ({compositor:"webrender_software"}) }) } };
  function make(id) {
    const handlers = new Map(), attributes = new Map();
    const pane = { element: true, uninit() { this.element = false; }, init() { this.element = true; }, schedule() {}, publications: 0, geometries: 0,
      chromeLayout: {signature: "cached"}, addressPublication: "cached", addressSession: {draft: "unsent"},
      updateGeometry() { ++this.geometries; }, publish() { ++this.publications; } };
    const window = { closed: false, gWinUIPane: pane,
      docShell: { treeOwner: { QueryInterface: () => ({ nativeHandle: id }) } },
      document: { documentElement: { setAttribute: (key, value) => attributes.set(key, value), getAttribute: key => attributes.get(key), removeAttribute: key => attributes.delete(key) } },
      gBrowserInit: { firstContentWindowPaintPromise: Promise.resolve() },
      addEventListener: (name, handler) => handlers.set(name, handler),
      removeEventListener: name => handlers.delete(name),
      windowUtils: { get gpuProcessPid() { return gpu; }, terminateGPUProcess() { assert.equal(window.closed, false); ++gpuRestarts; }, syncWinUITabMenu(value) { if (JSON.parse(value).recoveryPainted) paints.push(JSON.parse(value)); } },
      close() { this.closed = true; handlers.get("unload")(); },
      notify(kind) { for (const observer of [...observers]) observer(null, "fenn-process-window", `${kind}:${id}:shell-${id}`); },
      paint: () => handlers.get("MozAfterPaint"),
      state: () => attributes.get("winui-startup-chrome-ready") };
    windows.push(window); watchProcessWindow(window); window.notify("1"); return window;
  }
  try {
    const a = make("a"), b = make("b");
    assert.equal(startups, 2);
    a.notify("5"); b.notify("5"); a.notify("1");
    assert.equal(gpuRestarts, 0, "a sibling still needs to rebind");
    b.close();
    assert.equal(gpuRestarts, 1, "closing the last pending sibling releases the survivor");
    assert.equal(a.state(), "rebound");
    const oldPaint = a.paint();
    oldPaint();
    assert.equal(paints.length, 0, "old GPU paint cannot acknowledge replacement rendering");
    ++gpu; oldPaint(); oldPaint();
    assert.equal(paints.length, 1, "current paint is reported once");
    assert.equal(paints[0].software, true);
    assert.equal(a.state(), "rebound", "paint is not the shell/input acknowledgement");
    const pane = a.gWinUIPane, session = pane.addressSession, publications = pane.publications;
    a.notify("1");
    assert.equal(pane.geometries, 1, "resumed bound page refreshes minimized geometry");
    assert.equal(pane.publications, publications + 1);
    assert.equal(pane.chromeLayout.signature, "");
    assert.equal(pane.addressPublication, null);
    assert.equal(pane.addressDirty, true);
    assert.equal(pane.addressSession, session, "refresh must retain the unsent edit session");
    assert.equal(session.draft, "unsent");
    assert.equal(a.state(), "rebound", "refresh does not grant readiness");
    assert.equal(gpuRestarts, 1, "resume must not recreate the compositor");
    a.notify("6"); assert.equal(a.state(), "ready");
    a.notify("1");
    assert.equal(pane.geometries, 1, "ready windows ignore duplicate page notifications");
    assert.equal(gpuRestarts, 1, "duplicate bind does not restart GPU again");
    a.notify("5"); a.notify("1");
    assert.equal(gpuRestarts, 2, "a later recovery gets its own completion");
    ++gpu; oldPaint();
    assert.equal(paints.length, 1, "a retired callback cannot publish into a later recovery");
    assert.equal(startups, 2, "neither recovery repeats first-launch navigation");
    assert.equal(startupPaints, 2, "recovery does not replay the initial page-paint gate");
    a.close();

    const c = make("c"), d = make("d");
    c.notify("5"); d.notify("5"); c.close(); d.close();
    assert.equal(gpuRestarts, 2, "all windows closing must not recreate the GPU");

    const e = make("e"), f = make("f");
    e.notify("5"); f.notify("5"); e.notify("1"); f.notify("2");
    assert.equal(gpuRestarts, 3, "terminal sibling cannot strand a rebound window");
    assert.equal(f.state(), "fallback");
    e.close(); f.close();
    assert.equal(observers.size, 0);
    const g = make("g"), h = make("h");
    assert.equal(g.document.documentElement.getAttribute("winui-native-frame"), "");
    const startupCount = startups;
    g.notify("7"); h.notify("7");
    assert.equal(g.document.documentElement.getAttribute("winui-native-frame"), undefined, "stock fallback restores Gecko surround ownership");
    assert.equal(tasks.length, 1, "all stock windows share one compositor recreation");
    assert.equal(gpuRestarts, 3, "all native proxies must be restored before GPU recreation");
    tasks.shift()();
    assert.equal(gpuRestarts, 4);
    const stockPaint = g.paint(); stockPaint();
    assert.equal(g.state(), "fallback", "old GPU cannot complete stock presentation");
    ++gpu; stockPaint(); h.paint()();
    assert.equal(g.state(), "stock-ready");
    assert.equal(h.state(), "stock-ready");
    g.notify("1"); g.notify("5"); g.notify("6"); g.notify("7");
    assert.equal(g.state(), "stock-ready", "late shell notifications cannot leave terminal fallback");
    assert.equal(startups, startupCount, "stock fallback never replays startup navigation");
    assert.equal(tasks.length, 0);
    g.close(); h.close();
    assert.equal(observers.size, 0);
    const i = make("i"), j = make("j");
    const restartsBeforeClose = gpuRestarts;
    i.notify("7"); j.notify("7");
    assert.equal(tasks.length, 1);
    i.close(); j.close();
    tasks.shift()();
    assert.equal(gpuRestarts, restartsBeforeClose, "queued stock completion cannot restart GPU after all windows close");
    assert.equal(i.paint(), undefined);
    assert.equal(j.paint(), undefined);
    const k = make("k");
    k.notify("7"); tasks.shift()();
    const retiredStockPaint = k.paint();
    k.close(); ++gpu; retiredStockPaint();
    assert.equal(k.state(), "fallback", "closed stock window rejects its retired paint callback");
    assert.equal(observers.size, 0);

    await Promise.resolve();
  } finally {
    for (const window of windows) if (!window.closed) window.close();
    for (const [key, value] of Object.entries(saved)) {
      if (value === undefined) delete globalThis[key]; else globalThis[key] = value;
    }
  }
});
