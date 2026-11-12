import test from "node:test";
import assert from "node:assert/strict";
import {FennAdblockDiagnosticCapture as Capture} from "../upstream/firefox/browser/components/fennadblock/FennAdblockDiagnostics.sys.mjs";

test("capture bounds records and serialized export under request flood", () => {
  const c = new Capture();
  for (let i = 0; i < 10000; i++) { c.record("xmlhttprequest", "blocked-unsupported-replacement", true, true); }
  const r = c.report(null);
  assert.equal(r.events.length, 256); assert.equal(r.dropped, 9744);
  assert.ok(JSON.stringify(r).length < 65536);
});
test("schema discards addresses, rule strings and unknown diagnostic fields", () => {
  const c = new Capture(); c.record("https://secret.test/path?token=secret", "allowed");
  c.record("script", "secret-rule");
  const r = c.report({styleCounts:{applied:2,secret:"password",rejected:-1},
    domCounts:{exhausted:true,exhaustionReason:"scan-work",checks:Infinity,hidden:4,lastScan:{url:"secret"}}});
  assert.equal(r.events.length, 1); assert.equal(r.events[0].kind, "other");
  assert.deepEqual(r.cosmetics.style, {applied:2});
  assert.deepEqual(r.cosmetics.dom, {hidden:4,exhausted:true,exhaustionReason:"scan-work"});
  assert.ok(!JSON.stringify(r).includes("secret"));
});
test("stop erases retained records and cannot be reversed by late callbacks", () => {
  const c = new Capture(); c.record("script", "blocked"); const held = c.events;
  c.clear(); c.record("script", "blocked");
  assert.equal(held.length, 0); assert.equal(c.dropped, 0); assert.equal(c.report(null), null);
});
test("export is detached and exception/replacement decisions stay distinct", () => {
  const c = new Capture(); c.record("script", "allowed", true); c.record("script", "replaced");
  const a = c.report(null, "timeout"); a.events[0].outcome = "blocked";
  const b = c.report(null); assert.equal(b.events[0].outcome, "allowed");
  assert.equal(b.events[0].exception, true); assert.equal(b.events[1].outcome, "replaced");
  assert.equal(a.cosmetics.status, "timeout");
});
