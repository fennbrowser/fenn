"""Prepare a verified production filter candidate. No upload, key creation or browser switch.

Supply independently pinned public policy and a protected online PKCS#8 key file.
Root keys never enter this process. Reissue archived inputs at a higher sequence
for rollback; never restore an older timestamp or sequence.
"""
import argparse,datetime as dt,hashlib,json,runpy,subprocess,tempfile
from pathlib import Path
from urllib.parse import urlsplit
ROOT=Path(__file__).resolve().parents[1]

def digest(data):return hashlib.sha256(data).hexdigest()
def load(path):return json.loads(Path(path).read_bytes())
def validate_policy(policy):
 url=urlsplit(policy['base_url'])
 if (url.scheme!='https' or url.hostname!='updates.fennbrowser.com' or url.port is not None or
     url.username or url.password or url.query or url.fragment or url.path!='/adblock/stable-v1/'):
  raise ValueError('Expected separate production endpoint https://updates.fennbrowser.com/adblock/stable-v1/')
 for key in ('root_sha256','catalog_sha256'):
  value=policy[key]
  if not isinstance(value,str) or len(value)!=64 or any(c not in '0123456789abcdef' for c in value):raise ValueError('Invalid policy hash')
 forbidden=policy.get('forbidden_key_ids')
 if not isinstance(forbidden,list) or not forbidden or any(not isinstance(k,str) or len(k)!=64 or any(c not in '0123456789abcdef' for c in k) for k in forbidden):raise ValueError('Trial authority key exclusions required')
 if policy.get('schema')!=1:raise ValueError('Unknown publication policy')

def prepare(args):
 policy=load(args.policy);validate_policy(policy)
 root=Path(args.root).read_bytes()
 if digest(root)!=policy['root_sha256']:raise ValueError('Production root pin mismatch')
 if set(json.loads(root)['signed']['keys']) & set(policy['forbidden_key_ids']):raise ValueError('Production must not reuse trial keys')
 previous=load(args.previous) if args.previous else {}
 if previous and previous['root_sha256']!=policy['root_sha256']:raise ValueError('Root rotation requires a separately verified migration')
 if not 0<args.sequence<=2**53-1 or args.sequence<=previous.get('version',0):raise ValueError('Publication sequence must increase')
 output=Path(args.output).resolve();output.mkdir(parents=True,exist_ok=False)
 source=output/'sources';source.mkdir()
 entries=load(args.source_lock);names=set()
 for entry in entries:
  name=entry['name']
  if not isinstance(name,str) or Path(name).name!=name or ':' in name or name in names:raise ValueError('Invalid locked source name')
  names.add(name);data=(Path(args.source)/name).read_bytes()
  if len(data)!=entry['bytes'] or digest(data)!=entry['sha256']:raise ValueError('Locked source mismatch')
  (source/name).write_bytes(data)
 lock=output/'source-lock.json';lock.write_text(json.dumps(entries,indent=2),encoding='utf-8')
 receipt=runpy.run_path(str(ROOT/'scripts/Prepare-FennAdblockCorpus.py'))['prepare'](source,lock_path=lock)
 publisher=runpy.run_path(str(ROOT/'scripts/Publish-FennAdblockBundle.py'))
 publication=output/'published'
 info=publisher['publish']((source/'configuration.json').read_bytes(),receipt,publication,args.sequence,Path(args.parser))
 if info['catalog']['sha256']!=policy['catalog_sha256']:raise ValueError('Production catalog contract changed; browser release required')
 shared=runpy.run_path(str(ROOT/'scripts/FennAdblockPublishing.py'))
 compatibility=shared['compatibility'](publication,previous)
 # Rollback reissues already-published sources selected by explicit archive hash.
 if not getattr(args,'rollback',False):shared['require_review'](compatibility,getattr(args,'approve_review_sha256',None),output)
 now=dt.datetime.now(dt.timezone.utc);expires=(now+dt.timedelta(days=7)).strftime('%Y-%m-%dT%H:%M:%SZ')
 repository=output/'repository';anchor=output/'trusted-root.json';anchor.write_bytes(root)
 request={'op':'publish','root':str(anchor),'root_sha256':policy['root_sha256'],'key_file':str(Path(args.key_file).resolve()),
  'bundle':str(publication/'bundle'),'output':str(repository),'version':args.sequence,'expires':expires}
 # Keep even key paths out of the reviewable candidate artifact.
 with tempfile.TemporaryDirectory(prefix='fenn-production-sign-') as temporary:
  path=Path(temporary)/'request.json';path.write_text(json.dumps(request),encoding='utf-8')
  subprocess.run([str(args.signer),str(path)],check=True,capture_output=True)
  verified=subprocess.run([str(args.verifier),str(anchor),str(repository),str(Path(temporary)/'state'),str(Path(temporary)/'verified')],check=True,capture_output=True,text=True)
 verification=json.loads(verified.stdout)
 files=sorted(p for p in repository.rglob('*') if p.is_file() and p.name!='timestamp.json')+[repository/'metadata/timestamp.json']
 plan=[{'path':p.relative_to(repository).as_posix(),'sha256':digest(p.read_bytes()),'bytes':p.stat().st_size,
  'cache_control':'no-store, max-age=0' if p.name=='timestamp.json' else 'public, max-age=31536000, immutable'} for p in files]
 record={'schema':1,'channel':'production','base_url':policy['base_url'],'root_sha256':policy['root_sha256'],
  'version':args.sequence,'expires':expires,'bundle':str(publication/'bundle'),'repository':str(repository),'catalog_sha256':info['catalog']['sha256'],
  'compatibility':compatibility,'verification':verification,'upload_plan':plan,'uploaded':False}
 (output/'publication.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
 return record

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('policy','root','source','source-lock','key-file','output'):p.add_argument('--'+name,required=True,type=Path)
 p.add_argument('--sequence',type=int,required=True);p.add_argument('--previous',type=Path)
 p.add_argument('--approve-review-sha256',help='Publish a held candidate whose added rules hash to this value')
 p.add_argument('--parser',type=Path,default=ROOT/'build/fenn-adblock/release/examples/publish_rules.exe')
 p.add_argument('--signer',type=Path,default=ROOT/'build/fenn-adblock-trust/release/fenn-adblock-sign.exe')
 p.add_argument('--verifier',type=Path,default=ROOT/'build/fenn-adblock-trust/release/fenn-adblock-trust.exe')
 print(json.dumps(prepare(p.parse_args()),indent=2))
