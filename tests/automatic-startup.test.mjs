import assert from "node:assert/strict";
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { beginAutomaticStartup, advanceAutomaticStartup, registerStartupChromeGate, unsupportedStartupLayout, recoverAutomaticStartup, cacheStartupLabels, prepareStartupPresentation } from "../upstream/firefox/browser/base/content/WinUIAutomaticStartup.sys.mjs";

const settle = () => new Promise(resolve => setImmediate(resolve));

function fixture({ early = true, fail = false, ready = true } = {}) {
  const saved = Object.fromEntries(["Services", "Cc", "Ci", "ChromeUtils", "Localization"].map(k => [k, globalThis[k]]));
  const attrs = new Map(); const calls = []; const prefs = new Map(); const presentations = []; let observer;
  globalThis.Services = { locale: {appLocaleAsBCP47:"en-US",isAppLocaleRTL:false}, env: { get: () => "1" }, prefs: {prefIsLocked: () => false, setBoolPref: (key, value) => prefs.set(key,value), getBoolPref: (key, fallback) => prefs.get(key) ?? fallback, addObserver() {}, removeObserver() {}}, obs: {
    addObserver(value) { observer = value; }, removeObserver() { observer = null; }
  } };
  const root = { hasAttribute: k => attrs.has(k), setAttribute: (k, v) => attrs.set(k, v) };
  const pane = { localization: () => ({formatValueSync: key => `localized:${key}`}), attached: true, nativeRevision: 1, addressSession: {}, addressToolbar: { active: true, navigation: true } };
  const window = { gWinUIPane: pane, gBrowserInit: { delayedStartupFinished: ready }, addEventListener() {}, removeEventListener() {},
    document: { documentElement: root, getElementById: () => ({}) }, getComputedStyle: () => ({ opacity: "0" }),
    windowUtils: { transferWinUIStartup: input => { const request=JSON.parse(input); if(request.operation===19){presentations.push(request);return "{}";} return JSON.stringify({ early, session: "42" }); } } };
  const connection = { stopped: false, start() {}, async attachPresenter() { calls.push("attach"); advanceAutomaticStartup(window); },
    async enableNormalTabs() { calls.push("adopt"); },
    async send(op) { calls.push(op); if (fail) throw Error("stale native binding"); } };
  globalThis.ChromeUtils = { importESModule: () => ({ async transferStartup(w, request) { return JSON.parse(w.windowUtils.transferWinUIStartup(JSON.stringify(request))); }, async connectFirefoxStartup(w, context, generation) {
    assert.equal(w, window); assert.equal(context, "42"); assert.equal(generation, "42"); calls.push("connect"); return connection;
  } }) };
  return { window, pane, attrs, calls, prefs, presentations, fireReady() { observer(window); }, restore() {
    for (const [k, v] of Object.entries(saved)) { if (v === undefined) delete globalThis[k]; else globalThis[k] = v; }
  } };
}

test("automatic connection waits for services and both native replacements, and cannot reenter", async () => {
  const f = fixture({ ready: false });
  try {
    beginAutomaticStartup(f.window); await settle();
    assert.equal(f.pane.automaticStartup.phase, "waiting");
    f.attrs.set("winui-replaces-tabs", ""); f.attrs.set("winui-hides-nav-bar", "");
    advanceAutomaticStartup(f.window); await settle(); assert.deepEqual(f.calls, []);
    f.fireReady(); await settle();
    assert.deepEqual(f.calls, ["connect", "attach", "adopt", 16]);
    assert.equal(f.attrs.get("winui-startup-chrome-ready"), "native");
    assert.equal(f.pane.automaticStartup.initialOpacity, "0");
    advanceAutomaticStartup(f.window); await settle(); beginAutomaticStartup(f.window); await settle();
    assert.deepEqual(f.calls, ["connect", "attach", "adopt", 16]);
  } finally { f.restore(); }
});

test("windows without an early presenter retain Firefox fallback", async () => {
  const f = fixture({ early: false });
  try { beginAutomaticStartup(f.window); await settle(); assert.deepEqual(f.calls, []);
    assert.equal(f.attrs.get("winui-startup-chrome-ready"), "fallback");
  } finally { f.restore(); }
});

test("a failed handoff never unmasks startup chrome or automatically retries", async () => {
  const f = fixture({ fail: true }); const error = console.error; console.error = () => {};
  try {
    f.attrs.set("winui-replaces-tabs", ""); f.attrs.set("winui-hides-nav-bar", "");
    beginAutomaticStartup(f.window); await settle(); advanceAutomaticStartup(f.window); await settle();
    assert.equal(f.pane.automaticStartup.phase, "failed");
    assert.equal(f.attrs.has("winui-startup-chrome-ready"), false);
    assert.deepEqual(f.calls, ["connect", "attach", "adopt", 16]);
  } finally { console.error = error; f.restore(); }
});

