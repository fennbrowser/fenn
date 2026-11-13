"""Hosted production refresh, independent verification, immutable upload and timestamp CAS.

Credentials come only from environment secrets. No root private key is accepted.
Default output contains public audit data and a source archive, never secrets.
"""
import argparse,base64,datetime as dt,hashlib,io,json,os,re,runpy,subprocess,tempfile,time,types,urllib.error,urllib.request,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BASE='https://updates.fennbrowser.com/adblock/stable-v1/'
VERIFY=ROOT/'build/fenn-adblock-trust/release/fenn-adblock-trust.exe'
PREP=runpy.run_path(str(ROOT/'scripts/Prepare-FennAdblockProduction.py'))
SHARED=runpy.run_path(str(ROOT/'scripts/FennAdblockPublishing.py'))
def sha(raw):return hashlib.sha256(raw).hexdigest()
def dump(path,value):Path(path).write_text(json.dumps(value,indent=2),encoding='utf-8')
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self,*args,**kwargs):raise ValueError('Publication redirects are forbidden')
class Remote:
 def __init__(self,token):self.token=token;self.opener=urllib.request.build_opener(NoRedirect)
 def request(self,name,*,body=None,etag=None,private=False,missing=False):
  if not re.fullmatch(r'(metadata/(timestamp|[1-9][0-9]{0,15}\.(root|snapshot|targets))\.json|targets/[0-9a-f]{64}\.(rules\.txt|descriptor\.json|provenance\.json)|archives/[1-9][0-9]{0,15}\.zip)',name):raise ValueError('Invalid remote object')
  headers={'User-Agent':'Fenn-filter-production-publisher/1','Cache-Control':'no-cache'}
  if body is not None or private:headers['Authorization']='Bearer '+self.token
  if body is not None:
   headers.update({'X-Content-SHA256':sha(body),'Content-Type':'application/octet-stream'})
   if name=='metadata/timestamp.json':headers['If-Match' if etag else 'If-None-Match']=etag or '*'
  request=urllib.request.Request(BASE+name,data=body,headers=headers,method='PUT' if body is not None else 'GET')
  for attempt in range(3):
   try:
    with self.opener.open(request,timeout=60) as response:
     limit=32*1024*1024 if name.startswith('archives/') else 10*1024*1024
     data=response.read(limit+1)
     if len(data)>limit:raise ValueError('Remote object bound')
     return data,response.headers.get('ETag')
   except urllib.error.HTTPError as error:
    if error.code==404 and missing:return None,None
    if error.code<500 or attempt==2:raise
   except (urllib.error.URLError,TimeoutError):
    if attempt==2:raise
   time.sleep(2**attempt)

def version(metadata):
 n=metadata['signed']['version']
 if type(n) is not int or not 0<n<=2**53-1:raise ValueError('Metadata version bound')
 return n

def current(remote,anchor,folder,initialize=False):
 folder.mkdir();repo=folder/'repository';(repo/'metadata').mkdir(parents=True);(repo/'targets').mkdir()
 timestamp,etag=remote.request('metadata/timestamp.json',missing=True)
 root,_=remote.request('metadata/1.root.json',missing=True)
 if root is not None and root!=anchor.read_bytes():raise ValueError('Served production authority differs')
 if timestamp is None:
  if not initialize:raise ValueError('Empty feed requires explicit initial publication')
  return {},None
 if root is None:raise ValueError('Published timestamp lacks root')
 if not etag or not re.fullmatch(r'"[0-9a-f]{32}"',etag):raise ValueError('Missing timestamp CAS identity')
 (repo/'metadata/1.root.json').write_bytes(root);(repo/'metadata/timestamp.json').write_bytes(timestamp)
 parsed=json.loads(timestamp);seq=version(parsed)
 # This publisher authors matching metadata versions; reject other layouts.
 if parsed['signed']['meta']['snapshot.json']['version']!=seq:raise ValueError('Snapshot version mismatch')
 snapshot,_=remote.request(f'metadata/{seq}.snapshot.json');(repo/f'metadata/{seq}.snapshot.json').write_bytes(snapshot)
 snap=json.loads(snapshot)
 if version(snap)!=seq or snap['signed']['meta']['targets.json']['version']!=seq:raise ValueError('Targets version mismatch')
 targets,_=remote.request(f'metadata/{seq}.targets.json');(repo/f'metadata/{seq}.targets.json').write_bytes(targets)
 target=json.loads(targets)
 if version(target)!=seq or set(target['signed']['targets'])!={'descriptor.json','rules.txt','provenance.json'}:raise ValueError('Unexpected targets')
 for name,meta in target['signed']['targets'].items():
  identity=meta['hashes']['sha256']
  if not re.fullmatch('[0-9a-f]{64}',identity):raise ValueError('Invalid target hash')
  body,_=remote.request(f'targets/{identity}.{name}')
  if sha(body)!=identity or len(body)!=meta['length']:raise ValueError('Remote target mismatch')
  (repo/f'targets/{identity}.{name}').write_bytes(body)
 verified=folder/'verified'
 subprocess.run([str(VERIFY),str(anchor),str(repo),str(folder/'trust-state'),str(verified)],check=True,capture_output=True)
 return {'version':seq,'root_sha256':sha(root),'bundle':str(verified)},etag

