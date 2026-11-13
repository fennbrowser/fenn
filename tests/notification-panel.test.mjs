import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const adapter=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUINotificationPanel.sys.mjs',import.meta.url),'utf8');
const controller=readFileSync(new URL('../upstream/firefox/toolkit/modules/PopupNotifications.sys.mjs',import.meta.url),'utf8');
function fixture(){
 const calls=[];let now=10000;
 const b={contentPrincipal:{equals(p){return p===this;}},browsingContext:{currentWindowGlobal:{innerWindowId:1}},focus(){}};
 const w={gBrowser:{selectedBrowser:b,selectedTab:{}},document:{fullscreen:false},setTimeout(fn){calls.push(fn);},focus(){calls.push('focus');},CustomEvent:class {constructor(type,options){this.type=type;Object.assign(this,options);}},Localization:class {formatValueSync(id){return id;}formatMessagesSync(){return [{attributes:[{name:'label',value:'OK'}]}];}}};
 const toolbar={active:true,navigation:true,supported:()=>true,owner:{addressSession:{sessionId:1},invalidateAddress(){}},window:w,omnibar:{entries:new Map()},menuAnchor:()=>true};
 const context=vm.createContext({console,ChromeUtils:{now:()=>now},Services:{focus:{activeWindow:w},console:{logStringMessage(){}},prefs:{}},XPCOMUtils:{defineLazyPreferenceGetter:(object,key)=>object[key]=1000}});
 vm.runInContext(controller.replace(/^import\b[^;]*;\r?\n/gm,'').replace('export function PopupNotifications','function PopupNotifications')+'\nthis.Controller=PopupNotifications;',context);
 vm.runInContext(adapter.replace('export class WinUINotificationPanel','this.Panel=class WinUINotificationPanel'),context);
 const p=new context.Panel(toolbar);toolbar.notificationPanel=p;w.gWinUIPane={addressToolbar:toolbar};
 const m=Object.create(context.Controller.prototype);m.window=w;m.tabbrowser=w.gBrowser;m.panel={state:'closed',children:[],setAttribute(){},removeAttribute(){},hasAttribute:()=>true,dispatchEvent:e=>calls.push(e.type)};
 m._clearPanel=()=>m.panel.children=[];m._update=()=>{if(!m.getNotificationsForBrowser(b).length)p.hide();};
 const n={id:'fixture',browser:b,anchorElement:{removeAttribute(){}},message:'Allow <> to notify you?',options:{name:'site.test',persistent:true,hideClose:true,escAction:'secondarybuttoncommand'},secondaryActions:[],_recordTelemetryStat(){},_recordTelemetry(){}};
 n.mainAction={label:'Allow',callback:state=>calls.push(['allow',state])};n.secondaryActions=[{label:'Block',disableSecurityDelay:true,callback:state=>calls.push(['block',state])}];
 const attrs=new Map();const el={notification:n,dispatchEvent:()=>true,getAttribute:k=>attrs.get(k)||'',hasAttribute:k=>attrs.has(k),toggleAttribute(k,v){if(v)attrs.set(k,'true');else attrs.delete(k);},setAttribute:(k,v)=>attrs.set(k,v),removeAttribute:k=>attrs.delete(k),querySelector:()=>null,matches:()=>false,closest:()=>el};
 el.checkbox={checked:false,closest:()=>el};el.menupopup={children:[]};m.panel.children=[el];m.panel.firstElementChild=el;m._setNotificationsForBrowser(b,[n]);
 const open=()=>{assert.equal(p.show(m,[n],null),true);const request=p.pending.request;assert.equal(p.execute({command:`notification-panel:open:${request}:7`}),1);return p.pending;};
 const invoke=id=>p.execute({command:`notification-panel:invoke:${p.pending.session}:${p.pending.revision}:${id}`});
 return {p,m,w,b,n,el,attrs,calls,open,invoke,tick:ms=>now+=ms,toolbar};
}
test('native gateway shares original showing and refresh ordering',()=>{
 const flat=controller.replace(/\s+/g,''),refresh=flat.indexOf('this._refreshPanel(notificationsToShow);'),show=flat.indexOf('notificationPanel?.show(this,notificationsToShow,anchorElement');
 assert.ok(refresh>=0 && show>=0 && refresh<show);
 assert.match(controller,/notificationPanel\?\.hide\(\)/);
});
test('native shown acknowledgment starts the original security clock once',()=>{
 const f=fixture(),p=f.open();assert.equal(p.shown,false);assert.equal(f.invoke(101),2);f.invoke(1);assert.equal(p.shown,true);const shown=f.n.timeShown;f.tick(2000);assert.equal(f.invoke(1),2);assert.equal(f.n.timeShown,shown);
});
test('early allow is rejected by Firefox and cannot bypass the delay',()=>{
 const f=fixture();f.open();f.invoke(1);f.invoke(101);assert.ok(!f.calls.some(c=>c[0]==='allow'));assert.ok(f.p.pending);f.tick(1100);f.invoke(101);assert.equal(f.calls.filter(c=>c[0]==='allow').length,1);assert.equal(f.p.pending,null);
});
test('deny and persistent Escape use original delay exemption and source',()=>{
 for(const id of [102,2]){const f=fixture();f.open();f.invoke(1);f.invoke(id);const call=f.calls.find(c=>c[0]==='block');assert.ok(call);assert.equal(call[1].source,id===2?'esc-press':'button');assert.equal(f.p.pending,null);}
});
test('inactive windows cannot grant permissions',()=>{
 const f=fixture();f.open();f.invoke(1);f.tick(2000);f.el.matches=()=>true;f.invoke(101);assert.ok(f.calls.includes('focus'));assert.ok(!f.calls.some(c=>c[0]==='allow'));
});
test('checkbox updates original UI state, warning and callback values',()=>{
 const f=fixture();f.n.options.checkbox={label:'Remember',checkedState:{disableMainAction:true,warningLabel:'Warning'}};f.open();f.invoke(1);f.invoke(100);assert.equal(f.n._checkboxChecked,true);assert.equal(f.p.pending.presentation.rows[0].warning,'Warning');assert.equal(f.invoke(101),2);f.invoke(100);f.tick(1100);f.invoke(101);assert.equal(f.calls.find(c=>c[0]==='allow')[1].checkboxChecked,false);
});
test('removed notifications and replaced documents revoke all native actions',()=>{
 for(const mutate of [f=>f.b.browsingContext.currentWindowGlobal.innerWindowId++,f=>f.b.contentPrincipal={equals:()=>false},f=>f.w.gBrowser.selectedTab={},f=>f.toolbar.owner.addressSession.sessionId++,f=>f.m._setNotificationsForBrowser(f.b,[])]){const f=fixture();f.open();f.invoke(1);mutate(f);assert.equal(f.invoke(101),2);assert.equal(f.p.pending,null);assert.ok(!f.calls.some(c=>c[0]==='allow'));}
});
test('disabled, stale, malformed and replayed actions cannot grant',()=>{
 const f=fixture();f.open();f.invoke(1);const command=`notification-panel:invoke:7:${f.p.pending.revision}:101`;f.n.mainAction.disabled=true;assert.equal(f.p.execute({command}),2);assert.equal(f.invoke(101),2);for(const command of ['notification-panel:invoke:7:2:9999','notification-panel:invoke:7:2:4294967296','notification-panel:open:1:99999999999999'])assert.equal(f.p.execute({command}),2);assert.ok(!f.calls.some(c=>c[0]==='allow'));
});
test('programmatic hiding keeps the original request without a dismissal response',()=>{
 const f=fixture();f.open();f.invoke(1);f.invoke(3);assert.equal(f.p.pending,null);assert.equal(f.m.getNotificationsForBrowser(f.b).length,1);assert.ok(!f.n.dismissed);assert.ok(!f.calls.some(c=>c[0]==='block'));
});
test('nonpersistent outside dismissal preserves Firefox dismissed and removal callbacks',()=>{
 for(const remove of [false,true]){const f=fixture();f.n.options.persistent=false;f.n.options.removeOnDismissal=remove;f.n.options.eventCallback=event=>f.calls.push(event);f.open();f.invoke(1);f.p.userDismiss();assert.equal(f.p.pending,null);assert.ok(f.calls.includes(remove?'removed':'dismissed'));assert.equal(!!f.n.dismissed,!remove);}
});
test('custom content, oversized descriptions, fullscreen and unsupported layouts retain Firefox',()=>{
 for(const mutate of [f=>f.el.querySelector=()=>({}),f=>f.el.getAttribute=k=>k==='is'?'custom':'',f=>f.n.message='x'.repeat(4097),f=>f.w.document.fullscreen=true,f=>f.toolbar.supported=()=>false,f=>f.n.secondaryActions=Array(9).fill(f.n.secondaryActions[0])]){const f=fixture();mutate(f);assert.equal(f.p.show(f.m,[f.n],null),false);assert.ok(!f.p.pending);}
});
test('secondary menu commands keep Firefox action metadata and dismissal handling',()=>{
 const f=fixture();f.n.secondaryActions.push({label:'Never',telemetryStatId:3,callback:state=>f.calls.push(['never',state])});f.el.menupopup.children=[{notification:f.n,action:f.n.secondaryActions[1],closest:()=>f.el}];f.open();f.invoke(1);f.invoke(103);assert.equal(f.calls.find(c=>c[0]==='never')[1].source,'menucommand');assert.equal(f.p.pending,null);
});
test('original prompt command listeners can veto primary actions',()=>{
 const f=fixture();f.open();f.invoke(1);f.tick(2000);f.el.dispatchEvent=()=>false;f.invoke(101);assert.ok(!f.calls.some(c=>c[0]==='allow'));assert.ok(f.p.pending);
});
test('icon focus requests do not reset the permission security clock',()=>{
 const f=fixture();f.open();f.invoke(1);const shown=f.n.timeShown;f.p.focus();assert.equal(f.p.pending.presentation.focus,1);assert.equal(f.n.timeShown,shown);
});
test('custom content added on shown revokes native actions and permanently selects retained UI for that request',()=>{
 const f=fixture();f.open();f.n.options.eventCallback=event=>{if(event==='shown')f.el.querySelector=()=>({});};f.invoke(1);assert.equal(f.p.pending,null);assert.ok(f.p.retained.has(f.n));assert.ok(f.calls.some(c=>typeof c==='function'));f.el.querySelector=()=>null;assert.equal(f.p.show(f.m,[f.n],null),false);assert.ok(!f.calls.some(c=>c[0]==='allow'));
});
test('sidebar and other-browser notifications retain their original presenter',()=>{
 const f=fixture();f.n.browser={};assert.equal(f.p.show(f.m,[f.n],null),false);assert.ok(!f.p.pending);assert.ok(!f.calls.some(c=>c[0]==='allow'));
});
test('an automatically exposed notification icon gets an identity before the next toolbar snapshot',()=>{
 const f=fixture(),icon={ownerDocument:f.w.document};f.toolbar.omnibar.entry=node=>{assert.equal(node,icon);return {id:'action-99'};};assert.equal(f.p.show(f.m,[f.n],{closest:()=>icon}),true);assert.equal(f.p.pending.source,'action-99');
});
test('clicking an already open native notification icon focuses without rerunning showing callbacks',()=>{
 const f=fixture();f.open();f.invoke(1);f.m.iconBox={addEventListener(){}};const icon={parentNode:f.m.iconBox};f.n.anchorElement=icon;f.m._onIconBoxCommand({type:'click',button:0,target:icon,stopPropagation(){}});assert.equal(f.p.pending.presentation.focus,1);assert.equal(f.calls.filter(c=>c==='Shown').length,1);
});
