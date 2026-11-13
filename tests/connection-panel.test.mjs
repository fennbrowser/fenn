import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const source=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-siteIdentity.js',import.meta.url),'utf8');
const adapter=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIConnectionPanel.sys.mjs',import.meta.url),'utf8').replace(/^import\b[^;]*;$/m,'').replace('export class WinUIConnectionPanel','this.Panel=class WinUIConnectionPanel');
const settle=()=>new Promise(resolve=>setTimeout(resolve,0));
function fixture(){
 const calls=[],permissions=new Map();let privateWindow=false,dataReply,qwacReply,trackingException=false,trackingSupported=true;
 class URI {constructor(scheme='https'){this.scheme=scheme;this.host=this.asciiHost='first.test';this.port=-1;this.spec=scheme+'://first.test';}schemeIs(s){return s===this.scheme;}mutate(){return {setScheme:s=>({finalize:()=>new URI(s)})};}}
 const principal={originAttributes:{userContextId:2,privateBrowsingId:0},equals(p){return p===this;}};
 const browser={getContentBlockingLog:()=> '{}',contentPrincipal:principal,currentURI:new URI(),documentURI:new URI(),browsingContext:{currentWindowGlobal:{innerWindowId:7}},focus:()=>calls.push(['focus'])};
 const gBrowser={selectedBrowser:browser,selectedTab:{},contentPrincipal:principal,get currentURI(){return browser.currentURI;},loadURI:(...a)=>calls.push(['load',...a])};
 const node={id:'identity-icon-box',contains:e=>e===node},document={fullscreen:false,getElementById:()=>node};
 const window={document,gBrowser,gDialogBox:{isOpen:false},gURLBar:{getAttribute:()=> 'valid'},PrivateBrowsingUtils:{isWindowPrivate:()=>privateWindow},SiteDataManager:{hasSiteData:()=>new Promise(r=>dataReply=r)},openTrustedLinkIn:(...a)=>calls.push(['help',...a])};
 const SitePermissions={SCOPE_PERSISTENT:1,SCOPE_SESSION:2,getForPrincipal:()=>({state:permissions.get('https') || 0}),setForPrincipal:(p,id,state,scope)=>{permissions.set('https',state);calls.push(['set',p,id,state,scope]);},removeFromPrincipal:(...a)=>{permissions.delete('https');calls.push(['remove',...a]);}};
 const flags={STATE_IS_BROKEN:1,STATE_IS_SECURE:2,STATE_IDENTITY_EV_TOPLEVEL:4,STATE_IDENTITY_ASSOCIATED:8,STATE_LOADED_MIXED_ACTIVE_CONTENT:16,STATE_BLOCKED_MIXED_ACTIVE_CONTENT:32,STATE_LOADED_MIXED_DISPLAY_CONTENT:64,STATE_HTTPS_ONLY_MODE_UPGRADED:128,STATE_HTTPS_ONLY_MODE_UPGRADE_FAILED:256,STATE_HTTPS_ONLY_MODE_UPGRADED_FIRST:512,STATE_CERT_USER_OVERRIDDEN:1024};
 window.ContentBlockingAllowList={canHandle:()=>trackingSupported,includes:()=>trackingException};
 window.gProtectionsHandler={disableForCurrentPage:reload=>{calls.push(["tracking-off",reload]);trackingException=true;},enableForCurrentPage:reload=>{calls.push(["tracking-on",reload]);trackingException=false;}};
 const globals={ContentBlockingAllowList:window.ContentBlockingAllowList,window,document,gBrowser,SitePermissions,PrivateBrowsingUtils:window.PrivateBrowsingUtils,ChromeUtils:{defineESModuleGetters(){}},console:{error:e=>calls.push(['error',String(e)])},
  Ci:{nsIWebProgressListener:flags,nsINestedURI:class {},nsIHttpsOnlyModePermission:{LOAD_INSECURE_ALLOW:10,LOAD_INSECURE_ALLOW_SESSION:20},nsIWebNavigation:{LOAD_FLAGS_REPLACE_HISTORY:1}},
  Services:{env:{exists:()=>false},obs:{addObserver(){},removeObserver(){}},prefs:{addObserver(){},removeObserver(){}},scriptSecurityManager:{createContentPrincipal:(uri,attrs)=>({uri,attrs}),getSystemPrincipal:()=>({system:true})},urlFormatter:{formatURLPref:()=> 'https://support.test/'}},
  QWACs:{determineQWACStatus:()=>new Promise(r=>qwacReply=r)},BrowserCommands:{reload:()=>calls.push(["tracking-reload"]),reloadSkipCache:()=>calls.push(['reload'])},PanelMultiView:{hidePopup:p=>calls.push(['hide',p])},displaySecurityInfo:()=>calls.push(['info']),
  SiteDataManager:{getBaseDomainFromHost:h=>h,promptSiteDataRemoval:(...a)=>{calls.push(['prompt',...a]);return true;},remove:(...a)=>calls.push(['data-remove',...a])},gNavigatorBundle:{getFormattedString:(id,args)=>id+': '+args.join(','),getString:id=>id}};
 window.BrowserCommands=globals.BrowserCommands;
 window.XULBrowserWindow={_event:0};window.gTrustPanelHandler={getConnectionTrackingState:()=>({count:0,categories:[]})};
 const context=vm.createContext(globals);vm.runInContext(source,context);const c=context.gIdentityHandler;window.gIdentityHandler=c;
 c._uri=browser.currentURI;c._uriHasHost=true;c._state=flags.STATE_IS_SECURE;c._isSecureContext=false;c._hasCustomRoot=()=>false;c.getHostForDisplay=()=>"first.test";
 Object.defineProperty(c,'_identityIconLabel',{value:{tooltipText:'Verified by Example CA'}});
 for(const name of ["_httpsOnlyModeEnabled","_httpsOnlyModeEnabledPBM","_httpsFirstModeEnabled","_httpsFirstModeEnabledPBM","_schemelessHttpsFirstModeEnabled","_overrideService"])delete c[name];
 c._httpsOnlyModeEnabled=false;c._httpsOnlyModeEnabledPBM=false;c._httpsFirstModeEnabled=false;c._httpsFirstModeEnabledPBM=false;c._schemelessHttpsFirstModeEnabled=false;
 c._overrideService={clearValidityOverride:(...a)=>calls.push(['override',...a])};
 vm.runInContext(adapter,context);const toolbar={window,active:true,navigation:true,supported:()=>true,customization:{editorOpen:false},owner:{addressSession:{sessionId:9},invalidateAddress(){}},omnibar:{entries:new Map([['a',{node,id:'source-1'}]]),entry:n=>({node:n,id:'source-1'})},menuAnchor:()=>({})};
 const panel=new context.Panel(toolbar);panel.label=(id,args,attr)=>id+(args?JSON.stringify(args):'')+(attr?':'+attr:'');window.gWinUIPane={addressToolbar:{connectionPanel:panel}};
 const open=()=>{assert.equal(panel.show({target:node}),true,JSON.stringify(calls));const p=panel.pending;assert.equal(panel.execute({command:`connection-panel:open:${p.request}:1`}),1);return p;};
 const invoke=id=>panel.execute({command:`connection-panel:invoke:${panel.pending.session}:${panel.pending.revision}:${id}`});
 return {panel,c,window,browser,toolbar,globals,flags,calls,permissions,open,invoke,setPrivate:()=>privateWindow=true,setTracking:(exception,supported=true)=>{trackingException=exception;trackingSupported=supported;},data:value=>dataReply(value),qwac:value=>qwacReply(value)};
}
test('native gateway precedes retained construction and shares QWAC operation',()=>{const body=source.slice(source.indexOf('  _openPopup(event)'));assert.ok(body.indexOf('connectionPanel?.show')<body.indexOf('this._initializePopup()'));});
test('closed adapter does not enumerate identity details or site data',()=>{const f=fixture();f.c.getConnectionPanelState=()=>assert.fail('closed read');f.panel.snapshot();assert.equal(f.panel.pending,undefined);});
test('secure classification comes from controller state rather than address text',()=>{const f=fixture(),p=f.open();assert.equal(p.presentation.summary,'identity-connection-verified');f.c._state=0;f.panel.changed();assert.equal(p.presentation.summary,'identity-connection-not-secure-security-view');assert.equal(p.presentation.icon,'warning');assert.equal(f.c._popupInitialized,false);});
test('broken HTTPS with mixed content is not presented as secure',()=>{const f=fixture();f.c._state=f.flags.STATE_IS_BROKEN|f.flags.STATE_LOADED_MIXED_ACTIVE_CONTENT;const p=f.open();assert.equal(p.presentation.icon,'warning');assert.ok(p.presentation.blocks.includes('identity-description-active-loaded'));assert.ok(!p.presentation.blocks.includes('identity-description-insecure'));});
test('passive mixed content and active blocking retain combined warning',()=>{const f=fixture();f.c._state=f.flags.STATE_IS_BROKEN|f.flags.STATE_LOADED_MIXED_DISPLAY_CONTENT|f.flags.STATE_BLOCKED_MIXED_ACTIVE_CONTENT;const p=f.open();assert.ok(p.presentation.blocks.includes('identity-description-passive-loaded-mixed2'));assert.equal(p.actions.get(6).page,'mixed-content');});
test('weak cipher, custom root and TLS key logging are explicit warnings',()=>{for(const mode of ['weak','custom','keys']){const f=fixture();if(mode==='weak')f.c._state=f.flags.STATE_IS_BROKEN;if(mode==='custom')f.c._hasCustomRoot=()=>true;if(mode==='keys')f.globals.Services.env.exists=()=>true;const p=f.open();assert.equal(p.presentation.icon,'warning');assert.ok(p.presentation.blocks.some(t=>t.includes(mode==='weak'?'weak-cipher':mode==='custom'?'custom-root':'tls-key-logging')));}});
test('EV owner, supplemental location and certificate verifier use shared original helper',()=>{const f=fixture();f.c._state|=f.flags.STATE_IDENTITY_EV_TOPLEVEL;f.c._secInfo={serverCert:{organization:'Example Org',issuerOrganization:'Example CA',subjectName:'L=City,ST=State,C=Country'}};const p=f.open();assert.ok(p.presentation.blocks.includes('Example Org'));assert.ok(p.presentation.blocks.some(t=>t.includes('State,Country')));});
test('internal, extension, file and associated classifications retain distinct text',()=>{for(const [property,value,id] of [['_isSecureInternalUI',true,'internal'],['_pageExtensionPolicy',{},'extension-page'],['_isURILoadedFromFile',true,'file'],['_state',8,'associated']]){const f=fixture();f.c._state=0;f.c[property]=value;const p=f.open();assert.equal(p.presentation.summary,id==='extension-page'?'identity-extension-page':'identity-connection-'+id);assert.equal(p.presentation.icon,'connection');}});
test('certificate exception removal invokes original scoped override and reload',()=>{const f=fixture();f.c._state|=f.flags.STATE_CERT_USER_OVERRIDDEN;const p=f.open();assert.ok(p.actions.has(3));assert.equal(f.invoke(3),1);const call=f.calls.find(c=>c[0]==='override');assert.deepEqual(Array.from(call.slice(1,3)),['first.test',443]);assert.equal(call[3],f.browser.contentPrincipal.originAttributes);assert.ok(f.calls.some(c=>c[0]==='reload'));assert.equal(f.panel.pending,null);});
test('More information works without constructing the retained popup',()=>{const f=fixture();f.open();assert.equal(f.invoke(8),1);assert.ok(f.calls.some(c=>c[0]==='info'));assert.equal(f.c._popupInitialized,false);});
test('site data arrives only for the captured page and uses original confirmation',async()=>{const f=fixture(),p=f.open();assert.ok(!p.actions.has(4));f.data(true);await settle();assert.ok(p.actions.has(4));assert.equal(f.invoke(4),1);await settle();assert.ok(f.calls.some(c=>c[0]==='prompt'));assert.ok(f.calls.some(c=>c[0]==='data-remove'));});
test('late site data cannot add an action to a dismissed or replaced page',async()=>{const f=fixture();f.open();f.panel.dismiss();f.data(true);await settle();assert.equal(f.panel.pending,null);assert.equal(f.calls.length,0);});
test('private panels omit site-data action and persistent HTTPS off choice',()=>{const f=fixture();f.setPrivate();f.c._httpsOnlyModeEnabled=true;f.c._state|=f.flags.STATE_HTTPS_ONLY_MODE_UPGRADED;const p=f.open();assert.deepEqual(Array.from(p.presentation.options,o=>o.id),[100,102]);assert.ok(!p.actions.has(4));});
test('private existing persistent off remains selectable, using original HTTP principal mapping',()=>{const f=fixture();f.setPrivate();f.c._httpsOnlyModeEnabled=true;f.permissions.set('https',10);const p=f.open();assert.equal(p.presentation.selected,101);assert.equal(f.invoke(102),1);const set=f.calls.find(c=>c[0]==='set');assert.equal(set[1].uri.scheme,'http');assert.equal(set[1].attrs,f.browser.contentPrincipal.originAttributes);assert.equal(set[3],20);assert.ok(!f.calls.some(c=>c[0]==='reload'));assert.equal(f.panel.pending,p);});
test('HTTPS on/off transition reloads, private forbidden persistent choice is rejected',()=>{const f=fixture();f.c._httpsOnlyModeEnabled=true;f.c._state|=f.flags.STATE_HTTPS_ONLY_MODE_UPGRADED;f.open();assert.equal(f.invoke(102),1);assert.ok(f.calls.some(c=>c[0]==='reload'));const p=fixture();p.setPrivate();p.c._httpsOnlyModeEnabled=true;p.c.changeHttpsOnlyPermission(1);assert.ok(!p.calls.some(c=>c[0]==='set'));});
test('schemeless upgrades show state but no exception selector',()=>{const f=fixture();f.c._schemelessHttpsFirstModeEnabled=true;f.c._state|=f.flags.STATE_HTTPS_ONLY_MODE_UPGRADED_FIRST;const p=f.open();assert.equal(p.presentation.options.length,0);assert.ok(p.presentation.blocks.includes('identity-https-only-connection-upgraded'));});
test('new tab, document, principal attributes or address session revoke old commands',()=>{for(const mutate of [f=>f.window.gBrowser.selectedTab={},f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.browser.contentPrincipal={originAttributes:{userContextId:3}},f=>f.toolbar.owner.addressSession.sessionId++]){const f=fixture(),p=f.open(),command=`connection-panel:invoke:${p.session}:${p.revision}:8`;mutate(f);assert.equal(f.panel.execute({command}),2);assert.equal(f.calls.length,0);}});
test('security changes revoke an already published action rather than silently executing it',()=>{const f=fixture(),p=f.open(),command=`connection-panel:invoke:${p.session}:${p.revision}:8`;f.c._state=0;assert.equal(f.panel.execute({command}),2);assert.equal(f.calls.length,0);});
test('replayed terminal commands and malformed identities do not execute',()=>{const f=fixture(),p=f.open(),command=`connection-panel:invoke:${p.session}:${p.revision}:8`;assert.equal(f.panel.execute({command}),1);assert.equal(f.panel.execute({command}),2);assert.equal(f.panel.execute({command:'connection-panel:open:4294967296:2'}),2);assert.equal(f.calls.filter(c=>c[0]==='info').length,1);});
test('QWAC completion revalidates original promise and cannot resurrect dismissed presentation',async()=>{const f=fixture();f.c._isSecureContext=true;f.open();f.panel.dismiss();f.qwac({organization:'Qualified Org'});await settle();assert.equal(f.panel.pending,null);assert.equal(f.c._qwac.organization,'Qualified Org');});
test('unsupported layouts retain Firefox presenter',()=>{const f=fixture();f.toolbar.active=false;assert.equal(f.panel.show(),false);f.toolbar.active=true;f.window.document.fullscreen=true;assert.equal(f.panel.show(),false);});
test('trust-mark decode is scoped to the current opening and image generation',async()=>{
 const f=fixture(),p=f.open(),replies=[];let serial=0;
 f.window.Image=class {decode(){this.id=++serial;return new Promise(resolve=>replies.push(resolve));}};
 f.window.document.createElementNS=()=>({getContext:()=>({drawImage(){},getImageData:()=>({width:64,height:95})})});
 f.toolbar.owner.encodePixels=data=>({...data,pixels:'test-'+serial});
 const old=f.panel.loadTrustMark(p),current=f.panel.loadTrustMark(p);replies[0]();await old;assert.equal(p.mark,undefined);replies[1]();await current;assert.equal(p.mark.pixels,'test-2');
 const late=f.panel.loadTrustMark(p);f.panel.dismiss();replies[2]();await late;assert.equal(p.mark.pixels,'test-2');
});
test('HTTPS-only failure and invalid address proxy do not claim a verified connection',()=>{
 const f=fixture();f.c._httpsOnlyModeEnabled=true;f.c._state=0;Object.defineProperty(f.c,'_isAboutHttpsOnlyErrorPage',{value:true});const p=f.open();assert.equal(p.presentation.icon,'warning');assert.ok(p.presentation.blocks.includes('identity-https-only-info-no-upgrade'));f.panel.dismiss();f.window.gURLBar.getAttribute=()=> 'invalid';assert.equal(f.panel.show(),false);
});
test('original same-document security notifications refresh native warnings directly',()=>{
 const f=fixture(),p=f.open();f.window.gBrowser.securityUI={secInfo:null};f.c.setURI=uri=>f.c._uri=uri;f.c._getIsSecureContext=()=>false;f.c.refreshIdentityBlock=()=>{};
 f.c.updateIdentity(f.flags.STATE_IS_BROKEN,f.browser.currentURI);assert.equal(p.presentation.icon,'warning');assert.ok(p.presentation.blocks.includes('identity-description-weak-cipher-intro'));
});
test('tracking switch publishes Firefox allow-list state and original localized labels',()=>{
 const f=fixture(),p=f.open();assert.equal(p.presentation.tracking.on,true);assert.equal(p.presentation.tracking.command,9);assert.equal(p.presentation.tracking.label,'trustpanel-etp-label-enabled');
 f.setTracking(true);f.panel.changed();assert.equal(p.presentation.tracking.on,false);assert.equal(p.presentation.tracking.label,'trustpanel-etp-label-disabled');assert.ok(p.presentation.tracking.name.includes('first.test'));
});
test('tracking switch uses original enable/disable without a retained popup and reloads once',()=>{
 for(const exception of [false,true]){const f=fixture();f.setTracking(exception);const p=f.open(),command=`connection-panel:invoke:${p.session}:${p.revision}:9`;assert.equal(f.panel.execute({command}),1);assert.equal(f.panel.execute({command}),2);assert.deepEqual(f.calls.find(c=>c[0]===(exception?'tracking-on':'tracking-off')),[exception?'tracking-on':'tracking-off',false]);assert.equal(f.calls.filter(c=>c[0]==='tracking-reload').length,1);assert.equal(f.c._popupInitialized,false);assert.equal(f.panel.pending,null);}
});
test('unsupported documents have no tracking action and changed allow-list authority rejects stale clicks',()=>{
 const f=fixture();f.setTracking(false,false);const p=f.open();assert.equal(p.presentation.tracking.visible,false);assert.equal(p.presentation.tracking.command,0);assert.equal(f.invoke(9),2);assert.equal(f.calls.length,0);
 const other=fixture(),q=other.open(),command=`connection-panel:invoke:${q.session}:${q.revision}:9`;other.setTracking(true);assert.equal(other.panel.execute({command}),2);assert.equal(other.calls.length,0);
});
test('private tracking commands still use Firefox operations on the captured private browser',()=>{
 const f=fixture();f.setPrivate();f.open();assert.equal(f.invoke(9),1);assert.ok(f.calls.some(c=>c[0]==='tracking-off'));assert.ok(!f.calls.some(c=>c[0]==='set' || c[0]==='remove'));
});

