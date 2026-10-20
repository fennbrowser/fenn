import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const placesSource=readFileSync(new URL('../upstream/firefox/browser/components/places/PlacesUIUtils.sys.mjs',import.meta.url),'utf8');
const tagMethods=vm.runInNewContext('({'+placesSource.slice(placesSource.indexOf('  parseBookmarkTags(value)'),placesSource.indexOf('  _bookmarkToolbarTelemetryListening:'))+'})');
const code=readFileSync(new URL('../upstream/firefox/browser/base/content/WinUIBookmarkPanel.sys.mjs',import.meta.url),'utf8').replace('export class WinUIBookmarkPanel','this.Panel=class WinUIBookmarkPanel');
function fixture(){
  const calls=[],prefs=new Map(),records=new Map([['toolbar',{guid:'toolbar',type:2,title:'Toolbar',parentGuid:'root'}],['menu',{guid:'menu',type:2,title:'Menu',parentGuid:'root'}],['saved',{guid:'saved',type:1,url:new URL('https://example.com/'),title:'Old',parentGuid:'toolbar'}]]);
  let resultClosed=false,search;const sourceNode={};
  const realm=vm.createContext({console:{error:e=>calls.push(['error',String(e)])},Services:{obs:{addObserver(){},removeObserver(){}},prefs:{getCharPref:(k,f)=>prefs.get(k)??f,setCharPref:(k,v)=>prefs.set(k,v),setBoolPref:(k,v)=>prefs.set(k,v)},io:{newURI:spec=>({spec})}},Cc:{'@mozilla.org/autocomplete/search;1?name=places-tag-autocomplete':{createInstance:()=>({stopSearch(){},startSearch(text,_params,_previous,observer){search={text,observer};}})}},Ci:{nsIAutoCompleteSearch:{}}});
  vm.runInContext(code,realm);
  class State {
    constructor(options){this.options=options;calls.push(['state',options]);}
    async _titleChanged(v){calls.push(['title',v]);}
    async _tagsChanged(v){calls.push(['tags',Array.from(v)]);}
    async _parentGuidChanged(v){calls.push(['parent',v]);}
    async save(){calls.push(['save']);}
  }
  const window={gBrowser:{selectedTab:{},selectedBrowser:{browsingContext:{currentWindowGlobal:{innerWindowId:7}}}},gDialogBox:{isOpen:false},
    document:{getElementById:()=>sourceNode},StarUI:{showForNewBookmarks:true,userHasTags:true,_autoCloseTimeout:3500,_storeRecentlyUsedFolder:async(...a)=>calls.push(['recent',...a]),showConfirmation:()=>calls.push(['hint'])},
    BookmarkingUI:{updateStarState:()=>calls.push(['star-refresh'])},PlacesUIUtils:{...tagMethods,getBestTitle:node=>node.title,storeRecentlyUsedBookmarkFolder:async(...a)=>calls.push(['recent',...a]),promptLocalization:{formatValueSync:()=>"No title"},BookmarkState:State,LAST_USED_FOLDERS_META_KEY:'recent',maxRecentFolders:5},
    PlacesTransactions:{Remove:guids=>({transact:async()=>calls.push(['remove',Array.from(guids)])}),NewFolder:info=>({transact:async()=>{records.set('created',{...info,guid:'created',type:2});return 'created';}})},
    PlacesUtils:{bookmarks:{getLocalizedTitle:folder=>folder.title,TYPE_FOLDER:2,userContentRoots:['toolbar','menu'],fetch:async(arg,cb)=>{if(typeof arg==='string')return records.get(arg);for(const row of records.values())if(row.url?.href===arg.url)cb?.(row);},fetchTags:async()=>[{name:'First'}]},tagging:{getTagsForURI:()=>['First']},metadata:{get:async()=>[]},nodeIsConcreteFolder:()=>true,getFolderContents:()=>({root:{childCount:1,containerOpen:true,getChild:()=>({bookmarkGuid:'menu',title:'Menu'}),set containerOpen(v){resultClosed=!v;}}})}};
  const toolbar={window,active:true,navigation:true,supported:()=>true,customization:{editorOpen:false},owner:{addressSession:{sessionId:9},invalidateAddress:()=>calls.push(['invalidate'])},omnibar:{visible:()=>true,entry:()=>({id:'action-1'})},menuAnchor:()=>({})};
  const panel=new realm.Panel(toolbar);panel.label=(id,attr)=>id+(attr?'-'+attr:'');
  const node={bookmarkGuid:'saved',title:'Old',parent:{bookmarkGuid:'toolbar'}};
  const open=async(isNew=false)=>{await panel.show(node,isNew,new URL('https://example.com/'));const request=panel.pending.request;assert.equal(panel.execute({command:`bookmark-panel:open:${request}:1`}),1);return panel.pending;};
  const input=(p,id,verb='invoke',changes={})=>({command:`bookmark-panel:${verb}:${p.session}:${p.revision}:${id}`,bookmark:{name:'Edited',tags:'One, Two',showEditor:true,...changes}});
  return {panel,toolbar,window,node,calls,prefs,records,open,input,closed:()=>resultClosed,search:()=>search,realm};
}
const settle=()=>new Promise(resolve=>setTimeout(resolve,0));

