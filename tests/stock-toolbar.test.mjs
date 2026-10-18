import assert from 'node:assert/strict';
import test from 'node:test';
import {STOCK_TOOLBAR_WIDGETS,WinUIStockToolbar} from '../upstream/firefox/browser/base/content/WinUIStockToolbar.sys.mjs';

function fixture(widget='find-button') {
  const calls=[], attrs=new Map(), cmdattrs=new Map();
  const button={getAttribute:k=>attrs.get(k)||'',querySelector:()=>null,doCommand:()=>calls.push('command')};
  const source={getAttribute:k=>cmdattrs.get(k)||''};
  const anchor={setAttribute(){}};
  const window={MutationObserver:class {observe(){} disconnect(){calls.push('disconnect');}},
    document:{getElementById:()=>source},PanelUI:{showSubView:(...args)=>calls.push(args)},KeyboardEvent:class{constructor(type,opts){Object.assign(this,{type,...opts});}}};
  const toolbar={window,navigationModel:{snapshot:()=>({})},menuAnchor:()=>anchor,owner:{invalidateAddress(){}}};
  const c={toolbar,l10nLabel:()=>'',nativeOverflow:new Set(),entries:new Map([[widget,{id:'widget-1',widget,node:button}]]),
    ui:{getWidget:()=>({forWindow:()=>({node:button})})}};
  const stock=new WinUIStockToolbar(c);
  let placed=true;
  c.snapshot=()=>({items:[{id:'widget-1',placed,stock:stock.snapshot(widget,button)}]});
  toolbar.navigationState=()=>({customization:c.snapshot()});
  return {stock,c,toolbar,window,attrs,cmdattrs,button,source,calls,setPlaced:value=>placed=value};
}
test('stock inventory explicitly covers stock buttons and groups, excluding arbitrary widgets and Sidebar',()=>{
  assert.deepEqual(STOCK_TOOLBAR_WIDGETS['zoom-controls'],['zoom-out-button','zoom-reset-button','zoom-in-button']);
  assert.equal(STOCK_TOOLBAR_WIDGETS['edit-controls'].length,3);
  assert.equal(STOCK_TOOLBAR_WIDGETS['sidebar-button'],undefined);
  assert.equal(STOCK_TOOLBAR_WIDGETS['some-user-script'],undefined);
});
test('activation rechecks live placement, disabled commands, modal state, overflow and exact node identity',()=>{
  for(const mutate of [f=>f.setPlaced(false),f=>f.attrs.set('disabled','true'),f=>{f.attrs.set('command','cmd_find');f.cmdattrs.set('disabled','true');},f=>f.c.nativeOverflow.add('find-button'),f=>f.c.editorOpen=true,f=>f.window.gDialogBox={isOpen:true},f=>f.c.ui.getWidget=()=>({forWindow:()=>({node:{}})})]) {
    const f=fixture();f.attrs.set('label','Find');mutate(f);
    assert.equal(f.stock.execute('stock:widget-1:find-button',{},{}),2);
    assert.deepEqual(f.calls,[]);
  }
});
test('only current stock entry and exact part can dispatch its original Firefox command',()=>{
  const f=fixture();f.attrs.set('label','Find');
  f.toolbar.navigationModel.snapshot=()=>{throw Error('stock activation must refresh the complete publication, including pinned extensions');};
  for(const id of ['stock:widget-999:find-button','stock:widget-1:print-button','stock:widget-1:find-button:extra'])assert.equal(f.stock.execute(id,{},{}),2);
  assert.equal(f.stock.execute('stock:widget-1:find-button',{},{}),1);
  assert.deepEqual(f.calls,['command']);
});
test('panel widgets use a validated native anchor and Firefox PanelUI with keyboard origin',()=>{
  const f=fixture('library-button');f.attrs.set('label','Library');
  assert.equal(f.stock.execute('stock:widget-1:library-button',{keyboard:true},{}),1);
  assert.equal(f.calls[0][0],'appMenu-libraryView');assert.equal(f.calls[0][2].key,'Enter');
  f.toolbar.menuAnchor=()=>null;
  assert.equal(f.stock.execute('stock:widget-1:library-button',{},{}),2);
});
test('group state comes from each original button and command; zoom text remains Firefox-owned',()=>{
  const f=fixture('zoom-controls');
  f.button.querySelector=selector=>({getAttribute:key=>key==='label'?(selector==='#zoom-reset-button'?'125%':selector.slice(1)):key==='command'?selector.slice(1):'',querySelector:()=>null});
  f.cmdattrs.set('disabled','true');
  const parts=f.stock.snapshot('zoom-controls',f.button);
  assert.equal(parts.length,3);assert.equal(parts[1].text,'125%');assert.equal(parts[1].label,'125%');assert.equal(parts.every(p=>!p.enabled),true);
  f.stock.close();assert.deepEqual(f.calls,['disconnect']);
});
test('New Tab invokes Firefox with the original modifier event rather than its container popup',()=>{
  const f=fixture('new-tab-button'),event={shiftKey:true,ctrlKey:true};f.attrs.set('label','New Tab');
  f.window.BrowserCommands={openTab:opts=>f.calls.push(opts)};
  f.button.querySelector=()=>({openPopup(){throw Error('primary activation is not a container menu');}});
  assert.equal(f.stock.execute('stock:widget-1:new-tab-button',{},event),1);
  assert.equal(f.calls[0].event,event);
});
test('unplaced stock widgets resolve their own Fluent resource before DOM localization',()=>{
  const f=fixture('fullscreen-button');f.attrs.set('data-l10n-id','appmenuitem-fullscreen');
  f.window.Localization=class {
    constructor(resources,sync){for(const resource of ['branding/brand.ftl','toolkit/branding/brandings.ftl','browser/appmenu.ftl','browser/screenshots.ftl'])assert(resources.includes(resource));assert.equal(sync,true);}
    formatMessagesSync(requests){assert.equal(requests[0].id,'appmenuitem-fullscreen');return [{attributes:[{name:'label',value:'Localized Fullscreen'}]}];}
  };
  assert.equal(f.stock.label(f.button),'Localized Fullscreen');
});
test('Firefox View stays out of private palettes and source CSS visibility remains authoritative',()=>{
  const f=fixture('firefox-view-button');f.window.PrivateBrowsingUtils={isWindowPrivate:()=>true};
  assert.equal(f.stock.eligible('firefox-view-button'),false);assert.equal(f.stock.eligible('find-button'),true);
  f.attrs.set('label','View');f.button.isConnected=true;f.button.checkVisibility=()=>false;
  const part=f.stock.snapshot('firefox-view-button',f.button)[0];assert.equal(part.shown,false);assert.equal(part.enabled,false);
});
