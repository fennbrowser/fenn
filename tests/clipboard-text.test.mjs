import assert from 'node:assert/strict';
import test from 'node:test';
import {formatUrlbarClipboardText as format} from '../upstream/firefox/browser/components/urlbar/content/UrlbarClipboardText.mjs';

function fixture() {
  const state={selectedValue:'example.test',value:'example.test',rangeCount:1,
    selectionStart:0,pageProxyValid:true,decodeURLsOnCopy:false};
  const uri={displaySpec:'https://www.example.test/',schemeIs:scheme=>scheme==='https'};
  const services={getPageURI:()=>uri,parseURI:()=>uri,makeURIReadable:value=>value,
    getTrimmedURLPrefix:()=> 'https://www.',canParse:value=>URL.canParse(value)};
  return {state,uri,services};
}

test('plain typed, partial-domain, non-prefix and multi-range selections skip URI services',()=>{
  for(const patch of [{rangeCount:2},{selectionStart:1},{selectedValue:''},
    {valueIsTyped:true},{selectedValue:'example',value:'example.test/path'}]) {
    const {state}=fixture();Object.assign(state,patch);
    const services=new Proxy({}, {get(){throw new Error('URI resolution should not run');}});
    assert.equal(format(state,services),state.selectedValue);
  }
});

test('full loaded URL uses Firefox readable URI, including a loading-target URI',()=>{
  const {state,services,uri}=fixture();
  assert.equal(format(state,services),uri.displaySpec);
  services.getPageURI=()=>({displaySpec:'reader source'});
  services.makeURIReadable=value=>{assert.equal(value.displaySpec,'reader source');return uri;};
  assert.equal(format(state,services),uri.displaySpec);
});

test('autofill copies the provider URL before URI parsing',()=>{
  const {state,services}=fixture();
  state.pageProxyValid=false;state.resultForCurrentValue={autofill:{value:state.selectedValue},payload:{url:'http://example.test/full'}};
  services.parseURI=()=>{throw new Error('Autofill already resolved the URL');};
  assert.equal(format(state,services),'http://example.test/full');
});

test('non-URL result selections stay as text and confirmed URLs use untrimmed state',()=>{
  const {state,services,uri}=fixture();state.pageProxyValid=false;state.untrimmedValue='https://example.test/';
  services.parseURI=value=>{assert.equal(value,state.untrimmedValue);return null;};
  assert.equal(format(state,services),state.selectedValue);
  services.parseURI=()=>uri;assert.equal(format(state,services),uri.displaySpec);
});

test('prefix restoration preserves partial selection and encodes decoded URL characters',()=>{
  const {state,services}=fixture();state.value='example.test/one two/rest';state.selectedValue='example.test/one two';
  assert.equal(format(state,services),'https://www.example.test/one%20two');
  state.decodeURLsOnCopy=true;
  assert.equal(format(state,services),'https://www.example.test/one two');
  state.selectedValue='https://www.example.test/one two';
  assert.equal(format(state,services),state.selectedValue,'never duplicate the trimmed prefix');
});

test('data and javascript readability, malformed escapes and failed parses retain Firefox policy',()=>{
  for(const scheme of ['data','javascript']) {
    const {state,uri,services}=fixture();state.value=state.selectedValue=scheme+':a b';
    uri.displaySpec=scheme+':a%20b';uri.schemeIs=value=>value===scheme;
    services.getTrimmedURLPrefix=()=>'';
    assert.equal(format(state,services),scheme==='data' ? 'data:a b' : 'javascript:a%20b');
  }
  const {state,services}=fixture();state.value='example.test/other';state.selectedValue='example.test/\ud800';
  assert.equal(format(state,services),'https://www.example.test/\ud800');
  services.canParse=()=>false;state.selectedValue='example.test/a b';
  assert.equal(format(state,services),'https://www.example.test/a b');
});

test('trim flags still restore prefixes for values marked typed by Firefox',()=>{
  for(const flag of ['protocolIsTrimmed','wwwIsTrimmed']) {
    const {state,services,uri}=fixture();state.valueIsTyped=true;state[flag]=true;
    assert.equal(format(state,services),uri.displaySpec);
  }
});