test('the original panel hook runs before lazy DOM construction',()=>{
 const source=readFileSync(new URL('../upstream/firefox/browser/base/content/browser-places.js',import.meta.url),'utf8');
 const body=source.slice(source.indexOf('  async showEditBookmarkPopup('));
 assert.ok(body.indexOf('nativeEditor.show(')<body.indexOf('this.panel.state'));
});
test('native preparation uses the shared BookmarkState without the original overlay',async()=>{
 const f=fixture();await f.open();const model=f.panel.snapshot();assert.equal(model.name,'Old');assert.equal(model.labels.title,'bookmarks-edit-bookmark');assert.equal(model.autoClose,3500);assert.ok(f.closed());assert.equal(f.calls.find(c=>c[0]==='state')[1].info.itemGuid,'saved');
});
test('disabled native presentation falls back before creating a draft',async()=>{
 const f=fixture();f.toolbar.active=false;assert.equal(await f.panel.show(f.node,true,new URL('https://example.com/')),false);assert.equal(f.panel.pending,undefined);
});
test('save delegates title, tags, folder metadata and preference ownership to Firefox',async()=>{
 const f=fixture(),p=await f.open();assert.equal(f.panel.execute(f.input(p,1)),1);await settle();assert.ok(f.calls.some(c=>c[0]==='save'));assert.deepEqual(f.calls.find(c=>c[0]==='tags'),['tags',['One','Two']]);assert.equal(f.prefs.get('browser.bookmarks.editDialog.showForNewBookmarks'),true);assert.equal(f.panel.pending,null);
});
test('Escape cancels both new and existing drafts without a Places transaction',async()=>{
 for(const isNew of [false,true]){const f=fixture(),p=await f.open(isNew);assert.equal(f.panel.execute(f.input(p,2,'escape')),1);await settle();assert.ok(!f.calls.some(c=>['save','remove'].includes(c[0])));assert.equal(f.panel.pending,null);assert.equal(f.calls.some(c=>c[0]==='star-refresh'),isNew);}
});
test('Remove targets only still-matching captured GUIDs and is consumed once',async()=>{
 const f=fixture(),p=await f.open();p.guids.push('changed');f.records.set('changed',{guid:'changed',url:new URL('https://other.example/')});const action=f.input(p,2);assert.equal(f.panel.execute(action),1);assert.equal(f.panel.execute(action),2);await settle();assert.deepEqual(f.calls.find(c=>c[0]==='remove'),['remove',['saved']]);
});
test('new Cancel never inserts or removes any bookmark',async()=>{
 const f=fixture(),p=await f.open(true);assert.equal(f.panel.execute(f.input(p,2)),1);await settle();assert.ok(!f.calls.some(c=>['save','remove'].includes(c[0])));
});
test('stale revisions, selected documents and malformed text cannot mutate a draft',async()=>{
 const f=fixture(),p=await f.open();assert.equal(f.panel.execute({...f.input(p,1),command:'bookmark-panel:invoke:1:99:1'}),2);assert.equal(f.panel.execute(f.input(p,1,'invoke',{name:'x'.repeat(4097)})),2);f.window.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId=8;assert.equal(f.panel.execute(f.input(p,1)),2);assert.ok(!f.calls.some(c=>c[0]==='save'));
});
test('tag search delegates the entire comma-separated input and rejects late results',async()=>{
 const f=fixture(),p=await f.open();assert.equal(f.panel.execute(f.input(p,7)),1);await settle();assert.equal(f.search().text,'One, Two');f.search().observer.onSearchResult(null,{matchCount:1,getValueAt:()=> 'One, Two more'});assert.equal(p.suggestions[0],'One, Two more');f.panel.dismiss();f.search().observer.onSearchResult(null,{matchCount:1,getValueAt:()=> 'late'});assert.equal(p.suggestions[0],'One, Two more');
});
test('a failed save reopens only its original still-current target with an error',async()=>{
 const f=fixture(),p=await f.open();f.records.delete('saved');assert.equal(f.panel.execute(f.input(p,1)),1);await settle();assert.equal(f.panel.pending,p);assert.equal(p.session,0);assert.ok(p.error);assert.ok(p.request>1);
});


