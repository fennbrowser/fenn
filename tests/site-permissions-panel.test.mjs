import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const controllerSource=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-sitePermissionPanel.js',import.meta.url),'utf8');
const nativeSource=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUISitePermissionsPanel.sys.mjs',import.meta.url),'utf8').replace('export class WinUISitePermissionsPanel','this.Panel=class WinUISitePermissionsPanel');
const settle=()=>new Promise(resolve=>setTimeout(resolve,0));
function fixture(){
 const records=new Map(),calls=[],locked=new Set();let geoReply;
 const principal={origin:'https://first.test',equals(other){return other?.origin===this.origin;}};
 const browser={contentPrincipal:principal,currentURI:{spec:principal.origin},loadContext:{},browsingContext:{currentWindowGlobal:{innerWindowId:7}},popupAndRedirectBlocker:{getBlockedPopupCount:()=>0,isRedirectBlocked:()=>false,unblockAllPopups:()=>calls.push(['popups']),unblockFirstRedirect:()=>calls.push(['redirect'])},getDevicePermissionOrigins:()=>new Set(['https://frame.test'])};
 const s={UNKNOWN:0,ALLOW:1,BLOCK:2,PROMPT:3,AUTOPLAY_BLOCKED_ALL:5,PERM_KEY_DELIMITER:'^',SCOPE_PERSISTENT:'persistent',SCOPE_TEMPORARY:'temporary',SCOPE_REQUEST:'request',SCOPE_GLOBAL:'global',SCOPE_POLICY:'policy',
   isSupportedPrincipal:()=>true,getPermissionLabel:id=>id==='unknown'?null:id,getDefault:id=>['autoplay-media','popup'].includes(id)?2:0,
   getCurrentStateLabel:(state,id,scope)=>state===5?null:`${id}:${state}:${scope}`,getAvailableStates:id=>id==='autoplay-media'?[1,2,5]:[1,2],getMultichoiceStateLabel:(_id,state)=>'state '+state,
   getAllPermissionDetailsForBrowser:()=>[...records.values()].map(p=>({...p})),getAllForBrowser:()=>[...records.values()].map(p=>({...p})),getForPrincipal:(_p,id)=>records.get(id) || {state:0},
   setForPrincipal:(p,id,state)=>{calls.push(['set',p,id,state]);records.set(id,{id,state,scope:'persistent'});},removeFromPrincipal:(p,id,b)=>{calls.push(['remove',p,id,b]);records.delete(id);}};
 const node={id:'identity-permission-box',contains(target){return target===this;}},document={fullscreen:false,getElementById:()=>node};
 const window={document,gBrowser:{selectedBrowser:browser,selectedTab:{},updateBrowserSharing:(...a)=>calls.push(['sharing',...a])},SitePermissions:s,gDialogBox:{isOpen:false},gIdentityHandler:{getHostForDisplay:()=> 'first.test'},gNavigatorBundle:{getString:id=>id,getFormattedString:(id,args)=>id+': '+args.join(',')},openTrustedLinkIn:(...a)=>calls.push(['link',...a])};
 const globals={console:{error:e=>calls.push(['error',String(e)])},window,document,gBrowser:window.gBrowser,SitePermissions:s,gNavigatorBundle:window.gNavigatorBundle,
   Services:{obs:{addObserver(){},removeObserver(){},notifyObservers:(...a)=>calls.push(['notify',...a])},prefs:{addObserver(){},removeObserver(){},prefIsLocked:id=>locked.has(id)},io:{newURI:value=>new URL(value)},eTLD:{getSite:uri=>'https://'+uri.hostname.split('.').slice(-2).join('.')},scriptSecurityManager:{createContentPrincipalFromOrigin:origin=>({origin})},intl:{RelativeTimeFormat:class {formatBestUnit(){return 'recent';}}},urlFormatter:{formatURLPref:()=> 'https://support.test/'}},
   Ci:{nsISiteCategory:{},nsIMediaManagerService:{STATE_CAPTURE_ENABLED:1}},ChromeUtils:{defineESModuleGetters(){}},XPCOMUtils:{defineLazyServiceGetter(){},defineLazyPreferenceGetter:(o,key,_pref,value)=>o[key]=value},Glean:{webNotificationPermission:{permissionRevokedToolbar:{record:value=>calls.push(['telemetry',value])}}},SiteCategory:{getCategory:()=> 'test'},SerialDeviceSharingHelper:{resetBrowserCount:b=>calls.push(['serial',b])},webrtcUI:{clearPermissionsAndStopSharing:(...a)=>calls.push(['rtc',...a])},ContentPrefService2:{getByDomainAndName:(_url,_key,_context,callback)=>{geoReply=callback;}}};
 const realm=vm.createContext(globals);vm.runInContext(controllerSource,realm);window.gPermissionPanel=realm.gPermissionPanel;vm.runInContext(nativeSource,realm);
 const toolbar={window,active:true,navigation:true,supported:()=>true,customization:{editorOpen:false},owner:{addressSession:{sessionId:9},invalidateAddress(){}},omnibar:{entries:new Map([['permissions',{node,id:'action-1'}]]),entry:n=>({node:n,id:'action-1'})},menuAnchor:()=>({})};
 const panel=new realm.Panel(toolbar);panel.label=(id,args)=>id+(args?JSON.stringify(args):'');
 const put=(id,state=2,scope='persistent')=>records.set(id,{id,state,scope});
 const open=()=>{assert.equal(panel.show({target:node}),true);const p=panel.pending;assert.equal(panel.execute({command:`permissions-panel:open:${p.request}:1`}),1);return p;};
 const invoke=id=>panel.execute({command:`permissions-panel:invoke:${panel.pending.session}:${panel.pending.revision}:${id}`});
 const action=(verb,kind)=>[...panel.pending.actions].find(([,a])=>a.verb===verb && (!kind || a.kind===kind))?.[0];
 return {panel,toolbar,window,browser,s,records,calls,locked,put,open,invoke,action,controller:realm.gPermissionPanel,geo:()=>geoReply};
}
test('native gateway precedes XUL template construction and original fullscreen handling remains first',()=>{
 const body=controllerSource.slice(controllerSource.indexOf('  openPopup(event)'));
 assert.ok(body.indexOf('document.fullscreen')<body.indexOf('permissionsPanel?.show(event)'));
 assert.ok(body.indexOf('permissionsPanel?.show(event)')<body.indexOf('this._initializePopup()'));
});
test('unsupported presentation and browser overrides retain the original presenter',()=>{
 const f=fixture();f.toolbar.active=false;assert.equal(f.panel.show(),false);f.toolbar.active=true;f.controller._browserOverride={};assert.equal(f.panel.show(),false);
});
test('native preparation and refresh do not initialize retained DOM',()=>{
 const f=fixture();f.put('geo');const p=f.open();assert.equal(f.controller._popupInitialized,false);assert.equal(p.presentation.rows[0].label,'geo');const rev=p.revision;f.panel.snapshot();assert.equal(p.revision,rev);
});
test('shared filtering covers site-keyed storage grants without exposing duplicate origins',()=>{
 const f=fixture();f.put('3rdPartyFrameStorage^https://third.test',1);f.put('3rdPartyStorage^https://a.third.test',1);f.put('storage-access',1);f.put('unknown');const p=f.open();assert.equal(p.presentation.rows.length,1);assert.equal(p.presentation.rows[0].group,'storage');assert.ok(!p.presentation.rows[0].key.includes('third.test'));
});
test('shared device precedence keeps one row per device and honors Always Ask',()=>{
 const f=fixture();f.put('camera^device',1);f.put('camera',2);f.put('camera^later',1);f.put('microphone',3);let rows=f.controller.getPermissionDetailsForDisplay();assert.deepEqual(Array.from(rows,p=>p.id),['camera']);f.controller._gumShowAlwaysAsk=true;rows=f.controller.getPermissionDetailsForDisplay();assert.deepEqual(Array.from(rows,p=>p.id),['camera','microphone']);
});
test('live sharing supplies temporary permission rows with non-color status',()=>{
 const f=fixture();f.browser._sharingState={geo:true,webRTC:{camera:1,screen:'ScreenPaused'}};const p=f.open();assert.equal(p.presentation.rows.find(r=>r.label==='geo').iconState,'active');assert.equal(p.presentation.rows.find(r=>r.label==='screen').iconState,'paused');assert.ok(p.presentation.rows.find(r=>r.label==='camera').usage);
});
test('saved and temporary revocations use the original controller and browser target',()=>{
 for(const scope of ['persistent','temporary']){const f=fixture();f.put('geo',1,scope);const p=f.open();assert.equal(f.invoke(f.action('clear','geo')),1);assert.equal(f.records.has('geo'),false);assert.ok(p.presentation.reload);assert.equal(f.calls.find(c=>c[0]==='remove')[3],f.browser);assert.equal(f.calls.find(c=>c[0]==='sharing')[1],f.browser);}
});
test('device removal invokes Firefox sharing revocation as well as permission removal',()=>{
 const f=fixture();f.put('microphone',1,'temporary');f.open();f.invoke(f.action('clear','microphone'));assert.deepEqual(Array.from(f.calls.find(c=>c[0]==='rtc')[1]),['microphone']);assert.equal(f.calls.find(c=>c[0]==='rtc')[2],f.browser);
});
test('XR and serial removal preserve extra origins and device notifications',()=>{
 const f=fixture();f.browser._sharingState={xr:true,serial:true};f.put('xr',1);f.put('serial',1);f.open();f.invoke(f.action('clear','xr'));assert.ok(f.calls.some(c=>c[0]==='remove' && c[1].origin==='https://frame.test'));f.invoke(f.action('clear','serial'));assert.ok(f.calls.some(c=>c[0]==='serial'));assert.ok(f.calls.some(c=>c[0]==='notify' && c[2]==='serial-permission-revoked'));
});
test('frame-storage revocation also removes covered origin-keyed grants',()=>{
 const f=fixture();f.put('3rdPartyFrameStorage^https://third.test',1);f.put('3rdPartyStorage^https://a.third.test',1);f.open();f.invoke(f.action('clear','3rdPartyFrameStorage'));assert.equal(f.records.size,0);
});
test('notification removal retains telemetry on the bound principal',()=>{
 const f=fixture();f.put('desktop-notification');f.open();f.invoke(f.action('clear','desktop-notification'));assert.ok(f.calls.some(c=>c[0]==='telemetry'));
});
test('managed permissions have no clear action and cannot be changed through shared commands',()=>{
 const f=fixture();f.put('geo',1,'policy');f.put('autoplay-media',1,'global');const p=f.open();assert.ok(p.presentation.rows.every(row=>row.locked && !row.clear));assert.equal(f.controller.clearPermission(f.records.get('geo')),false);assert.equal(f.controller.setPermissionState(f.records.get('autoplay-media'),2),false);assert.equal(f.calls.length,0);
});
test('a newly applied policy revokes an already published clear action',()=>{
 const f=fixture();f.put('geo');const p=f.open(),id=f.action('clear','geo'),command=`permissions-panel:invoke:${p.session}:${p.revision}:${id}`;f.put('geo',2,'policy');assert.equal(f.panel.execute({command}),2);assert.ok(f.records.has('geo'));
});
test('multichoice maps default state to zero and rejects locked or unavailable states',()=>{
 const f=fixture();f.put('autoplay-media',2);const p=f.open();const row=p.presentation.rows[0];assert.equal(p.actions.get(row.selected).value,0);const option=[...p.actions].find(([,a])=>a.verb==='set' && a.value===5);assert.equal(f.invoke(option[0]),1);assert.equal(f.records.get('autoplay-media').state,5);assert.equal(f.controller.setPermissionState(f.records.get('autoplay-media'),99),false);f.locked.add('dom.disable_open_during_load');assert.equal(f.controller.setPermissionState({id:'popup'},1),false);
});
test('stale document, principal, tab and address sessions cannot mutate permissions',()=>{
 for(const change of [f=>f.browser.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.browser.contentPrincipal={origin:'https://other.test'},f=>f.window.gBrowser.selectedTab={},f=>f.toolbar.owner.addressSession.sessionId++]){const f=fixture();f.put('geo');f.open();const id=f.action('clear','geo');change(f);assert.equal(f.invoke(id),2);assert.ok(f.records.has('geo'));f.panel.snapshot();assert.equal(f.panel.pending,null);}
});
test('revocation cannot be replayed and dismissal does not mutate permissions',()=>{
 const f=fixture();f.put('geo');const p=f.open(),command=`permissions-panel:invoke:${p.session}:${p.revision}:${f.action('clear','geo')}`;assert.equal(f.panel.execute({command}),1);assert.equal(f.panel.execute({command}),2);f.panel.dismiss();assert.equal(f.calls.filter(c=>c[0]==='remove').length,1);
});
test('blocked popup and redirect commands use the original bound blocker after dismissal',()=>{
 const f=fixture();f.browser.popupAndRedirectBlocker.getBlockedPopupCount=()=>2;f.browser.popupAndRedirectBlocker.isRedirectBlocked=()=>true;const p=f.open();assert.ok(p.presentation.rows.some(row=>row.label==='popup'));assert.equal(f.invoke(3),1);assert.equal(f.panel.pending,null);assert.ok(f.calls.some(c=>c[0]==='popups'));
});
test('larger collections remain reachable through bounded permission pages',()=>{
 const f=fixture();for(let i=0;i<35;i++)f.put('open-protocol-handler^app'+i,1);const p=f.open();assert.equal(p.presentation.rows.length,16);f.invoke(7);assert.equal(p.presentation.rows.length,16);f.invoke(7);assert.equal(p.presentation.rows.length,3);assert.ok(!p.actions.has(7));f.invoke(6);assert.equal(p.page,1);
});
test('late geolocation details cannot revive a dismissed or replaced opening',async()=>{
 const f=fixture();f.put('geo',1);f.open();const callback=f.geo();f.panel.dismiss();callback.handleResult({value:new Date().toISOString()});callback.handleCompletion();await settle();assert.equal(f.panel.pending,null);
});
test('malformed numbers and action identities cannot mutate state',()=>{
 const f=fixture();f.put('geo');const p=f.open();for(const command of ['permissions-panel:invoke:1:1:4294967296',`permissions-panel:invoke:${p.session}:${p.revision}:9999`,'permissions-panel:open:1:999999999999999999999'])assert.equal(f.panel.execute({command}),2);assert.ok(f.records.has('geo'));
});


