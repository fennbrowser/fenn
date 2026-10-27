import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const tabs=[{linkedBrowser:{}},{linkedBrowser:{}}];
class Switcher {
  requestTab(tab) { this.requestedTab=tab; return 17; }
  onLayersReady() { throw new Error('original failure'); }
  updateDisplay() { this.visibleTab=this.requestedTab; this.switchPaintId=42; }
  notePaint(event) { if(event.transactionId>=this.switchPaintId)this.switchPaintId=-1; }
}
const original=Switcher.prototype.notePaint;
const listeners=new Map();
const surface={addEventListener:(name,fn)=>listeners.set(name,fn),removeEventListener:name=>listeners.delete(name)};
const window={...surface,windowUtils:{lastTransactionId:41,isMozAfterPaintPending:true}};
const context={window,gWinUIPane:{},browsingTabs:tabs,
  gBrowser:{selectedTab:tabs[0],tabContainer:surface},
  ChromeUtils:{importESModule:()=>({AsyncTabSwitcher:Switcher})},performance};
const source=fs.readFileSync(new URL('../scripts/Profile-DesktopLatency.js',import.meta.url),'utf8');
const admission=vm.runInNewContext(`(function(){${source}})('sparse')`,context);
assert.equal(admission.missing.length,0);
const switcher=new Switcher();
assert.equal(switcher.requestTab(tabs[0]),17);
switcher.updateDisplay();
assert.throws(()=>switcher.onLayersReady(),/original failure/);
switcher.notePaint({transactionId:43});
const probe=window.__desktopProbe;
const ack=probe.spans.find(s=>s.name==='switcher.notePaint');
assert.equal(ack.paintBefore,42);assert.equal(ack.paintAfter,-1);assert.equal(ack.transaction,43);
assert.equal(probe.depth,0);
assert.ok(probe.spans.every(s=>s.name.startsWith('switcher.')));
probe.restore();assert.equal(Switcher.prototype.notePaint,original);assert.equal(listeners.size,0);
console.log('PASS sparse wrappers preserve results, exceptions, transaction identity and restoration');
