import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import vm from "node:vm";
import test from "node:test";

const source = readFileSync(new URL("../upstream/firefox/browser/components/urlbar/UrlbarValueFormatter.sys.mjs", import.meta.url), "utf8")
  .replace(/^import .*;$/m, "")
  .replace("export class UrlbarValueFormatter", "this.Formatter = class UrlbarValueFormatter");
function fixture(text, host, base = host) {
  const metadata = {domain:host, origin:host, preDomain:"https://", trimmedLength:8, url:"https://" + text};
  const state = {urlMetaData:{inputValue:text, untrimmedValue:"https://" + text, data:metadata}};
  const lazy = {UrlbarPrefs:{get:()=>true}, SearchService:{isInitialized:true}};
  const input = {value:text, untrimmedValue:"https://" + text, focused:false,
    getBrowserState:()=>state, window:{gBrowser:{selectedBrowser:{}},addEventListener(){}}};
  const context = vm.createContext({XPCOMUtils:{declareLazy:()=>lazy}, Services:{eTLD:{getBaseDomainFromHost:()=>base}}});
  vm.runInContext(source, context);
  return {formatter:new context.Formatter(input), input, state, lazy, metadata};
}
test("Firefox base-domain offsets dim subdomains, ports, paths and queries", () => {
  for (const [text,host,base] of [
    ["www.youtube.com/watch?v=abc", "www.youtube.com", "youtube.com"],
    ["a.example.co.uk:8443/path?q=a#b", "a.example.co.uk", "example.co.uk"],
    ["localhost:8080/path", "localhost", "localhost"],
    ["127.0.0.1/path", "127.0.0.1", "127.0.0.1"],
    ["[::1]:8080/", "[::1]", "[::1]"],
    ["www.bücher.de/path", "www.bücher.de", "bücher.de"],
  ]) {
    const {formatter} = fixture(text, host, base);
    const result = formatter.nativeDomainEmphasis();
    assert.equal(result.text.slice(result.start, result.end), base);
    assert.equal(result.text.slice(result.end), text.slice(host.length));
  }
});
test("focus, search terms, preference and startup suppress formatting", () => {
  const f = fixture("www.example.com/path", "www.example.com", "example.com");
  f.input.focused = true;
  assert.equal(f.formatter.nativeDomainEmphasis(), null);
  f.input.focused = false; f.state.searchTerms = "example";
  assert.equal(f.formatter.nativeDomainEmphasis(), null);
  f.state.searchTerms = ""; f.lazy.UrlbarPrefs.get = () => false;
  assert.equal(f.formatter.nativeDomainEmphasis(), null);
  f.lazy.UrlbarPrefs.get = () => true; f.lazy.SearchService.isInitialized = false;
  assert.equal(f.formatter.nativeDomainEmphasis(), null);
});
test("untrimmed scheme and credentials remain secondary", () => {
  const text = "https://user:pass@www.example.com/path";
  const f = fixture(text, "www.example.com", "example.com");
  f.metadata.preDomain = "https://user:pass@";
  f.metadata.trimmedLength = 0;
  const result = f.formatter.nativeDomainEmphasis();
  assert.equal(text.slice(result.start, result.end), "example.com");
});
