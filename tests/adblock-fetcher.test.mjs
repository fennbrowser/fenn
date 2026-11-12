import test from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {PRODUCTION_UPDATE_URL} from "../upstream/firefox/browser/components/fennadblock/FennAdblockUpdates.sys.mjs";
// Use the actual transport implementation with only its Firefox adapters replaced.
let source=await readFile(new URL("../upstream/firefox/browser/components/fennadblock/FennAdblockFetcher.sys.mjs",import.meta.url),"utf8");
source=source.replace(/^import .*;$/gm,"");
const {FennAdblockFetcher}=await import("data:text/javascript;base64,"+Buffer.from(`const PRODUCTION_UPDATE_URL=${JSON.stringify(PRODUCTION_UPDATE_URL)};\n`+source).toString("base64"));
function setup(response,request={id:1,path:"metadata/timestamp.json",limit:65536}) {
  const calls=[],writes=[];
  globalThis.ChromeUtils={now:()=>0};globalThis.PathUtils={join:(...p)=>p.join("/")};
  globalThis.IOUtils={exists:async()=>true,read:async()=>new TextEncoder().encode(JSON.stringify(request)),
    write:async(...v)=>writes.push(v),writeJSON:async(...v)=>writes.push(v)};
  globalThis.fetch=async(...args)=>{calls.push(args);return response();};
  const f=new FennAdblockFetcher(PRODUCTION_UPDATE_URL,()=>true);f.path="fixture";
  return {f,calls,writes};
}
test("only the approved HTTPS feed is accepted outside explicit loopback tests",()=>{
  globalThis.ChromeUtils={now:()=>0};
  for(const url of ["http://127.0.0.1:123/","https://example.com/","https://updates-test.fennbrowser.com/adblock/trial-v1/","https://updates.fennbrowser.com/adblock/stable-v1/?x=1"]){assert.throws(()=>new FennAdblockFetcher(url,()=>true));}
  assert.doesNotThrow(()=>new FennAdblockFetcher("http://127.0.0.1:123/",()=>true,{allowLoopback:true}));
});
test("transport omits credentials and rejects redirects while writing bounded replies",async()=>{
  const {f,calls,writes}=setup(()=>new Response("{}",{status:200}));await f.pump();
  assert.equal(calls.length,1);assert.equal(calls[0][1].credentials,"omit");assert.equal(calls[0][1].redirect,"error");assert.equal(calls[0][1].referrerPolicy,"no-referrer");
  assert.equal(writes.at(-1)[1].status,"ok");
});
test("Retry-After escapes transport to the persisted scheduler",async()=>{
  const {f,calls}=setup(()=>new Response("",{status:503,headers:{"Retry-After":"120"}}));
  await assert.rejects(f.pump(),error=>error.retryAfterMs===120000);assert.equal(calls.length,1);
});
test("decoded streaming body bound rejects oversized responses",async()=>{
  const {f,writes}=setup(()=>new Response(new Uint8Array(65537),{status:200}));await f.pump();
  assert.equal(writes.length,1);assert.equal(writes[0][1].status,"error");
});
test("broker traversal is rejected before any network request",async()=>{
  const {f,calls}=setup(()=>new Response("{}"),{id:1,path:"metadata/../timestamp.json",limit:65536});
  await assert.rejects(f.pump());assert.equal(calls.length,0);
});
