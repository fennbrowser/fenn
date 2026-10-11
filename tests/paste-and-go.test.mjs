import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {executeUrlbarPasteAndGo} from '../upstream/firefox/browser/components/urlbar/content/UrlbarClipboardText.mjs';

test('Paste and Go preserves ordering and resumes after each synchronous failure',()=>{
  const steps=['suppressQuery','paste','clearResult','submit','clearQueryCache','resumeQuery'];
  for(const failure of [null,'paste','clearResult','submit','clearQueryCache']) {
    const called=[];const commands=Object.fromEntries(steps.map(step=>[step,()=>{called.push(step);if(step===failure) throw Error(step);} ]));
    if(failure) assert.throws(()=>executeUrlbarPasteAndGo(commands),new RegExp(failure));
    else executeUrlbarPasteAndGo(commands);
    assert.deepEqual(called,failure ? [...steps.slice(0,steps.indexOf(failure)+1),'resumeQuery'] : steps);
  }
});

test('submission remains fire-and-forget while cache clearing and resume stay synchronous',()=>{
  const called=[];executeUrlbarPasteAndGo({suppressQuery(){},paste(){},clearResult(){},
    submit(){called.push('submit');return new Promise(()=>{});},
    clearQueryCache(){called.push('cache');},resumeQuery(){called.push('resume');}});
  assert.deepEqual(called,['submit','cache','resume']);
});

function binding(name,smartbar=false) {
  const source=readFileSync(new URL('../upstream/firefox/browser/components/urlbar/content/'+name,import.meta.url),'utf8');
  const method=name=>{const start=source.indexOf('  '+name+'(');assert.notEqual(start,-1);return source.slice(start,source.indexOf('\n  }\n',start)+4);};
  const trace=[];
  const context=vm.createContext({executeUrlbarPasteAndGo,Services:{clipboard:{hasDataMatchingFlavors:()=>true}},Ci:{nsIClipboard:{kGlobalClipboard:1}}});
  vm.runInContext('this.Binding=class {#isSmartbarMode='+smartbar+';#readClipboardData(){return {getData:()=>"clipboard"};}'+
    method('pasteAndGo')+method('canPasteAndGo')+(name==='SmartbarInput.mjs'?method('#pasteForPasteAndGo'):'')+'}',context);
  const b=new context.Binding();const controller={isCommandEnabled:()=>true,doCommand:c=>trace.push(c)};
  Object.assign(b,{inputField:{value:'text',setSelectionRange:(a,z)=>trace.push([a,z]),controllers:{getControllerForCommand:()=>controller}},
    document:{commandDispatcher:{getControllerForCommand:()=>controller}},view:{nativePresentation:null},
    window:{goDoCommand:c=>trace.push(c)},select:()=>trace.push('select'),
    setResultForCurrentValue:v=>trace.push(['result',v]),handleCommand:()=>trace.push('submit'),
    parentController:{clearLastQueryContextCache:()=>trace.push('cache')},
    suppressStartQuery(){this._suppressStartQuery=true;},unsuppressStartQuery(){this._suppressStartQuery=false;}});
  return {b,trace,controller};
}

test('original and native bindings retain exact trusted paste route and query cleanup',()=>{
  for(const native of [false,true]) {
    const {b,trace}=binding('UrlbarInputBase.mjs');b.view.nativePresentation=native?{}:null;
    assert.equal(b.canPasteAndGo(),true);b.pasteAndGo();
    assert.deepEqual(trace,[native?[0,4]:'select','cmd_paste',['result',null],'submit','cache']);
    assert.equal(b._suppressStartQuery,false);
    b.handleCommand=()=>{throw Error('submit failed');};assert.throws(()=>b.pasteAndGo(),/submit failed/);
    assert.equal(b._suppressStartQuery,false);
  }
});

test('Smartbar retains its clipboard route and permanent query suppression',()=>{
  for(const smart of [false,true]) for(const permanent of [false,true]) {
    const {b,trace}=binding('SmartbarInput.mjs',smart);b._permanentlySuppressStartQuery=permanent;
    assert.equal(b.canPasteAndGo(),true);b.pasteAndGo();
    assert.deepEqual(trace,['select',...(smart?[]:['cmd_paste']),['result',null],'submit','cache']);
    if(smart) assert.equal(b.value,'clipboard');
    assert.equal(b._suppressStartQuery,permanent);
    b.handleCommand=()=>{throw Error('submit failed');};assert.throws(()=>b.pasteAndGo(),/submit failed/);
    assert.equal(b._suppressStartQuery,permanent);
  }
});
