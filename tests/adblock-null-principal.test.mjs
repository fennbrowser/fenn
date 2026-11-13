import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

const TYPE = {TYPE_SCRIPT:2, TYPE_IMAGE:3, TYPE_IMAGESET:21, TYPE_STYLESHEET:4, TYPE_SUBDOCUMENT:7, TYPE_FONT:14,
  TYPE_MEDIA:15, TYPE_XMLHTTPREQUEST:11, TYPE_FETCH:20, TYPE_PING:10, TYPE_WEBSOCKET:16, TYPE_OBJECT:5,
  TYPE_DOCUMENT:6, TYPE_INTERNAL_XMLHTTPREQUEST_ASYNC:51};
globalThis.Ci = {nsIContentPolicy:TYPE, nsIHttpChannel:{}, nsIPermissionManager:{ALLOW_ACTION:1}};
globalThis.ChromeUtils = {generateQI:() => () => {}, now:() => 0};
globalThis.Components = {isSuccessCode:() => true};
let source = await readFile(new URL("../upstream/firefox/browser/components/fennadblock/FennAdblockService.sys.mjs", import.meta.url), "utf8");
source = source.replace(/^import\b[^;]*;$/gm, "const setTimeout=()=>0, clearTimeout=()=>{};");
const {FennAdblockService: service} = await import("data:text/javascript;base64," + Buffer.from(source).toString("base64"));

const uri = (spec, host = new URL(spec).hostname) => ({spec, asciiHost:host, schemeIs:s => spec.startsWith(s + ":")});
const content = (spec, suffix = "") => ({isContentPrincipal:true, isNullPrincipal:false, URI:uri(spec), originSuffix:suffix});
const nullPrincipal = precursor => ({isContentPrincipal:false, isNullPrincipal:true, precursorPrincipal:precursor,
  URI:uri("moz-nullprincipal:{00000000-0000-0000-0000-000000000000}", "")});

function setup({excepted = new Set()} = {}) {
  const calls = [];
  globalThis.Services = {
    prefs:{getBoolPref:(_, fallback) => fallback},
    perms:{testExactPermissionFromPrincipal:(principal, type) =>
      excepted.has(principal.URI.asciiHost + "|" + type) ? 1 : 0},
  };
  globalThis.Cc = {"@mozilla.org/thirdpartyutil;1":{getService:() => ({
    isThirdPartyURI:(a, b) => a.asciiHost !== b.asciiHost})}};
  Object.assign(service, {active:true, stats:{...service.stats, checks:0, blocked:0, errors:0, matchMs:0}, error:"",
    call:value => { calls.push(value); return {blocked:true}; }});
  return calls;
}

function channel(loadingPrincipal, top) {
  let cancelled = null;
  const context = {top:{currentWindowGlobal:{documentPrincipal:top}}};
  return {
    QueryInterface() { return this; },
    loadInfo:{externalContentPolicyType:TYPE.TYPE_SCRIPT, internalContentPolicyType:TYPE.TYPE_SCRIPT,
      loadingPrincipal, triggeringPrincipal:loadingPrincipal, browsingContext:context, originAttributes:{}},
    URI:uri("https://ads.example/ad.js"), status:0, requestMethod:"GET",
    cancel(code) { cancelled = code; }, get cancelled() { return cancelled; },
  };
}

test("subresources of sandboxed frames are checked against their creator", () => {
  const calls = setup();
  globalThis.Cr = {NS_ERROR_BLOCKED_BY_POLICY:"blocked"};
  const publisher = content("https://publisher.example/");
  const request = channel(nullPrincipal(publisher), publisher);
  service.observe(request, "http-on-modify-request");
  assert.equal(calls.length, 1);
  assert.equal(calls[0].source, "publisher.example");
  assert.equal(calls[0].thirdParty, true);
  assert.equal(request.cancelled, "blocked");
});

test("sandboxed top documents still match and honor the creator's site exception", () => {
  const publisher = content("https://publisher.example/");
  let calls = setup();
  service.observe(channel(nullPrincipal(publisher), nullPrincipal(publisher)), "http-on-modify-request");
  assert.equal(calls.length, 1);
  calls = setup({excepted:new Set(["publisher.example|fenn-adblock-site"])});
  service.observe(channel(nullPrincipal(publisher), nullPrincipal(publisher)), "http-on-modify-request");
  assert.equal(calls.length, 0);
});

test("null principals without a content precursor stay unfiltered", () => {
  for (const precursor of [null, {isContentPrincipal:false, isNullPrincipal:false}]) {
    const calls = setup();
    service.observe(channel(nullPrincipal(precursor), content("https://publisher.example/")), "http-on-modify-request");
    assert.equal(calls.length, 0);
  }
});

test("cosmetic and generic lookups use the precursor origin", () => {
  const calls = setup();
  const publisher = content("https://publisher.example/");
  service.resources(nullPrincipal(publisher), publisher);
  service.generic(nullPrincipal(publisher), nullPrincipal(publisher), {generation:service.generation, classes:["ad"], ids:[]});
  assert.deepEqual(calls.map(c => [c.op, c.url]), [["cosmetic", "https://publisher.example/"], ["generic", "https://publisher.example/"]]);
  assert.equal(service.resources(nullPrincipal(null), publisher), null);
});
