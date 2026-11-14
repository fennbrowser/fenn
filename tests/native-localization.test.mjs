import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { runInNewContext } from "node:vm";

// Native menu construction is asynchronous: a removed Fluent message can
// reject the snapshot and leave the native search menu unavailable. Check
// against the pinned Firefox resource, rather than hard-coding message IDs.
const source = readFileSync(new URL(
  "../upstream/firefox/browser/components/urlbar/content/UrlbarSearchModeModel.mjs",
  import.meta.url), "utf8");
const resource = readFileSync(new URL(
  "../upstream/firefox/browser/locales/en-US/browser/browser.ftl",
  import.meta.url), "utf8");

test("native startup labels resolve against the pinned Fluent messages and attributes", () => {
  const bundle = ["browser.ftl", "tabbrowser.ftl", "browserContext.ftl", "toolbarContextMenu.ftl", "tabContextMenu.ftl"]
    .map(file => readFileSync(new URL("../upstream/firefox/browser/locales/en-US/browser/" + file, import.meta.url), "utf8"))
    .join("\n");
  const messages = new Map([...bundle.matchAll(
    /^([A-Za-z][A-Za-z0-9_-]*)\s*=([^\n]*(?:\n[ \t]+[^\n]*)*)/gm)]
    .map(([, id, body]) => [id, body]));
  const body = id => {
    assert(messages.has(id), `Native startup message was removed upstream: ${id}`);
    return messages.get(id);
  };
  const context = { window: { addEventListener() {} } };
  runInNewContext(readFileSync(new URL(
    "../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8"), context);
  context.gWinUIPane.localization = () => ({
    formatValueSync(id) { body(id); return id; },
    formatMessagesSync(requests) {
      return requests.map(({ id }) => ({ attributes: [...body(id).matchAll(/^[ \t]+\.([A-Za-z-]+)\s*=/gm)]
        .map(([, name]) => ({ name, value: `${id}.${name}` })) }));
    },
  });
  const labels = context.gWinUIPane.nativeLabels();
  const toolbox = readFileSync(new URL(
    "../upstream/firefox/browser/base/content/navigator-toolbox.inc.xhtml", import.meta.url), "utf8");
  const stockId = toolbox.match(/<toolbarbutton\b[^>]*\bid="alltabs-button"[^>]*\bdata-l10n-id="([^"]+)"/s)?.[1];
  assert(stockId, "stock All Tabs control declares its localized label");
  assert.equal(labels.allTabs.label, `${stockId}.label`);
  assert.equal(labels.allTabs.tooltip, `${stockId}.tooltiptext`);
  for (const item of Object.values(labels.navigation)) assert(item.label);
});

test("native search-mode snapshot only requests messages present in pinned Firefox", () => {
  const start = source.indexOf("async snapshot()");
  const end = source.length;
  assert(start >= 0 && end > start, "locate the native snapshot boundary");
  const ids = [...source.slice(start, end).matchAll(/"(urlbar-[a-z0-9-]+)"/g)]
    .map(match => match[1]);
  assert(ids.length > 0, "snapshot has localized labels");
  const available = new Set([...resource.matchAll(/^([a-zA-Z][a-zA-Z0-9_-]*)\s*=/gm)]
    .map(match => match[1]));
  for (const id of ids) {
    assert(available.has(id), `Native search-mode message was removed upstream: ${id}`);
  }
});

test("synthetic native action labels exist in pinned Firefox", () => {
  const fixture = readFileSync(new URL("../scripts/Test-FennPane.py", import.meta.url), "utf8");
  const ids = [...fixture.matchAll(/label:\s*"(urlbar-[a-z0-9-]+)"/g)]
    .map(match => match[1]);
  assert(ids.length > 0, "integration fixture supplies a localized action");
  const available = new Set([...resource.matchAll(/^([a-zA-Z][a-zA-Z0-9_-]*)\s*=/gm)]
    .map(match => match[1]));
  for (const id of ids) {
    assert(available.has(id), `Native action fixture message was removed upstream: ${id}`);
  }
});

test("native container-menu labels exist with the attributes Firefox consumes", () => {
  const pane = readFileSync(new URL(
    "../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8");
  const containers = readFileSync(new URL(
    "../upstream/firefox/toolkit/locales/en-US/toolkit/global/contextual-identity.ftl",
    import.meta.url), "utf8");
  const ids = [...pane.matchAll(/label\("(user-context-[a-z0-9-]+)"\)/g)]
    .map(match => match[1]);
  assert(ids.length > 0, "native container menu uses Firefox's contextual labels");
  for (const id of ids) {
    const message = containers.match(new RegExp(`^${id} =[^\\n]*(?:\\n[ \t]+[^\\n]*)*`, "m"));
    assert(message, `Native container message was removed upstream: ${id}`);
    assert.match(message[0], /\.label\s*=/, `${id} must supply the label attribute`);
  }
});


test("native customization loads Firefox's current flexible-space resource", () => {
  const context = { window: { addEventListener() {} }, Localization: class {
    constructor(resources) { this.resources = resources; }
  } };
  runInNewContext(readFileSync(new URL(
    "../upstream/firefox/browser/base/content/browser-winui-pane.js", import.meta.url), "utf8"), context);
  assert(context.gWinUIPane.localization().resources.includes("browser/customizeMode.ftl"));
});