test('same-document locations clear the reload hint while a replacement document revokes the panel',()=>{
 const f=fixture();f.put('geo');const p=f.open();f.invoke(f.action('clear','geo'));assert.ok(p.presentation.reload);f.panel.locationChanged();assert.equal(p.presentation.reload,'');assert.equal(f.panel.pending,p);f.browser.browsingContext.currentWindowGlobal.innerWindowId++;f.panel.locationChanged();assert.equal(f.panel.pending,null);
});
test('a stale terminal action closes only its original panel and never unblocks popups',()=>{
 const f=fixture();let count=2;f.browser.popupAndRedirectBlocker.getBlockedPopupCount=()=>count;const p=f.open();const command=`permissions-panel:invoke:${p.session}:${p.revision}:3`;count=3;assert.equal(f.panel.execute({command}),2);assert.equal(f.panel.pending,null);assert.ok(!f.calls.some(c=>c[0]==='popups'));
});
test('controller failures are acknowledged with an explicit error and unchanged permission state',()=>{
 const f=fixture();f.put('geo');const p=f.open();f.controller.clearPermission=()=>{throw new Error('Permission service unavailable');};assert.equal(f.invoke(f.action('clear','geo')),1);assert.ok(p.presentation.error);assert.ok(f.records.has('geo'));
});