test("the early chrome sheet is registered once and scoped to browser chrome", () => {
  const f = fixture(); let registered = false; const calls = [];
  try {
    globalThis.Services.io = { newURI: value => value };
    globalThis.Ci = { nsIStyleSheetService: {} };
    globalThis.Cc = { "@mozilla.org/content/style-sheet-service;1": { getService: () => ({ USER_SHEET: 1,
      sheetRegistered: () => registered, loadAndRegisterSheet(uri, type) { registered = true; calls.push([uri, type]); }
    }) } };
    registerStartupChromeGate(); registerStartupChromeGate();
    assert.equal(calls.length, 1); assert.equal(calls[0][1], 1);
    assert.match(decodeURIComponent(calls[0][0]), /url\("chrome:\/\/browser\/content\/browser.xhtml"\)/);
    assert.match(decodeURIComponent(calls[0][0]), /opacity: 0 !important/);
  } finally { f.restore(); }
});


test("startup labels publish before delayed startup and do not rebuild on unchanged readiness", async () => {
  const f=fixture({ready:false});try {
    beginAutomaticStartup(f.window); await settle();
    assert.equal(f.presentations.length,1);
    assert.equal(f.presentations[0].newTab,"localized:winui-startup-new-tab");
    assert.equal(f.presentations[0].session,"42");
    assert.deepEqual(f.calls,[]);
    advanceAutomaticStartup(f.window); await settle();assert.equal(f.presentations.length,1);
  } finally {f.restore();}
});

test("unsupported initial layouts stay explicit and preserve the handoff until support returns", async () => {
  for(const [pref,value,reason] of [["sidebar.verticalTabs",true,"vertical-tabs"],
    ["fenn.winui.tabstrip.replaceFirefoxTabs",false,"retained-tabs"],
    ["fenn.winui.navigation.toolbar.enabled",false,"retained-navigation"]]) {
    const f=fixture();try {
      f.prefs.set(pref,value);beginAutomaticStartup(f.window); await settle();
      assert.equal(f.pane.automaticStartup.phase,"blocked");
      assert.equal(f.pane.automaticStartup.reason,reason);
      assert.deepEqual(f.calls,[]);assert.equal(f.presentations.at(-1).blocked,true);
      assert.equal(f.attrs.has("winui-startup-chrome-ready"),false);
      f.prefs.delete(pref);f.attrs.set("winui-replaces-tabs","");f.attrs.set("winui-hides-nav-bar","");
      advanceAutomaticStartup(f.window); await settle();
      assert.equal(f.pane.automaticStartup.phase,"connected");
      assert.equal(f.presentations.at(-1).blocked,false);
      assert.deepEqual(f.calls,["connect","attach","adopt",16]);
    } finally {f.restore();}
  }
});

test("private and temporary layout restrictions are distinguished", () => {
  const f=fixture();try {
    f.pane.addressAvailable=()=>false;assert.equal(unsupportedStartupLayout(f.window),"private-address");
    delete f.pane.addressAvailable;f.window.fullScreen=true;assert.equal(unsupportedStartupLayout(f.window),"fullscreen");
    f.window.fullScreen=false;f.attrs.set("customizing","");assert.equal(unsupportedStartupLayout(f.window),"customizing");
  } finally {f.restore();}
});


test("startup recovery validates current presentation and policy before changing only its setting", async () => {
  const f=fixture();try {
    f.prefs.set("sidebar.verticalTabs",true);beginAutomaticStartup(f.window); await settle();
    const state=f.pane.automaticStartup;
    const action={kind:"startup-recovery",session:state.session,revision:String(state.presentationRevision)};
    assert.equal(recoverAutomaticStartup(f.window,{...action,session:"wrong"}),2);
    assert.equal(recoverAutomaticStartup(f.window,{...action,revision:"0"}),2);
    Services.prefs.prefIsLocked=()=>true;
    assert.equal(recoverAutomaticStartup(f.window,action),2);
    assert.equal(f.prefs.get("sidebar.verticalTabs"),true);
    advanceAutomaticStartup(f.window); await settle();
    assert.equal(f.presentations.at(-1).recoverable,false);
    Services.prefs.prefIsLocked=()=>false;advanceAutomaticStartup(f.window); await settle();
    action.revision=String(state.presentationRevision);
    assert.equal(recoverAutomaticStartup(f.window,action),1);
    assert.equal(f.prefs.get("sidebar.verticalTabs"),false);
    assert.equal(f.prefs.size,1);
    assert.equal(recoverAutomaticStartup(f.window,action),2);
  } finally {f.restore();}
});


test("startup label cache writes only localized presentation atomically and coalesces latest labels", async () => {
  const writes=[];let release;
  const first=new Promise(resolve=>{release=resolve;});
  const io={async writeJSON(path,value,options){writes.push({path,value,options});if(writes.length===1)await first;}};
  const paths={profileDir:"resolved-profile",join:(...parts)=>parts.join("/")};
  const pending=cacheStartupLabels({address:"premier"},"fr",io,paths);
  await cacheStartupLabels({address:"intermediate"},"fr",io,paths);
  await cacheStartupLabels({address:"dernier"},"fr",io,paths);
  release();await pending;
  assert.equal(writes.length,2);
  assert.deepEqual(writes[1].value,{version:3,locale:"fr",rtl:false,labels:{address:"dernier"}});
  assert.equal(writes[0].path,"resolved-profile/fenn-startup-labels.json");
  assert.equal(writes[0].options.tmpPath,writes[0].path+".tmp");
  await cacheStartupLabels({address:"dernier"},"fr",io,paths);assert.equal(writes.length,2);
  await assert.rejects(cacheStartupLabels({address:"retry"},"fr",{writeJSON:async()=>{throw Error("disk");}},paths),/disk/);
  await cacheStartupLabels({address:"retry"},"fr",io,paths);assert.equal(writes.length,3);
  await cacheStartupLabels({address:"retry"},"fr",io,paths,true);
  assert.equal(writes.length,4);assert.equal(writes[3].value.rtl,true);
});

