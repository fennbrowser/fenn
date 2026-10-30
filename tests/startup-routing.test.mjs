import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { test } from "node:test";

const source = readFileSync(new URL("../upstream/firefox/browser/modules/BrowserWindowTracker.sys.mjs", import.meta.url), "utf8")
  .replace(/^import .*;$/gm, "").replace("export const BrowserWindowTracker", "const BrowserWindowTracker") + "\nglobalThis.tracker = BrowserWindowTracker;";

function fixture({ mode = "1", platform = "win", owns = true, failure = false, routingFailure = false, localizationFailure = false } = {}) {
  const calls = [];
  const win = { addEventListener() {} };
  const context = vm.createContext({
    AppConstants: { platform, BROWSER_CHROME_URL: "chrome://browser/content/browser.xhtml" },
    XPCOMUtils: { defineLazyServiceGetters() {}, defineLazyPreferenceGetter() {} },
    console: {error() {}},
    ChromeUtils: { importESModule: () => ({prepareStartupPresentation: () => {
      if(localizationFailure)throw Error("localization unavailable");
      return '{"version":2,"rtl":true}';
    }}), defineESModuleGetters(lazy) {
      Object.assign(lazy, { AIWindow: { handleAIWindowOptions(options) {
        if (routingFailure) throw Error("routing failed");
        calls.push("routing"); return options.args;
      } }, PrivateBrowsingUtils: { enabled: true } });
    } },
    Ci: {}, Cc: { "@mozilla.org/supports-PRBool;1": { createInstance: () => ({ data: false }) } },
    Services: { env: { get: () => mode }, obs: { notifyObservers(subject, topic, data) {
      if(topic === "fenn-native-startup-window")assert.equal(data,localizationFailure?null:'{"version":2,"rtl":true}');
      calls.push(topic); if (subject) subject.data = owns;
    } }, ww: { openWindow(...args) {
      calls.push({ open: args }); if (failure === "throw") throw Error("open failed");
      return failure ? null : win;
    } } },
  });
  vm.runInContext(source, context);
  context.tracker.registerOpeningWindow = (window, privateWindow) => calls.push({ registered: window === win, privateWindow });
  return { calls, open: () => context.tracker.openWindow({ args: { destination: "queued" }, private: true, remote: false, fission: true }) };
}

test("native preparation follows routing and precedes browser chrome with unchanged options", () => {
  const f = fixture(); f.open();
  assert.equal(f.calls[0], "routing");
  assert.equal(f.calls[1], "fenn-native-startup-window");
  assert.match(f.calls[2].open[3], /,private,non-remote,fission/);
  assert.equal(f.calls[2].open[4].destination, "queued");
  assert.deepEqual(f.calls[3], { registered: true, privateWindow: true });
});

test("later windows do not repeat the one-use native preparation or localization", () => {
  const f=fixture();f.open();f.open();
  assert.equal(f.calls.filter(c=>c==='fenn-native-startup-window').length,1);
  assert.equal(f.calls.filter(c=>c?.open).length,2);
});

test("unavailable localization retains native fallback and browser creation",()=>{
  const f=fixture({localizationFailure:true});f.open();
  assert.equal(f.calls[1],"fenn-native-startup-window");
  assert.equal(f.calls[3].registered,true);
});

for (const failure of ["throw", "null"]) {
  for (const owns of [true, false]) {
    test(`failed open (${failure}) cancels only its own lease (${owns})`, () => {
      const f = fixture({ failure, owns }); assert.throws(f.open);
      assert.equal(f.calls.includes("fenn-native-startup-cancel"), owns);
    });
  }
}
for (const options of [{ mode: "" }, { mode: "2" }, { platform: "linux" }, { routingFailure: true }]) {
  test(`no native preparation for ${JSON.stringify(options)}`, () => {
    const f = fixture(options);
    if (options.routingFailure) assert.throws(f.open); else f.open();
    assert.equal(f.calls.some(c => typeof c === "string" && c.startsWith("fenn-native-")), false);
  });
}