test('both presenters share original tag parsing and checkbox semantics',()=>{
 assert.deepEqual(Array.from(tagMethods.parseBookmarkTags(' , One, Two ,, ')),['One','Two']);
 assert.equal(tagMethods.toggleBookmarkTag('One, Two','One',false),'Two');
 assert.equal(tagMethods.toggleBookmarkTag('One','One',true),'One');
 assert.equal(tagMethods.toggleBookmarkTag('One','one',true),'One, one');
});
test('the all-tags selector uses bounded stable opaque choices and shared toggle semantics',async()=>{
 const f=fixture(),p=await f.open();assert.equal(f.panel.execute(f.input(p,8)),1);await settle();
 assert.ok(p.tagOptions.some(row=>row.label==='First'));
 const choice=p.tagOptions.find(row=>row.label==='First');
 assert.equal(f.panel.execute(f.input(p,choice.id)),1);await settle();
 assert.ok(f.window.PlacesUIUtils.parseBookmarkTags(p.tags).includes('First'));assert.equal(p.tagSelection,1);
 assert.equal(f.panel.execute(f.input(p,10)),2);
});

test('context dismissal saves the captured draft without retargeting a newly selected document',async()=>{
 const f=fixture(),p=await f.open();f.panel.execute(f.input(p,7));await settle();
 f.window.gBrowser.selectedBrowser.browsingContext.currentWindowGlobal.innerWindowId=8;
 f.panel.snapshot();await settle();assert.equal(f.panel.pending,null);assert.ok(f.calls.some(c=>c[0]==='save'));assert.equal(p.state.options.info.itemGuid,'saved');
});
test('unchanged existing fields do not overwrite concurrent Places updates',async()=>{
 const f=fixture(),p=await f.open();f.panel.execute(f.input(p,1,'invoke',{name:p.name,tags:p.tags}));await settle();
 assert.ok(!f.calls.some(c=>c[0]==='title' || c[0]==='tags'));assert.ok(f.calls.some(c=>c[0]==='save'));
});
test('remembered field names remain compatible with the retained overlay',async()=>{
 const f=fixture(),p=await f.open();f.panel.execute(f.input(p,7));await settle();assert.equal(f.prefs.get('browser.bookmarks.editDialog.firstEditField'),'namePicker');
});

test('in-flight folder work never publishes partial data at an old revision',async()=>{
 const f=fixture(),p=await f.open();const before=JSON.stringify(f.panel.snapshot());
 let complete;const original=f.window.PlacesUtils.bookmarks.fetch;
 f.window.PlacesUtils.bookmarks.fetch=(arg,callback)=>arg==='menu'?new Promise(resolve=>{complete=()=>original(arg,callback).then(resolve);}):original(arg,callback);
 const choice=p.roots.find(row=>row.label==='Menu');f.panel.execute(f.input(p,choice.id,'invoke',{name:p.name,tags:p.tags}));
 assert.equal(JSON.stringify(f.panel.snapshot()),before);complete();await settle();
 // The staged reply may still be fetching that root for its shortcut list.
 if(p.busy && complete){complete();await settle();}
 assert.equal(p.revision,2);assert.equal(f.panel.snapshot().folder,'Menu');
});
test('rejected action IDs cannot update the draft or remembered edit preference',async()=>{
 const f=fixture(),p=await f.open();assert.equal(f.panel.execute(f.input(p,99999)),2);assert.equal(p.name,'Old');assert.equal(f.prefs.size,0);
});

