import assert from "node:assert/strict";
import { test } from "node:test";
import { connectFirefoxStartup } from "../upstream/firefox/browser/base/content/WinUIStartupTransfer.sys.mjs";

test("normal strip metadata is reseeded only after acknowledged presenter retirement", async () => {
  const saved = { Services: globalThis.Services, ChromeUtils: globalThis.ChromeUtils, Ci: globalThis.Ci };
  let observer, nextToken = 0;
  globalThis.Services = {
    obs: { addObserver(value) { observer = value; } },
    dirsvc: { get: () => ({ path: "fixture-profile" }) },
    tm: { dispatchToMainThread: queueMicrotask },
  };
  globalThis.ChromeUtils = { importESModule: () => ({ PrivateBrowsingUtils: { isWindowPrivate: () => false } }) };
  globalThis.Ci = { nsIFile: {} };
  try {
    for (const accepted of [false, true]) {
      const identity = { context: "42", session: "43", generation: "44" };
      const tab = {}, publications = [];
      let retirementReply;
      const pane = {
        tabIds: new Map([[tab, 90]]), visibleTabs: () => [tab],
        syncTabs() { publications.push("tabs"); },
        publishStrip(force) { publications.push(["strip", force]); },
        publish(force) { publications.push(["publish", force]); },
        syncAddress() {},
      };
      const window = { gWinUIPane: pane, gBrowser: { selectedTab: tab, tabs: [tab] }, addEventListener() {}, windowUtils: {
        transferWinUIStartup(input) {
          const request = JSON.parse(input), token = String(++nextToken);
          const result = { ...identity, kind: 0, mappings: 0, editorBrowser: "90", lost: false,
            rows: [[1, "0", "90", true]], normal: accepted };
          const reply = () => observer.observe(null, "winui-startup-reply", `${token}:${JSON.stringify(result)}`);
          if (request.operation === 18) retirementReply = reply;
          else queueMicrotask(reply);
          return token;
        },
      } };
      const connection = await connectFirefoxStartup(window, identity.context, identity.generation);
      await connection.attachPresenter();
      await connection.enableNormalTabs();
      publications.length = 0;
      const pending = connection.publishRows(true);
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(typeof retirementReply, "function");
      assert.deepEqual(publications, [], "pending retirement must retain startup presentation");
      retirementReply();
      await pending;
      assert.deepEqual(publications, accepted ? [["strip", true], "tabs", ["publish", true]] : []);
      assert.equal(connection.stopped, accepted);
      assert.equal(pane.startupConnection, accepted ? null : connection);
      connection.disconnect();
      await new Promise(resolve => setImmediate(resolve));
    }
  } finally {
    for (const [key, value] of Object.entries(saved)) {
      if (value === undefined) delete globalThis[key]; else globalThis[key] = value;
    }
  }
});
