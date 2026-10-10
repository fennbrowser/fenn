import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import vm from "node:vm";
const context = vm.createContext({console, UrlbarShared:{}});
vm.runInContext(readFileSync(new URL("../upstream/firefox/browser/base/content/WinUIUrlbarSession.sys.mjs", import.meta.url), "utf8")
  .replace(/^import .*;$/m, "").replace("export class WinUIUrlbarSession", "this.Session = class WinUIUrlbarSession"), context);
function fixture() {
  const s = Object.create(context.Session.prototype);
  Object.assign(s, {_sequence:0, changed(){}});
  const state = {version:1, enabled:true, session:"1", revision:0, text:"initial", rows:[], open:false};
  return {s, state};
}
test("empty-query publications are positive; replay and independent navigation keep identity", () => {
  const {s,state} = fixture();
  assert.equal(s.preparePublication({...state}).sequence, 1);
  assert.equal(s.preparePublication({...state, navigation:{back:{enabled:true}}, navigationToolbar:true}).sequence, 1);
  assert.equal(s.preparePublication({...state}).sequence, 1);
});
test("selection, open state, completion, labels and modes advance publication, not query structure", () => {
  const {s,state} = fixture();
  s.preparePublication(state);
  for (const change of [{selectedId:"a"}, {open:true}, {complete:true}, {labels:{actions:"Actions"}}, {modes:{generation:1}}, {text:"changed"}]) {
    const before = s.sequence;
    Object.assign(state, change);
    assert.equal(s.preparePublication(state).sequence, before + 1);
    assert.equal(state.revision, 0);
  }
});
test("result sequence and safe-integer exhaustion retain their meaning", () => {
  const {s,state} = fixture();
  s.sequence = 2 ** 32;
  assert.equal(s.preparePublication(state).sequence, 2 ** 32);
  s.sequence++;
  assert.equal(s.preparePublication(state).sequence, s.sequence);
  s.sequence = Number.MAX_SAFE_INTEGER;
  s.preparePublication(state);
  assert.throws(() => s.preparePublication({...state, text:"new"}), /exhausted/);
});
