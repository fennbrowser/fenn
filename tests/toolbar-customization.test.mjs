import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
function fixture() {
 let order=['back-button','urlbar-container','stop-reload-button','fxa-toolbar-menu-button'],revision=0;
 const calls=[],nodes=new Map(order.map(id=>[id,{getAttribute:()=>id}]));
 const ui={AREA_NAVBAR:'nav',AREA_ADDONS:'addons',getWidgetIdsInArea:area=>area==='nav'?[...order]:[],getWidget:id=>nodes.has(id)?{forWindow:()=>({node:nodes.get(id)})}:null,
  getPlacementOfWidget:id=>order.includes(id)?{area:'nav',position:order.indexOf(id)}:null,
  canWidgetMoveToArea:id=>nodes.has(id),isWidgetRemovable:id=>id==='fxa-toolbar-menu-button',addListener(){},removeListener(){},
  removeWidgetFromArea(id){calls.push(['remove',id]);order=order.filter(x=>x!==id);},addWidgetToArea(id){calls.push(['add',id]);order.push(id);},
  moveWidgetWithinArea(id,position){calls.push(['move',id,position]);order=order.filter(x=>x!==id);order.splice(position,0,id);}};
 const context=vm.createContext({ChromeUtils:{importESModule:()=>({CustomizableUI:ui})}});
 vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIToolbarCustomization.sys.mjs',import.meta.url),'utf8').replace('export class WinUIToolbarCustomization','this.Customization = class WinUIToolbarCustomization'),context);
 const toolbar={window:{},owner:{invalidateAddress(){revision++;},localization:()=>({formatValueSync:k=>k})}};
 const c=new context.Customization(toolbar);toolbar.navigationState=()=>c.snapshot({});
 const state=()=>c.snapshot({});
 const command=(control,verb,target)=>{const s=state();return `customize:${s.revision}:${s.items.find(i=>i.control===control).id}:${verb}`+(target?':'+s.items.find(i=>i.control===target).id:'');};
 return {c,state,command,calls,ui,nodes,order:()=>order};
}
test('placement projection keeps stable identities and revisions until Firefox state changes',()=>{
 const f=fixture(),a=f.state(),b=f.state();assert.equal(a.revision,b.revision);assert.deepEqual(a.items,b.items);
 const command=f.command('profile','before','back');assert.equal(f.c.execute(command),1);
 assert.equal(f.order()[0],'fxa-toolbar-menu-button');assert.equal(f.c.execute(command),2);
});
test('mandatory address removal and malformed commands are rejected; removal/addition use Firefox',()=>{
 const f=fixture();assert.equal(f.c.execute(f.command('address','remove')),2);
 assert.equal(f.c.execute('customize:1:widget-1:end:widget-2'),2);
 assert.equal(f.c.execute(f.command('profile','remove')),1);assert.equal(f.state().items.find(i=>i.control==='profile').placed,false);
 assert.equal(f.c.execute(f.command('profile','add')),1);assert.equal(f.order().at(-1),'fxa-toolbar-menu-button');
});
test('external placement, replaced widget and modal changes revoke captured commands',()=>{
 for(const mutate of [f=>f.ui.moveWidgetWithinArea('stop-reload-button',0),f=>f.nodes.set('stop-reload-button',{getAttribute:()=> 'new'}),f=>f.c.toolbar.window.gDialogBox={isOpen:true}]) {
  const f=fixture(),cmd=f.command('reload','before','back');mutate(f);const n=f.calls.length;
  assert.equal(f.c.execute(cmd),2);assert.equal(f.calls.length,n);
 }
});
