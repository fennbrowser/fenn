import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const root=new URL('../upstream/firefox/browser/components/urlbar/content/',import.meta.url);
const prefs={'maxRichResults':10,'smartbar.maxResults':5,'maxCharsForSearchSuggestions':8,
  'unifiedSearchButton.historyInSearchMode':false};
const sandbox=vm.createContext({UrlbarPrefs:{get:key=>prefs[key]},
  UrlbarShared:{RESULT_SOURCE:{ACTIONS:7},isPasteEvent:e=>e.inputType?.startsWith('insertFromPaste') || e.inputType==='insertFromYank'},
  UrlbarQueryContext:class {constructor(options){Object.assign(this,options);}}});
vm.runInContext(readFileSync(new URL('UrlbarQueryModel.mjs',root),'utf8').replace(/^import .*;$/gm,'')
  .replace('export class UrlbarQueryModel','this.Model = class UrlbarQueryModel'),sandbox);
const state={isPrivate:false,sapName:'urlbar'};
const create=(s={},request={searchString:'query'})=>sandbox.Model.create({...state,...s},request);

test('Actions limit takes precedence; Smartbar layout and sponsored policy remain independent',()=>{
  assert.equal(create().maxResults,10);
  assert.equal(create({smartbar:true,sapName:'searchbar'}).maxResults,5);
  assert.equal(create({smartbar:true,searchMode:{source:7}}).maxResults,99);
  assert.equal(create({sapName:'smartbar',excludeSponsoredResults:true}).excludeSponsoredResults,true);
  assert.equal(create({sapName:'smartbar'}).maxResults,10,'SAP alone does not select the layout limit');
  assert.equal('excludeSponsoredResults' in create(),false);
});

test('only a paste exceeding the live threshold prohibits remote results',()=>{
  for(const [isPaste,pasteLength,expected] of [[true,9,true],[true,8,false],[false,99,false],
    [true,undefined,false],[true,0,false]]) {
    assert.equal(create({}, {searchString:'query',isPaste,pasteLength}).prohibitRemoteResults,expected);
  }
});

test('browser facts and search-mode identity survive without a window or element',()=>{
  const mode={source:2},browser={userContextId:4,tabGroup:'group',currentPage:'https://example.test/'};
  const q=create({isPrivate:true,searchMode:mode,browser},{searchString:'',allowAutofill:false});
  assert.equal(q.searchMode,mode);assert.deepEqual([...q.sources],[2]);
  assert.equal(q.userContextId,4);assert.equal(q.tabGroup,'group');assert.equal(q.currentPage,browser.currentPage);
  assert.equal(q.isPrivate,true);assert.equal(q.allowAutofill,false);assert.equal(q.searchString,'');
  prefs['unifiedSearchButton.historyInSearchMode']=true;
  try {assert.equal('sources' in create({searchMode:mode}),false);}
  finally {prefs['unifiedSearchButton.historyInSearchMode']=false;}
  for(const currentPage of [undefined,null,'']) assert.equal('currentPage' in create({browser:{currentPage}}),false);
  assert.equal('userContextId' in create(),false);
  assert.equal('sources' in create({searchMode:{engineName:'Engine'}}),false);
});

test('both actual retained bindings capture browser facts and paste UTF-16 length for the shared model',()=>{
  sandbox.UrlbarQueryModel=sandbox.Model;
  for(const name of ['UrlbarInputBase.mjs','SmartbarInput.mjs']) {
    const source=readFileSync(new URL(name,root),'utf8');
    const start=source.indexOf('  #makeQueryContext('),end=source.indexOf('\n  }\n',start)+4;
    vm.runInContext('this.Binding=class {#isSmartbarMode=true;'+source.slice(start,end)+
      ' make(options){return this.#makeQueryContext(options);}}',sandbox);
    const input=new sandbox.Binding();
    Object.assign(input,{isPrivate:false,sapName:name==='SmartbarInput.mjs'?'smartbar':'urlbar',searchMode:null,
      window:{gBrowser:{selectedBrowser:{getAttribute:()=> '6'},selectedTab:{group:{id:'g'}},currentURI:null}}});
    const q=input.make({searchString:'query',event:{inputType:'insertFromPaste',data:'😀😀😀😀x'}});
    assert.equal(q.userContextId,6);assert.equal(q.tabGroup,'g');assert.equal('currentPage' in q,false);
    assert.equal(q.prohibitRemoteResults,true);assert.equal(q.maxResults,name==='SmartbarInput.mjs'?5:10);
    assert.equal(input.make({searchString:'query',event:{inputType:'insertText',get data(){throw Error('Non-paste data read');}}}).prohibitRemoteResults,false);
    input.window={};assert.equal('userContextId' in input.make({searchString:'query'}),false);
  }
});


test('query text preserves valid-page zero-prefix and invalid-page restoration policy',()=>{
  const resolve=(value,valid,requested)=>sandbox.Model.resolveSearchString({value,pageProxyValid:valid},requested);
  for (const request of [undefined,null,'']) {
    assert.equal(resolve('https://example.test/',true,request),'');
    assert.equal(resolve('restored words',false,request),'restored words');
  }
  assert.equal(resolve('prefix suffix',true,'prefix'),'prefix');
  assert.throws(()=>resolve('different',false,'prefix'),/doesn't start/);
});
