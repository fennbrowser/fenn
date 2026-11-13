import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

globalThis.Ci = {nsIContentPolicy:{}, nsIHttpChannel:{}, nsIPermissionManager:{ALLOW_ACTION:1}};
globalThis.ChromeUtils = {generateQI:() => () => {}, now:() => 0};
globalThis.Components = {isSuccessCode:() => true};
const delays = [];
globalThis.__adblockTimer = (callback, ms) => { delays.push(ms); return setTimeout(callback, 0); };
let source = await readFile(new URL("../upstream/firefox/browser/components/fennadblock/FennAdblockService.sys.mjs", import.meta.url), "utf8");
source = source.replace(/^import\b[^;]*;$/gm, "");
source = "const setTimeout=(f,ms)=>globalThis.__adblockTimer(f,ms), clearTimeout=()=>{};\n" + source;
const {FennAdblockService: service} = await import("data:text/javascript;base64," + Buffer.from(source).toString("base64"));

function setup(polls) {
  const ops = [];
  globalThis.Services = {
    prefs:{getBoolPref:(name, fallback) => name === "fenn.adblock.prototype.enabled" ? true : fallback, getIntPref:(_, f) => f},
    appinfo:{inSafeMode:false},
    obs:{addObserver() {}},
  };
  delays.length = 0;
  let remaining = polls;
  Object.assign(service, {
    work:null, ready:null, shuttingDown:false, observing:true, preparing:false, active:false, error:"",
    stats:{...service.stats, errors:0}, notifyState() {}, openEngine() {}, storeProfile:() => "profile",
    packagedAssets:() => null,
    call(value) { ops.push(value.op); return value.op === "poll" ? {done:--remaining <= 0, accepted:false} : {}; },
  });
  return ops;
}

test("a rejected install still lets the next update run and clears the chain", async () => {
  const ops = setup(1);
  const failed = Promise.reject(new Error("install failed"));
  failed.catch(() => {});
  service.work = failed;
  const ready = service.update();
  await ready;
  assert.deepEqual(ops, ["begin", "poll"]);
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(service.work, null);
});

test("preparation polling backs off from 2 ms to a 16 ms ceiling", async () => {
  setup(7);
  await service.update();
  assert.deepEqual(delays, [2, 4, 8, 16, 16, 16, 16]);
});