function trackers(f,{count=1,items=[{origin:'https://tracker.test',isAllowed:false}],blocking=true,index=3}={}){
 f.window.gTrustPanelHandler.getConnectionTrackingState=()=>({count,categories:[{index,blocking,keys:{general:index===1?'tracking-cookies':'fingerprinter',content:index===1?'cross-site-tracking-cookies':'fingerprinters'},title:'protections-blocking-fingerprinters',items}]});
 return f;
}
test('zero blocked trackers omits summary and its commands',()=>{const f=fixture(),p=f.open();assert.equal(p.presentation.trackers.visible,false);assert.equal(f.invoke(10),2);});
test('blocked summary opens native categories and details, then returns without closing',()=>{
 const f=trackers(fixture()),p=f.open();assert.equal(p.presentation.trackers.visible,true);assert.equal(f.invoke(10),1);assert.equal(p.presentation.trackers.view,1);assert.equal(f.invoke(23),1);assert.equal(p.presentation.trackers.view,5);assert.equal(p.presentation.trackers.rows[0].label,'https://tracker.test');assert.equal(f.invoke(12),1);assert.equal(p.presentation.trackers.view,1);assert.equal(f.invoke(11),1);assert.equal(p.presentation.trackers.view,0);assert.equal(f.panel.pending,p);
});
test('allowed category and shim origin retain explicit text rather than color-only state',()=>{
 const f=trackers(fixture(),{blocking:false,items:[{origin:'https://shim.test',isAllowed:true,shimAllowed:true}]}),p=f.open();f.invoke(10);assert.ok(p.presentation.trackers.rows[0].state.includes('tracking-header'));f.invoke(23);assert.ok(p.presentation.trackers.rows[0].state.includes('allowed-label'));assert.ok(p.presentation.trackers.rows[0].state.includes('shim-allowed-indicator'));
});
test('origin pages are bounded and no origin is lost',()=>{
 const items=Array.from({length:19},(_,i)=>({origin:`https://tracker${i}.test`,isAllowed:false})),f=trackers(fixture(),{items}),p=f.open();f.invoke(10);f.invoke(23);const seen=[];
 for(let i=0;i<3;i++){seen.push(...p.presentation.trackers.rows.map(r=>r.label));assert.ok(p.presentation.trackers.rows.length<=8);if(i<2)assert.equal(f.invoke(14),1);}
 assert.deepEqual(seen,items.map(i=>i.origin));assert.equal(f.invoke(14),2);assert.equal(f.invoke(13),1);assert.equal(p.presentation.trackers.rows[0].label,items[8].origin);
});
test('cookie exception removal uses captured Firefox origin and stays open',()=>{
 const f=fixture();f.globals.Services.perms={ALLOW_ACTION:1,DENY_ACTION:2};trackers(f,{index:1,items:[{origin:'https://cookie.test',isAllowed:true,exceptionState:1}]});f.window.gTrustPanelHandler.clearConnectionCookieException=origin=>f.calls.push(['clear-cookie',origin]);const p=f.open();f.invoke(10);f.invoke(21);assert.equal(p.presentation.trackers.rows[0].command,1000);assert.equal(f.invoke(1000),1);assert.deepEqual(f.calls.find(c=>c[0]==='clear-cookie'),['clear-cookie','https://cookie.test']);assert.equal(f.panel.pending,p);
});
test('content-blocking changes reject obsolete tracker actions and zero count returns to security',()=>{
 const f=trackers(fixture()),p=f.open();f.invoke(10);const command=`connection-panel:invoke:${p.session}:${p.revision}:23`;trackers(f,{count:0});f.browser.getContentBlockingLog=()=>'{"changed":[]}';assert.equal(f.panel.execute({command}),2);assert.equal(f.panel.pending,null);
 const g=trackers(fixture()),q=g.open();g.invoke(10);g.invoke(23);trackers(g,{count:0});g.browser.getContentBlockingLog=()=>'{"changed":[]}';g.panel.changed();assert.equal(q.presentation.trackers.visible,false);assert.equal(q.presentation.trackers.view,0);
});
test('unchanged snapshots reuse tracker classification and closed panels never read the log',()=>{
 const f=trackers(fixture());let reads=0;const describe=f.window.gTrustPanelHandler.getConnectionTrackingState;f.window.gTrustPanelHandler.getConnectionTrackingState=(...args)=>{reads++;return describe(...args);};f.open();const before=reads;f.panel.snapshot();f.panel.snapshot();assert.equal(reads,before);f.panel.dismiss();f.browser.getContentBlockingLog=()=>assert.fail('closed log read');f.panel.snapshot();
});
test('changes outside the displayed origin page still revoke old commands',()=>{
 const f=trackers(fixture()),p=f.open();f.invoke(10);f.invoke(23);const command=`connection-panel:invoke:${p.session}:${p.revision}:12`;f.browser.getContentBlockingLog=()=>'{"off-page":[]}';assert.equal(f.panel.execute({command}),2);assert.equal(f.panel.pending,null);
});
test('combined localized tracker descriptions and shim states respect native field bounds',()=>{
 const f=trackers(fixture(),{items:[{origin:'https://shim.test',isAllowed:true,shimAllowed:true}]});f.panel.label=()=> 'x'.repeat(2048);const p=f.open();f.invoke(10);f.invoke(23);assert.ok(p.presentation.trackers.description.length<=4096);assert.ok(p.presentation.trackers.rows[0].state.length<=2048);
});
