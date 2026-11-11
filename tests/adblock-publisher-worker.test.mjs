import test from 'node:test';
import assert from 'node:assert/strict';
import worker from '../services/adblock-publisher/worker.mjs';
const token='test-only-token-with-at-least-32-characters';
const base='https://updates.fennbrowser.com/adblock/stable-v1/';
const digest=async bytes=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(bytes)))].map(b=>b.toString(16).padStart(2,'0')).join('');
function fixture(){
 const data=new Map();let serial=0;
 return {data,env:{PUBLISH_TOKEN:token,FILTERS:{
  async get(k){const v=data.get(k);return v&&{...v,body:v.bytes};},async head(k){return data.get(k)||null;},
  async put(k,bytes,options){const old=data.get(k);if(options.onlyIf.etagDoesNotMatch==='*'&&old || options.onlyIf.etagMatches && old?.etag!==options.onlyIf.etagMatches)return null;
   const etag=(++serial).toString(16).padStart(32,'0');const value={bytes,etag,httpEtag:'"'+etag+'"',customMetadata:options.customMetadata};data.set(k,value);return value;}
 }}};
}
async function put(f,name,bytes,headers={}) {return worker.fetch(new Request(base+name,{method:'PUT',headers:{Authorization:'Bearer '+token,'Content-Length':String(new TextEncoder().encode(bytes).length),'X-Content-SHA256':await digest(bytes),...headers},body:bytes}),f.env);}
test('unauthorized writes and unknown paths never touch storage',async()=>{
 const f=fixture();assert.equal((await put(f,'metadata/1.root.json','root',{Authorization:'wrong'})).status,401);
 assert.equal((await put(f,'unknown','body')).status,404);assert.equal(f.data.size,0);
});
test('immutable objects allow identical retry but reject replacement',async()=>{
 const f=fixture();assert.equal((await put(f,'metadata/1.root.json','root')).status,201);
 assert.equal((await put(f,'metadata/1.root.json','root')).status,200);
 assert.equal((await put(f,'metadata/1.root.json','other')).status,412);
 const get=await worker.fetch(new Request(base+'metadata/1.root.json'),f.env);assert.equal(await get.text(),'root');assert.match(get.headers.get('Cache-Control'),/immutable/);
});
test('timestamp compare-and-swap and ambiguous-success retry',async()=>{
 const f=fixture();const name='metadata/timestamp.json';assert.equal((await put(f,name,'one')).status,428);
 const first=await put(f,name,'one',{'If-None-Match':'*'});assert.equal(first.status,201);
 const headers={'If-Match':first.headers.get('ETag')};assert.equal((await put(f,name,'two',headers)).status,201);
 assert.equal((await put(f,name,'two',headers)).status,200);assert.equal((await put(f,name,'three',headers)).status,412);
 const response=await worker.fetch(new Request(base+name),f.env);assert.equal(await response.text(),'two');assert.match(response.headers.get('Cache-Control'),/no-store/);
});
test('content length, digest and target filename are enforced',async()=>{
 const f=fixture();assert.equal((await put(f,'metadata/1.root.json','root',{'Content-Length':'99999999'})).status,413);
 assert.equal((await put(f,'metadata/1.root.json','root',{'X-Content-SHA256':'bad'})).status,400);
 assert.equal((await put(f,'targets/'+'a'.repeat(64)+'.rules.txt','rules')).status,400);
 const name='targets/'+await digest('rules')+'.rules.txt';assert.equal((await put(f,name,'rules')).status,201);
});
test('missing secret fails closed and responses do not enable cross-origin writes',async()=>{
 const f=fixture();delete f.env.PUBLISH_TOKEN;assert.equal((await put(f,'metadata/1.root.json','root')).status,401);
 assert.equal((await worker.fetch(new Request(base+'metadata/timestamp.json',{method:'OPTIONS'}),f.env)).status,405);
});
test('rollback archives require authentication for reads and cannot be overwritten',async()=>{
 const f=fixture();assert.equal((await put(f,'archives/1.zip','archive')).status,201);
 assert.equal((await worker.fetch(new Request(base+'archives/1.zip'),f.env)).status,401);
 assert.equal((await worker.fetch(new Request(base+'archives/1.zip',{headers:{Authorization:'Bearer '+token}}),f.env)).status,200);
 assert.equal((await put(f,'archives/1.zip','replacement')).status,412);
});
