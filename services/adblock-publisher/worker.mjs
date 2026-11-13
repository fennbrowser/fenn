const PREFIX = 'adblock/stable-v1/';
const IMMUTABLE = /^(metadata\/[1-9][0-9]{0,15}\.(root|targets|snapshot)\.json|targets\/[0-9a-f]{64}\.(descriptor\.json|rules\.txt|provenance\.json))$/;
const TIMESTAMP = 'metadata/timestamp.json';
const reply = (text, status) => new Response(text, {status, headers:{'Cache-Control':'no-store'}});
async function sha(bytes) { return [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map(b=>b.toString(16).padStart(2,'0')).join(''); }
async function authorized(request, secret) {
  if (!secret || secret.length < 32) return false;
  const header = request.headers.get('Authorization') || '';
  if (header.length > 256) return false;
  const a = await sha(new TextEncoder().encode(header));
  const b = await sha(new TextEncoder().encode('Bearer '+secret));
  let diff=0; for(let i=0;i<a.length;i++) diff |= a.charCodeAt(i)^b.charCodeAt(i);
  return diff===0;
}
export default {
 async fetch(request, env) {
  const url=new URL(request.url);
  if(url.search || !url.pathname.startsWith('/'+PREFIX)) return reply('Not found',404);
  const name=url.pathname.slice(PREFIX.length+1);
  const archive=/^archives\/[1-9][0-9]{0,15}\.zip$/.test(name);
  if(name!==TIMESTAMP && !IMMUTABLE.test(name) && !archive) return reply('Not found',404);
  const key=PREFIX+name;
  const cache=archive?'private, no-store':name===TIMESTAMP?'no-store, max-age=0':'public, max-age=31536000, immutable';
  if(archive && !await authorized(request,env.PUBLISH_TOKEN)) return reply('Unauthorized',401);
  if(request.method==='GET' || request.method==='HEAD') {
   const object=await env.FILTERS.get(key);
   if(!object) return reply('Not found',404);
   return new Response(request.method==='HEAD'?null:object.body,{headers:{'Content-Type':'application/octet-stream','Cache-Control':cache,'ETag':object.httpEtag,'X-Content-Type-Options':'nosniff'}});
  }
  if(request.method!=='PUT') return reply('Method not allowed',405);
  if(!await authorized(request,env.PUBLISH_TOKEN)) return reply('Unauthorized',401);
  const limit=archive?32*1024*1024:name.endsWith('.rules.txt')?10*1024*1024:1024*1024;
  const length=Number(request.headers.get('Content-Length'));
  if(!Number.isSafeInteger(length) || length<=0 || length>limit) return reply('Body length required and bounded',413);
  const reader=request.body?.getReader(); if(!reader) return reply('Missing body',400);
  const chunks=[];let size=0;
  while(true) { const {done,value}=await reader.read();if(done)break;size+=value.length;if(size>limit){await reader.cancel();return reply('Too large',413);}chunks.push(value); }
  if(size!==length) return reply('Length mismatch',400);
  const bytes=new Uint8Array(size);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}
  const digest=await sha(bytes);
  if(request.headers.get('X-Content-SHA256')!==digest) return reply('Hash mismatch',400);
  if(name.startsWith('targets/') && name.slice(8,72)!==digest) return reply('Target identity mismatch',400);
  let onlyIf={etagDoesNotMatch:'*'};
  if(name===TIMESTAMP) {
   const match=request.headers.get('If-Match');
   if(match && /^"[0-9a-f]{32}"$/.test(match)) onlyIf={etagMatches:match.slice(1,-1)};
   else if(request.headers.get('If-None-Match')!=='*') return reply('Timestamp precondition required',428);
  }
  const object=await env.FILTERS.put(key,bytes,{onlyIf,sha256:await crypto.subtle.digest('SHA-256',bytes),httpMetadata:{contentType:'application/octet-stream',cacheControl:cache},customMetadata:{sha256:digest}});
  if(!object) {
   const existing=await env.FILTERS.head(key);
   // A retry after an ambiguous success is safe only for identical bytes.
   if(existing?.customMetadata?.sha256===digest) return new Response(null,{status:200,headers:{ETag:existing.httpEtag,'Cache-Control':'no-store'}});
   return reply('Publication conflict',412);
  }
  return new Response(null,{status:201,headers:{ETag:object.httpEtag,'Cache-Control':'no-store'}});
 }
};
