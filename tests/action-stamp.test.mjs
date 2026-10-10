import test from "node:test";
import assert from "node:assert/strict";
import {readActionStamp,actionStampMatches} from "../upstream/firefox/browser/base/content/WinUIActionStamp.sys.mjs";

test("action stamps preserve safe revisions and optional bounded opening identity",()=>{
  for (const revision of [0,1,Number.MAX_SAFE_INTEGER,"001"]) {
    const stamp=readActionStamp(revision);
    assert.equal(actionStampMatches(stamp,Number(revision)),true);
    assert.equal(actionStampMatches(stamp,Number(revision),1),false);
  }
  const stamp=readActionStamp("1","4294967295",0xffffffff);
  assert.equal(actionStampMatches(stamp,1,0xffffffff),true);
  assert.equal(actionStampMatches(stamp,2,0xffffffff),false);
  assert.equal(actionStampMatches(stamp,1,1),false);
});

test("malformed, unsafe and out-of-range identities never match",()=>{
  for (const value of [undefined,null,true,{},NaN,Infinity,-1,1.5,""," 1","+1","1e2","0x10",Number.MAX_SAFE_INTEGER+1]) {
    assert.equal(readActionStamp(value),null);
    assert.equal(actionStampMatches(readActionStamp(value),1),false);
  }
  for (const session of [0,"0",-1,0x100000000]) {
    assert.equal(readActionStamp(1,session,0xffffffff),null);
  }
  assert.equal(readActionStamp(0x100000000,1,0xffffffff),null);
});
