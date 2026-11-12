import test from "node:test";
import assert from "node:assert/strict";
import {isSupportedSelectorList} from "../upstream/firefox/browser/components/fennadblock/FennAdblockSelectors.sys.mjs";

function validate(selector, expected) {
  const seen=[];
  const result=isSupportedSelectorList(selector,{CSS:{supports:value=>{seen.push(value);return !value.includes(":unknown");}}});
  assert.deepEqual(seen,expected.map(s=>`selector(${s})`));return result;
}
test("each complete member is delegated to Gecko",()=>{
  assert.equal(validate('.ad, .sponsor > p',['.ad','.sponsor > p']),true);
  assert.equal(validate('.ad, :unknown',['.ad',':unknown']),false);
});
test("attribute, functional, escaped and comment commas do not split members",()=>{
  for(const part of ['[data-label="a,b"]',':is(.a, .b)',':nth-child(2n of .a, .b)',String.raw`.a\,b`,'.a/* , ([ */']) {
    assert.equal(validate(`${part}, .next`,[part,'.next']),true);
  }
});
test("malformed members and unbalanced syntax reject the entire list",()=>{
  const win={CSS:{supports:()=>true}};
  for(const s of ['', ' ', ',.ad', '.ad,', '.ad,,.x', '.ad[', '.ad)', '[x="bad]', '.ad/*oops', '.ad([)]']) {
    assert.equal(isSupportedSelectorList(s,win),false,s);
  }
});
test("size and member bounds apply without truncating into a partial rule",()=>{
  const win={CSS:{supports:()=>true}};
  assert.equal(isSupportedSelectorList('.ad',win,2),false);
  assert.equal(isSupportedSelectorList(Array(128).fill('.a').join(','),win),true);
  assert.equal(isSupportedSelectorList(Array(129).fill('.a').join(','),win),false);
});