test("first startup presentation uses Firefox localization and negotiated direction without a cache", () => {
  const f=fixture();const calls=[];
  try {
    Services.locale={appLocaleAsBCP47:"ar",isAppLocaleRTL:true};
    globalThis.Localization=class {
      constructor(resources,sync){calls.push({resources,sync});}
      formatValueSync(id){
        const ftl=readFileSync(new URL("../upstream/firefox/browser/locales/en-US/browser/tabbrowser.ftl",import.meta.url),"utf8");
        assert.match(ftl,new RegExp(`^${id} =`,"m"));
        return `localized:${id}`;
      }
    };
    const importModule=ChromeUtils.importESModule;
    ChromeUtils.importESModule=path=>path.includes('CustomizableUI')?{CustomizableUI:{}}:
      path.includes('WinUIToolbarCustomization')?{startupToolbarControls:()=>['back','address','extensions','profile']}:importModule(path);
    Localization.prototype.formatMessagesSync=()=>[{attributes:[{name:'label',value:'Fenn'},{name:'tooltiptext',value:'Localized menu'}]}];
    const value=JSON.parse(prepareStartupPresentation());
    assert.equal(value.version,3);assert.equal(value.locale,"ar");assert.equal(value.rtl,true);
    assert.equal(Object.keys(value.labels).length,14);
    assert.equal(value.labels.engineRetry,"localized:winui-engine-recovery-retry");
    assert.equal(value.labels.address,"localized:winui-startup-address");
    assert.deepEqual(value.toolbar,{controls:['back','address','extensions','profile'],productMenu:true,menuLabel:'Fenn',menuTooltip:'Localized menu'});
    Services.prefs.setBoolPref('fenn.winui.productmenu.enabled',false);
    assert.equal(JSON.parse(prepareStartupPresentation()).toolbar.productMenu,false);
    assert.deepEqual(calls.slice(0,2),[{resources:["branding/brand.ftl","browser/tabbrowser.ftl"],sync:true},{resources:["branding/brand.ftl","browser/appmenu.ftl"],sync:true}]);
  } finally {f.restore();}
});
for(const bad of [null,"", "x".repeat(1025), "bad\0label"]) {
  test(`invalid initial label rejected (${bad?.length ?? "null"})`,()=>{
    const f=fixture();try {
      globalThis.Localization=class {formatValueSync(){return bad;}};
      assert.throws(prepareStartupPresentation,/Invalid initial native startup label/);
    }finally{f.restore();}
  });
}


test("saved startup preview tracks profile placement and menu preference without window data", async () => {
  const f=fixture();const writes=[],observers=new Map();
  const saved={IOUtils:globalThis.IOUtils,PathUtils:globalThis.PathUtils};
  try {
    globalThis.IOUtils={writeJSON:async(path,value)=>writes.push(structuredClone(value))};
    globalThis.PathUtils={profileDir:'preview-profile',join:(...parts)=>parts.join('/')};
    globalThis.Localization=class {
      formatValueSync(id){return 'localized:'+id;}
      formatMessagesSync(){return [{attributes:[{name:'label',value:'menu'},{name:'tooltiptext',value:'tip'}]}];}
    };
    let controls=['back','address','extensions','profile'];
    ChromeUtils.importESModule=path=>path.includes('CustomizableUI')?{CustomizableUI:{}}:
      {startupToolbarControls:()=>[...controls]};
    Services.prefs.addObserver=(key,callback)=>observers.set(key,callback);
    Services.obs.addObserver=(callback,key)=>observers.set(key,callback);
    const module=await import('../upstream/firefox/browser/base/content/WinUIAutomaticStartup.sys.mjs?preview-observers');
    module.prepareStartupPresentation();await settle();
    assert.deepEqual(writes.at(-1).toolbar.controls,controls);
    controls=['profile','back','address','extensions'];
    observers.get('browser.uiCustomization.state')();await settle();
    assert.deepEqual(writes.at(-1).toolbar.controls,controls);
    Services.prefs.setBoolPref('fenn.winui.productmenu.enabled',false);
    observers.get('fenn.winui.productmenu.enabled')();await settle();
    assert.equal(writes.at(-1).toolbar.productMenu,false);
    assert.deepEqual(Object.keys(writes.at(-1).toolbar).sort(),['controls','menuLabel','menuTooltip','productMenu']);
    assert.equal(observers.size,2);
  } finally {Object.assign(globalThis,saved);f.restore();}
});