test('an old preparation cannot dismiss a replacement opening',async()=>{
 const f=fixture(),original=f.window.PlacesUtils.bookmarks.fetch;let release,first=true;
 f.window.PlacesUtils.bookmarks.fetch=(arg,cb)=>typeof arg==='object' && first ? (first=false,new Promise(resolve=>{release=()=>original(arg,cb).then(resolve);})) : original(arg,cb);
 const preparing=f.panel.show(f.node,false,new URL('https://example.com/'));f.panel.dismiss();
 await f.panel.show(f.node,false,new URL('https://example.com/'));const replacement=f.panel.pending;
 release();await preparing;assert.equal(f.panel.pending,replacement);assert.ok(replacement.ready);
});
test('oversized editable data uses the original presenter without truncating a draft',async()=>{
 const f=fixture();f.window.PlacesUtils.tagging.getTagsForURI=()=>['x'.repeat(4097)];
 assert.equal(await f.panel.show(f.node,false,new URL('https://example.com/')),false);assert.equal(f.panel.pending,null);assert.ok(!f.calls.some(c=>c[0]==='state'));
});


test('a disappearing folder leaves the last valid choices available for recovery',async()=>{
 const f=fixture(),p=await f.open(),choice=p.roots.find(row=>row.label==='Menu');f.records.delete('menu');
 f.panel.execute(f.input(p,choice.id));await settle();assert.equal(p.folder,'toolbar');assert.ok(p.error);assert.ok(p.choices.has(choice.id));
 const toolbar=p.roots.find(row=>row.label==='Toolbar');assert.equal(f.panel.execute(f.input(p,toolbar.id)),1);await settle();assert.equal(p.folder,'toolbar');assert.equal(p.error,'');
});
test('large folder and tag collections remain reachable through bounded pages',async()=>{
 const f=fixture();f.window.PlacesUtils.getFolderContents=()=>({root:{childCount:65,containerOpen:true,getChild:i=>({bookmarkGuid:'folder'+i,title:'Folder '+i})}});
 f.window.PlacesUtils.bookmarks.fetchTags=async()=>Array.from({length:33},(_,i)=>({name:'Tag '+String(i).padStart(2,'0')}));
 const p=await f.open();assert.equal(p.folders.length,64);assert.equal(p.next,true);
 f.panel.execute(f.input(p,5));await settle();assert.equal(p.folders.length,1);assert.equal(p.folders[0].label,'Folder 64');assert.equal(p.previous,true);assert.equal(p.next,false);
 f.panel.execute(f.input(p,8));await settle();assert.equal(p.tagOptions.length,32);assert.equal(p.tagNext,true);
 f.panel.execute(f.input(p,10));await settle();assert.ok(p.tagOptions.some(row=>row.label==='Tag 32'));assert.equal(p.tagNext,false);
 f.panel.execute(f.input(p,9));await settle();assert.equal(p.tagPage,0);
});


test('an obsolete failed preparation cannot open the retained presenter over a replacement',async()=>{
 const f=fixture(),original=f.window.PlacesUtils.bookmarks.fetch;let reject,first=true;
 f.window.PlacesUtils.bookmarks.fetch=(arg,cb)=>typeof arg==='object' && first ? (first=false,new Promise((_resolve,no)=>{reject=no;})) : original(arg,cb);
 const preparing=f.panel.show(f.node,false,new URL('https://example.com/'));f.panel.dismiss();
 await f.panel.show(f.node,false,new URL('https://example.com/'));const replacement=f.panel.pending;
 reject(new Error('Old folder lookup failed'));assert.equal(await preparing,true);assert.equal(f.panel.pending,replacement);
});


test('a new draft finishes saving after window teardown without reading destroyed browser state',async()=>{
 const f=fixture();await f.open(true);f.panel.close();f.window.gBrowser=null;f.window.PlacesUtils=f.window.PlacesUIUtils=f.window.PlacesTransactions=null;await settle();
 assert.ok(f.calls.some(c=>c[0]==='save'));assert.ok(!f.calls.some(c=>c[0]==='error' || c[0]==='hint'));
});


test('a tag checkbox cannot overflow the native editor text bound',async()=>{
 const f=fixture(),p=await f.open();f.panel.execute(f.input(p,8));await settle();
 const first=p.tagOptions.find(row=>row.label==='First');f.panel.execute(f.input(p,first.id,'invoke',{name:p.name,tags:'x'.repeat(4093)}));await settle();
 assert.equal(p.tags.length,4093);assert.ok(p.error);assert.equal(p.tagSelection,0);assert.equal(f.panel.snapshot().tags.length,4093);
});
