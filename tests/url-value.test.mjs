import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const root=new URL('../upstream/firefox/browser/components/urlbar/content/',import.meta.url);
function fixture() {
  const calls=[];
  const services={getDisplaySpec:spec=>spec,getFixupPrimitives:(value,isPrivate)=>{calls.push({value,isPrivate});return {};}};
  const context=vm.createContext({URL,UrlbarContentUtils:services,UrlbarPrefs:{}});
  vm.runInContext(readFileSync(new URL('UrlbarShared.mjs',root),'utf8').replace(/^import .*;$/gm,'')
    .replace('export const UrlbarShared','var UrlbarShared'),context);
  vm.runInContext(readFileSync(new URL('UrlbarUrlValue.mjs',root),'utf8').replace(/^import .*;$/gm,'')
    .replaceAll('export function ','function '),context);
  return {context,services,calls};
}

test('lossless display preserves encoded delimiters, percent sequences and malformed UTF-8',()=>{
  const {context:c}=fixture();
  for(const suffix of ['%2F%3F%23','%252F','%25zz','%FF','%E0%A4%A']) {
    const value='https://example.test/'+suffix;
    assert.equal(c.losslessDecodeDisplaySpec(value),value);
  }
  assert.equal(c.losslessDecodeDisplaySpec('https://example.test/caf%C3%A9'),'https://example.test/café');
  assert.equal(c.losslessDecodeDisplaySpec('custom:caf%C3%A9%20x'),'custom:caf%C3%A9 x');
});

test('display encoding retains spoofing protections and permits joiners',()=>{
  const {context:c}=fixture();
  for(const escaped of ['%0A','%09','%C2%A0','%E2%80%8B','%E2%80%AE','%EF%BF%BC','%F0%9F%94%92']) {
    assert.equal(c.losslessDecodeDisplaySpec('https://example.test/'+escaped),'https://example.test/'+escaped);
  }
  assert.equal(c.losslessDecodeDisplaySpec('https://example.test/a  b '),'https://example.test/a%20 b%20');
  assert.equal(c.losslessDecodeDisplaySpec('https://example.test/a%E2%80%8Cb%E2%80%8D'),'https://example.test/a\u200cb\u200d');
});

test('URL decode uses browser display-spec service and its null fallback',()=>{
  const {context:c,services}=fixture();
  services.getDisplaySpec=()=> 'https://bücher.test/caf%C3%A9';
  assert.equal(c.losslessDecodeURL(new URL('https://xn--bcher-kva.test/caf%C3%A9')),'https://bücher.test/café');
  services.getDisplaySpec=()=>null;
  assert.equal(c.losslessDecodeURL(new URL('https://example.test/%20x')),'https://example.test/ x');
});

test('result URL rules preserve overrides, schemes, nonheuristics and binding eligibility',()=>{
  const {context:c,calls}=fixture();
  const result={heuristic:true,payload:{url:'http://example.test/path'}};
  const facts={isAddressbar:true,userTypedValue:'example.test/p',isPrivate:true};
  assert.equal(c.resultURLValue(result,facts),'example.test/path');
  assert.deepEqual(calls,[{value:'example.test/path',isPrivate:true}]);
  for(const text of ['http://example.test',' https://example.test','file://example.test']) {
    assert.equal(c.resultURLValue(result,{...facts,userTypedValue:text}),'http://example.test/path');
  }
  assert.equal(c.isSchemelessInput('HTTP://example.test'),true,'preserve Firefox case-sensitive classification');
  assert.equal(c.resultURLValue(result,{...facts,isAddressbar:false}),'http://example.test/path');
  assert.equal(c.resultURLValue({...result,heuristic:false},facts),'http://example.test/path');
  assert.equal(c.resultURLValue({heuristic:true,payload:{url:'https://example.test/path'}},facts),'https://example.test/path');
  assert.equal(c.resultURLValue(result,{...facts,urlOverride:''}),'');
  assert.equal(c.resultURLValue(result,{...facts,urlOverride:'invalid url'}),'');
  assert.equal(c.resultURLValue(result,{...facts,urlOverride:'http://override.test/'}),'http://override.test/');
  assert.equal(c.resultURLValue({payload:{url:'invalid url'}},facts),'');
  assert.equal(calls.length,1);
});

test('fixup search guard and retained decoder adapters preserve browser policy',()=>{
  const {context:c,services,calls}=fixture();
  services.getFixupPrimitives=(text,isPrivate)=>{calls.push({text,isPrivate});return {keywordAsSent:'search'};};
  assert.equal(c.resultURLValue({heuristic:true,payload:{url:'http://unknown/'}},
    {isAddressbar:true,userTypedValue:'unknown',isPrivate:true}),'http://unknown/');
  assert.deepEqual(calls,[{text:'unknown/',isPrivate:true}]);
  let decoded;
  assert.equal(c.resultURLValue({payload:{url:'https://example.test/'}},{},url=>{decoded=url.href;return 'adapter value';}),'adapter value');
  assert.equal(decoded,'https://example.test/');
});