def get(url):
 with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Fenn-filter-production-publisher/1'}),timeout=60) as reply:
  if not reply.url.startswith('https://'):raise ValueError('HTTPS source required')
  data=reply.read(4*1024*1024+1)
 if len(data)>4*1024*1024:raise ValueError('Source bound')
 return data

def refresh(folder):
 folder.mkdir();entries=json.loads((ROOT/'tests/fixtures/adblock-corpus.json').read_bytes())
 commit=json.loads(get('https://api.github.com/repos/uBlockOrigin/uAssets/commits/master'))['sha']
 if not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('Invalid upstream commit')
 for entry in entries:
  name=entry['name']
  if name.endswith('.txt'):
   entry['url']=SHARED['source_url'](name,commit)
   data=get(entry['url']);entry.update(bytes=len(data),sha256=sha(data))
  else:
   data=get(entry['url'])
   if len(data)!=entry['bytes'] or sha(data)!=entry['sha256']:raise ValueError('Pinned resource changed')
  (folder/name).write_bytes(data)
 dump(folder/'source-lock.json',entries)
 return commit

def archive_sources(folder):
 buffer=io.BytesIO()
 entries=json.loads((folder/'source-lock.json').read_bytes())
 with zipfile.ZipFile(buffer,'w',zipfile.ZIP_DEFLATED) as archive:
  for name in ['source-lock.json',*[e['name'] for e in entries]]:archive.write(folder/name,name)
 data=buffer.getvalue()
 if len(data)>32*1024*1024:raise ValueError('Source archive bound')
 return data

def restore_sources(data,expected,folder):
 if not re.fullmatch('[0-9a-f]{64}',expected or '') or sha(data)!=expected:raise ValueError('Explicit rollback archive hash required')
 folder.mkdir()
 with zipfile.ZipFile(io.BytesIO(data)) as archive:
  entries=archive.infolist();names=[e.filename for e in entries]
  if len(entries)>64 or len(names)!=len(set(names)) or sum(e.file_size for e in entries)>64*1024*1024:raise ValueError('Archive bounds')
  for entry in entries:
   if not re.fullmatch(r'[A-Za-z0-9_.-]+',entry.filename) or entry.filename in ('.','..') or entry.file_size>4*1024*1024:raise ValueError('Archive entry')
  lock=json.loads(archive.read('source-lock.json'))
  if set(names)!={'source-lock.json',*[e['name'] for e in lock]}:raise ValueError('Unknown archive content')
  for entry in entries:(folder/entry.filename).write_bytes(archive.read(entry))

def promote(remote,record,anchor,etag,archive):
 repo=Path(record['repository'])
 names=[item['path'] for item in record['upload_plan']]
 if not names or names[-1]!='metadata/timestamp.json' or len(names)!=len(set(names)):raise ValueError('Timestamp must be last and paths unique')
 if any(not re.fullmatch(r'(metadata/([1-9][0-9]{0,15}\.(root|snapshot|targets)|timestamp)\.json|targets/[0-9a-f]{64}\.(rules\.txt|descriptor\.json|provenance\.json))',name) for name in names):raise ValueError('Unexpected upload path')
 # Reverify exact local bytes immediately before sending any public metadata.
 with tempfile.TemporaryDirectory() as temp:
  subprocess.run([str(VERIFY),str(anchor),str(repo),str(Path(temp)/'state'),str(Path(temp)/'verified')],check=True,capture_output=True)
 remote.request(f"archives/{record['version']}.zip",body=archive)
 for item in record['upload_plan']:
  path=item['path'];body=(repo/path).read_bytes()
  if sha(body)!=item['sha256'] or len(body)!=item['bytes']:raise ValueError('Candidate changed after verification')
  remote.request(path,body=body,etag=etag if path=='metadata/timestamp.json' else None)
  served,_=remote.request(path)
  if served!=body:raise ValueError('Served publication differs; do not roll back timestamp')
 record.update(uploaded=True,archive_sha256=sha(archive))
 return record

