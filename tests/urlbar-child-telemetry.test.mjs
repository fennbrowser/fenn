import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";

let source = await readFile(new URL("../upstream/firefox/browser/components/urlbar/content/UrlbarChildTelemetry.mjs", import.meta.url), "utf8");
source = source.replace(/^import\b[^;]*;$/gm, "const UrlbarTelemetryUtils = new Proxy({}, {get: (_, key) => globalThis.fixtureTelemetryUtils[key]});");
const {UrlbarChildTelemetry} = await import("data:text/javascript;base64," + Buffer.from(source).toString("base64"));

function setup() {
  const recorded = [], errors = [];
  globalThis.console = {...console, error: (...args) => errors.push(args.map(String).join(" "))};
  globalThis.fixtureTelemetryUtils = {
    collectSnapshot: () => ({method: "engagement", internalDetails: {searchSource: "urlbar"}}),
    engagementData: () => ({visibleResults: []}),
    smartbarData: () => ({}),
    buildRecordedEngagement: () => ({built: {}, previousSearchWords: ""}),
    recordedEngagementToWire: value => value,
  };
  const controller = {input: {}, view: {queryContext: {}},
    parentController: {recordEngagement: value => recorded.push(value)}};
  return {telemetry: new UrlbarChildTelemetry(controller), recorded, errors};
}

test("a session-ending engagement reaches the parent recorder with resolved exposures", () => {
  const {telemetry, recorded, errors} = setup();
  telemetry.record({type: "keydown"}, {isSessionOngoing: false});
  assert.deepEqual(errors, []);
  assert.equal(recorded.length, 1);
  assert.deepEqual(recorded[0].exposures, []);
});

test("an ongoing session records without resolving exposures", () => {
  const {telemetry, recorded, errors} = setup();
  telemetry.record({type: "keydown"}, {isSessionOngoing: true});
  assert.deepEqual(errors, []);
  assert.equal(recorded.length, 1);
  assert.equal(recorded[0].exposures, null);
});
