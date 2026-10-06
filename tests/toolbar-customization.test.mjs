import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
function fixture() {
 let order=['back-button','urlbar-container','stop-reload-button','fxa-toolbar-menu-button'],revision=0;
 let nextSpring=0; let saved=null; const listeners=new Set();
 const defaults=[...order], extras=[], overflow=[], prefs=new Map();
 const calls=[],nodes=new Map(order.map(id=>[id,{getAttribute:()=>id}]));
 const ui={areas:['nav','addons','other','overflow'],AREA_FIXED_OVERFLOW_PANEL:'overflow',AREA_NAVBAR:'nav',AREA_ADDONS:'addons',getWidgetIdsInArea:area=>area==='nav'?[...order]:area==='other'?[...extras]:area==='overflow'?[...overflow]:[],getWidget:id=>nodes.has(id)?{forWindow:()=>({node:nodes.get(id)})}:null,
  getPlacementOfWidget:id=>order.includes(id)?{area:'nav',position:order.indexOf(id)}:overflow.includes(id)?{area:'overflow',position:overflow.indexOf(id)}:null,
  canWidgetMoveToArea:(id,area)=>area==='overflow' ? id==='fxa-toolbar-menu-button' : id==='spring'||nodes.has(id),isWidgetRemovable:id=>id.includes('spring')||id==='fxa-toolbar-menu-button',addListener(l){listeners.add(l);},removeListener(l){listeners.delete(l);},
  get inDefaultState(){return JSON.stringify(order)===JSON.stringify(defaults);},get canUndoReset(){return saved!==null;},
  reset(){calls.push(['reset']);saved=[...order];order=[...defaults];for(const l of listeners)l.onAreaReset?.();},
  undoReset(){calls.push(['undo-reset']);order=saved;saved=null;for(const l of listeners)l.onAreaReset?.();},
  removeWidgetFromArea(id){calls.push(['remove',id]);order=order.filter(x=>x!==id);},addWidgetToArea(id,area,position=order.length){calls.push(['add',id]);order=order.filter(x=>x!==id);const old=overflow.indexOf(id);if(old>=0)overflow.splice(old,1);if(area==='overflow'){overflow.push(id);return;}if(id==='spring'){id=`customizableui-special-spring${++nextSpring}`;nodes.set(id,{getAttribute:()=>id});}order.splice(position,0,id);},
  moveWidgetWithinArea(id,position){saved=null;calls.push(['move',id,position]);if(order.indexOf(id)<position)position--;order=order.filter(x=>x!==id);order.splice(position,0,id);}};
 const context=vm.createContext({Services:{strings:{createBundle:()=>({GetStringFromName:()=> 'Flexible Space'})},prefs:{addObserver(){},removeObserver(){},getPrefType(p){return prefs.has(p)?64:0;},getIntPref(p){return prefs.get(p);},prefHasUserValue(p){return prefs.has(p);}}},ChromeUtils:{importESModule:()=>({CustomizableUI:ui})}});
 vm.runInContext(readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIToolbarCustomization.sys.mjs',import.meta.url),'utf8').replace('export class WinUIToolbarCustomization','this.Customization = class WinUIToolbarCustomization'),context);
 const toolbar={window:{addEventListener(){},removeEventListener(){},document:{getElementById:()=>({overflowable:{setNativeOverflow:ids=>calls.push(["transient",...ids])}})}},owner:{invalidateAddress(){revision++;},localization:()=>({formatValueSync:k=>k})}};
 const c=new context.Customization(toolbar);toolbar.navigationState=()=>c.snapshot({});
 const state=()=>c.snapshot({});
 const command=(control,verb,target)=>{const s=state();return `customize:${s.revision}:${s.items.find(i=>i.control===control).id}:${verb}`+(target?':'+s.items.find(i=>i.control===target).id:'');};
 return {c,state,command,calls,ui,nodes,prefs,extras,overflow,order:()=>order};
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

test('restore and undo require an open editor, live revision, and Firefox availability',()=>{
 const f=fixture();assert.equal(f.c.execute(`customize:${f.state().revision}:reset`),2);
 f.c.execute('customize:editor:open');assert.equal(f.state().canReset,false);
 f.ui.moveWidgetWithinArea('stop-reload-button',0);const before=[...f.order()];
 const reset=`customize:${f.state().revision}:reset`;
 assert.equal(f.c.execute(reset),1);assert.equal(f.state().canReset,false);assert.equal(f.state().canUndoReset,true);
 assert.equal(f.c.execute(reset),2);
 assert.equal(f.c.execute(`customize:${f.state().revision}:undo-reset`),1);
 assert.deepEqual(f.order(),before);assert.equal(f.state().canUndoReset,false);
});
test('dismissing, another edit, and an external reset revoke native undo eligibility',()=>{
 for(const mutate of [f=>{f.c.execute('customize:editor:close');f.c.execute('customize:editor:open');},f=>f.ui.moveWidgetWithinArea('stop-reload-button',0),f=>f.ui.reset()]) {
  const f=fixture();f.c.execute('customize:editor:open');f.ui.moveWidgetWithinArea('stop-reload-button',0);
  assert.equal(f.c.execute(`customize:${f.state().revision}:reset`),1);
  const old=`customize:${f.state().revision}:undo-reset`;mutate(f);
  assert.equal(f.state().canUndoReset,false);assert.equal(f.c.execute(old),2);
 }
});

test('a reset captured before unrelated toolbar or density changes is rejected',()=>{
 for(const mutate of [f=>f.extras.push('outside-native-toolbar'),f=>f.prefs.set('browser.uidensity',2)]) {
  const f=fixture();f.c.execute('customize:editor:open');f.ui.moveWidgetWithinArea('stop-reload-button',0);
  const command=`customize:${f.state().revision}:reset`;mutate(f);const n=f.calls.length;
  assert.equal(f.c.execute(command),2);assert.equal(f.calls.length,n);
 }
});

test('spring palette creates distinct Firefox identities, movable/removable with a bounded native palette',()=>{
 const f=fixture();const palette=f.state().items.find(i=>i.control==='new-spring');
 assert.equal(palette.kind,'spring');assert.equal(palette.placed,false);
 assert.equal(f.c.execute(f.command('new-spring','add')),1);
 assert.equal(f.c.execute(f.command('new-spring','add')),1);
 const springs=f.state().items.filter(i=>i.kind==='spring' && i.placed);
 assert.equal(springs.length,2);assert.notEqual(springs[0].id,springs[1].id);
 assert.equal(f.c.execute(f.command(springs[0].control,'before','address')),1);
 const stale=f.command(springs[0].control,'end');
 assert.equal(f.c.execute(f.command(springs[0].control,'remove')),1);
 assert.equal(f.c.execute(stale),2);
 for(let i=1;i<16;i++)assert.equal(f.c.execute(f.command('new-spring','add')),1);
 assert.equal(f.state().items.find(i=>i.control==='new-spring').movable,false);
 assert.equal(f.c.execute(f.command('new-spring','add')),2);
});

test('forward moves use Firefox pre-removal insertion indexes exactly once',()=>{
 const f=fixture();assert.equal(f.c.execute(f.command('back','before','reload')),1);
 assert.deepEqual(f.order(),['urlbar-container','back-button','stop-reload-button','fxa-toolbar-menu-button']);
 assert.equal(f.c.execute(f.command('back','end')),1);
 assert.equal(f.order().at(-1),'back-button');
 assert.equal(f.c.execute(f.command('back','before','address')),1);
 assert.equal(f.order()[0],'back-button');
});


test('overflow delegates eligibility and placement to Firefox, with stale actions rejected',()=>{
 const f=fixture();
 for(const id of ['address','back','reload','new-spring'])assert.equal(f.c.execute(f.command(id,'overflow')),2);
 const move=f.command('profile','overflow');assert.equal(f.c.execute(move),1);
 assert.equal(f.c.execute(move),2);assert.deepEqual(f.overflow,['fxa-toolbar-menu-button']);
 const item=f.state().items.find(i=>i.control==='profile');assert.equal(item.overflow,true);assert.equal(item.placed,false);
 assert.equal(f.c.execute(f.command('profile','remove')),2);
 assert.equal(f.c.execute(f.command('profile','add')),1);assert.deepEqual(f.overflow,[]);
 const stale=f.command('profile','overflow');f.overflow.push('unsupported-firefox-widget');
 assert.equal(f.c.execute(stale),2);
});


test('native overflow is transient, bounded, revision-scoped and revalidates eligibility',()=>{
 const f=fixture(),s=f.state(),id=s.items.find(i=>i.control==='profile').id;
 assert.equal(f.c.execute(`customize:${s.revision}:overflow-layout:${id}`),1);
 assert.deepEqual(f.calls.at(-1),['transient','fxa-toolbar-menu-button']);
 assert.equal(f.state().items.find(i=>i.id===id).placed,true);
 assert.equal(f.c.execute(`customize:${s.revision}:overflow-layout:${id},${id}`),2);
 assert.equal(f.c.execute(`customize:${s.revision}:overflow-layout:widget-999`),2);
 const spring=s.items.find(i=>i.control==='new-spring').id;
 assert.equal(f.c.execute(`customize:${s.revision}:overflow-layout:${spring}`),2);
 f.ui.moveWidgetWithinArea('fxa-toolbar-menu-button',0);
 assert.equal(f.c.execute(`customize:${s.revision}:overflow-layout:none`),2);
 assert.equal(f.c.execute(`customize:${f.state().revision}:overflow-layout:none`),1);
 assert.equal(f.c.nativeOverflow.size,0);
});


test('palette drops insert through Firefox only while editing, at a live target',()=>{
 const f=fixture();f.c.execute(f.command('profile','remove'));
 assert.equal(f.c.execute(f.command('profile','insert','back')),2);
 f.c.execute('customize:editor:open');
 const captured=f.command('profile','insert','back');
 assert.equal(f.c.execute(captured),1);assert.equal(f.order()[0],'fxa-toolbar-menu-button');
 assert.equal(f.c.execute(captured),2);
 assert.equal(f.c.execute(f.command('new-spring','insert','address')),1);
 const s=f.state(),spring=s.items.find(i=>i.kind==='spring' && i.placed);
 assert.equal(f.order()[f.order().indexOf('urlbar-container')-1],spring.control);
 const bad=f.command('new-spring','insert')+':widget-999';const n=f.calls.length;
 assert.equal(f.c.execute(bad),2);assert.equal(f.calls.length,n);
 f.c.execute('customize:editor:close');assert.equal(f.c.execute(f.command('new-spring','insert','back')),2);
});

test('address is fixed and rejects direct reorder commands',()=>{
 const f=fixture();assert.equal(f.state().items.find(i=>i.control==='address').movable,false);
 for(const verb of ['end','before','remove'])assert.equal(f.c.execute(f.command('address',verb,verb==='before'?'back':undefined)),2);
});

for(const [widget,control] of [['history-panelmenu','history'],['bookmarks-menu-button','bookmarks']]) test(`${control} is an optional Firefox-owned customization widget`,()=>{
 const f=fixture();f.nodes.set(widget,{getAttribute:key=>key==='label'?'Localized '+control:''});
 const item=f.state().items.find(i=>i.control===control);
 assert.equal(item.placed,false);assert.equal(item.extension,false);assert.equal(item.label,'Localized '+control);
 assert.equal(f.c.execute(f.command(control,'add')),1);
 assert.equal(f.order().at(-1),widget);assert.equal(f.state().items.find(i=>i.control===control).placed,true);
});