SECRETS=('FENN_ADBLOCK_PUBLISH_TOKEN','FENN_ADBLOCK_ONLINE_KEY_B64')
def take_secrets():
 # Remove secrets from the inherited environment before any subprocess starts:
 # the parser handles freshly downloaded lists and the verifier served metadata.
 return {name:os.environ.pop(name) for name in SECRETS}

def run(args):
 secrets=take_secrets()
 out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=False)
 policy=json.loads(os.environ['FENN_ADBLOCK_PRODUCTION_POLICY']);PREP['validate_policy'](policy)
 root=base64.b64decode(os.environ['FENN_ADBLOCK_PRODUCTION_ROOT_B64'],validate=True)
 if sha(root)!=policy['root_sha256']:raise ValueError('Root pin mismatch')
 anchor=out/'trusted-root.json';anchor.write_bytes(root)
 remote=Remote(secrets['FENN_ADBLOCK_PUBLISH_TOKEN'])
 previous,etag=current(remote,anchor,out/'remote',args.initialize)
 previous_path=out/'previous.json';dump(previous_path,previous)
 seq=max(int(time.time()),previous.get('version',0)+1);sources=out/'sources'
 if args.rollback_version:
  if not re.fullmatch('[1-9][0-9]{0,15}',args.rollback_version):raise ValueError('Invalid rollback version')
  data,_=remote.request(f'archives/{args.rollback_version}.zip',private=True)
  restore_sources(data,args.rollback_sha256,sources);commit=None
 else:commit=refresh(sources)
 archive=archive_sources(sources)
 with tempfile.TemporaryDirectory(prefix='fenn-online-signing-') as temp:
  key=Path(temp)/'online.pkcs8';key.write_bytes(base64.b64decode(secrets.pop('FENN_ADBLOCK_ONLINE_KEY_B64'),validate=True))
  policy_path=out/'policy.json';dump(policy_path,policy)
  options=types.SimpleNamespace(policy=policy_path,root=anchor,key_file=key,source=sources,source_lock=sources/'source-lock.json',sequence=seq,
   output=out/'candidate',previous=previous_path if previous else None,parser=ROOT/'build/fenn-adblock/release/examples/publish_rules.exe',
   signer=ROOT/'build/fenn-adblock-trust/release/fenn-adblock-sign.exe',verifier=VERIFY,
   approve_review_sha256=args.approve_review_sha256,rollback=bool(args.rollback_version))
  try:record=PREP['prepare'](options)
  except Exception as error:
   if type(error).__name__!='HeldForReview':raise
   held={'held':True,'version':seq,'source_commit':commit,**error.review}
   dump(out/'held.json',held)
   summary=os.environ.get('GITHUB_STEP_SUMMARY')
   if summary:
    with open(summary,'a',encoding='utf-8') as file:
     file.write('Held for review: '+'; '.join(held['hold'])+'\n\nReview `candidate/held-for-review.txt` in the run artifact, then rerun with `approve_review_sha256` = `'+held['sha256']+'`.\n')
   print(str(error));raise SystemExit(3)
 record.update(source_commit=commit,rollback_version=args.rollback_version or None)
 if args.upload:record=promote(remote,record,anchor,etag,archive)
 else:record['archive_sha256']=sha(archive)
 dump(out/'publication.json',record)
 print(json.dumps({key:record[key] for key in ('version','expires','uploaded','archive_sha256')},indent=2))

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True)
 parser.add_argument('--upload',action='store_true');parser.add_argument('--initialize',action='store_true')
 parser.add_argument('--rollback-version');parser.add_argument('--rollback-sha256')
 parser.add_argument('--approve-review-sha256',help='Publish a held candidate whose added rules hash to this value');run(parser.parse_args())
