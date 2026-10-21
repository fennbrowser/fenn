import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const read=name=>readFileSync(new URL('../upstream/firefox/browser/base/content/'+name,import.meta.url),'utf8');
function fixture(){
 const src=read('browser-siteProtections.js');
 const names=[...new Set([...src.matchAll(/\.((?:STATE_)[A-Z_0-9]+)/g)].map(m=>m[1]))];
 const flags=Object.fromEntries(names.map((n,i)=>[n,2**i]));
 const prefs={};const cookie={BEHAVIOR_ACCEPT:0,BEHAVIOR_REJECT:2,BEHAVIOR_REJECT_FOREIGN:1,BEHAVIOR_REJECT_TRACKER:4,BEHAVIOR_PARTITION_FOREIGN:5,BEHAVIOR_LIMIT_FOREIGN:3};
 const context=vm.createContext({console,window:{},document:{createElementNS(){assert.fail('native model constructed DOM');},createDocumentFragment(){assert.fail('native model constructed DOM');}},
  MozXULElement:{insertFTLIfNeeded(){}},BreachAlertsData:class {},
  ChromeUtils:{defineESModuleGetters(){},defineLazyGetter(o,k,get){Object.defineProperty(o,k,{get,configurable:true});}},
  XPCOMUtils:{defineLazyServiceGetter(){},defineLazyPreferenceGetter(o,k,p,value){o[k]=prefs[p]??value;}},
  Ci:{nsIWebProgressListener:flags,nsICookieService:cookie},
  PrivateBrowsingUtils:{isWindowPrivate:()=>false},
  Services:{prefs:{PREF_BOOL:128,getPrefType:()=>0},perms:{ALLOW_ACTION:1,DENY_ACTION:2,UNKNOWN_ACTION:0,testPermissionFromPrincipal:()=>0,testPermission:()=>0},
    io:{newURI:origin=>({host:new URL(origin).hostname})},eTLD:{getBaseDomain:uri=>uri.host}},
  gBrowser:{currentURI:{host:'site.test'},contentPrincipal:{},selectedBrowser:{getContentBlockingLog:()=> '{}'}},
  identifyType:entry=>entry.some(([flag,blocked])=>blocked && (flag & flags.STATE_BLOCKED_FINGERPRINTING_CONTENT))?'fingerprint':null,
  privacyMetricsStatsCategories:{fingerprint:'fingerprinting'},
 });
 vm.runInContext(src+'\n'+read('browser-trustPanel.js')+'\nthis.categories={ProtectionCategory,TrackingProtection,ThirdPartyCookies};',context);
 return {c:context,flags,cookie};
}
test('original blocker origin descriptors preserve blocked, allowed and shim semantics without DOM',()=>{
 const {c,flags}=fixture(),Category=c.categories.ProtectionCategory;
 const category=new Category('test',{prefEnabled:'test'},{block:1,load:2,allow:4,shim:8});
 const rows=category.getTrackerItems({'blocked.test':[[1,true]],'allowed.test':[[2,false]],'shim.test':[[2|8,false]],'explicit.test':[[4,false],[1,true]]});
 assert.deepEqual(Array.from(rows,r=>r.origin),['blocked.test','allowed.test','explicit.test']);assert.equal(rows[1].isAllowed,true);assert.equal(rows[2].shimAllowed,true);assert.ok(flags.STATE_BLOCKED_FINGERPRINTING_CONTENT);
});
test('tracking annotation-level exclusions are shared with the native model',()=>{
 const {c,flags}=fixture(),t=c.categories.TrackingProtection;t.annotationsLevel2Enabled=true;Object.defineProperty(t,'trackingProtectionLevel2Enabled',{value:false});
 assert.equal(t.getTrackerItems({'https://excluded.test':[[flags.STATE_LOADED_LEVEL_2_TRACKING_CONTENT,false],[flags.STATE_BLOCKED_TRACKING_CONTENT,true]]}).length,0);
});
test('trust summary counts blocked origins using the original metrics classifier',()=>{
 const {c,flags}=fixture(),flag=flags.STATE_BLOCKED_FINGERPRINTING_CONTENT;
 const empty=c.gTrustPanelHandler.getConnectionTrackingState({'https://allowed.test':[[flag,false]]},flag);assert.equal(empty.count,0);
 const data=c.gTrustPanelHandler.getConnectionTrackingState({'https://blocked.test':[[flag,true]]},flag);assert.equal(data.count,1);const category=data.categories.find(c=>c.keys.general==='fingerprinter');assert.equal(category.items[0].origin,'https://blocked.test');assert.equal(category.blocking,true);
});
test('summary-only descriptions do not enumerate category origin details',()=>{
 const {c,flags}=fixture();c.categories.ProtectionCategory.prototype.getTrackerItems=()=>assert.fail('summary enumerated origin details');
 const flag=flags.STATE_BLOCKED_FINGERPRINTING_CONTENT,data=c.gTrustPanelHandler.getConnectionTrackingState({'https://blocked.test':[[flag,true]]},flag,false);assert.equal(data.count,1);assert.equal(data.categories.length,0);
});
test('cookie lists use the existing first/third-party and cookie-behavior policy',()=>{
 const {c,cookie}=fixture(),t=c.categories.ThirdPartyCookies;t._processContentBlockingLog=()=>({trackers:[{origin:'tracker'}],firstParty:[{origin:'first'}],thirdParty:[{origin:'third'}]});
 for(const [behavior,origins] of [[cookie.BEHAVIOR_REJECT_TRACKER,['tracker']],[cookie.BEHAVIOR_REJECT_FOREIGN,['tracker','third']],[cookie.BEHAVIOR_REJECT,['tracker','first','third']]]){t.behaviorPref=behavior;assert.deepEqual(Array.from(t.getTrackerItems({}),r=>r.origin),origins);}
});
test('retained tracker rendering still counts the same filtered origins as the native model',async()=>{
 const {c,flags}=fixture(),t=c.categories.TrackingProtection;
 class Node {constructor(){this.children=[];this.classList={toggle(){},add(){}};}appendChild(child){this.children.push(child);}append(child){this.children.push(child);}setAttribute(){}get childElementCount(){return this.children.length;}}
 c.document.createDocumentFragment=()=>new Node();c.document.createElementNS=()=>new Node();c.document.createXULElement=()=>new Node();c.document.l10n={setAttributes(){}};
 const log={'https://blocked.test':[[flags.STATE_BLOCKED_TRACKING_CONTENT,true]],'https://unrelated.test':[[flags.STATE_COOKIES_LOADED,false]]};
 c.gBrowser.selectedBrowser.getContentBlockingLog=()=>JSON.stringify(log);
 assert.equal(await t.getBlockerCount(),t.getTrackerItems(log).length);assert.equal(await t.getBlockerCount(),1);
});
