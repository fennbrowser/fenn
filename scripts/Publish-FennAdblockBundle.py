"""Publish deterministic offline development assets; no downloads or signing.

Input is a prepared corpus. Reverify its immutable sources and regenerate the
receipt before publication. Parser exclusions are review artifacts, not support
claims. Output directories must be new, so a failed run cannot replace a release.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = 'adblock-0.13.3/abi-2/offline-1'
def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
def target(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
def stage_assets(source, payload, dll, *, release=False, trusted_root=None, verifier=None):
    """Copy a verified development catalog into an application/package payload."""
    if release:
        raise ValueError('development adblock assets are not approved for release/Store')
    source, payload = Path(source), Path(payload)
    manifest = json.loads((source/'manifest.json').read_bytes())
    if (manifest.get('schema') != 1 or manifest.get('development_only') is not True or
            manifest.get('engine_contract') != CONTRACT):
        raise ValueError('invalid development asset manifest')
    data = {}
    for key, filename in [('baseline','baseline.json'),('resources','resources.json')]:
        path = source/filename
        if path.stat().st_size > 10*1024*1024:
            raise ValueError('asset exceeds runtime bound')
        data[key] = path.read_bytes()
        if target(data[key]) != manifest[key]:
            raise ValueError('asset hash mismatch')
    baseline = json.loads(data['baseline'])
    if baseline['version'] != 1 or canonical(baseline['resources']) != data['resources']:
        raise ValueError('asset catalog mismatch')
    if not Path(dll).is_file():
        raise FileNotFoundError(dll)
    root_bytes = None
    if trusted_root is not None:
        if Path(trusted_root).stat().st_size > 1024*1024:
            raise ValueError('trusted root exceeds bound')
        root_bytes = Path(trusted_root).read_bytes()
        root = json.loads(root_bytes)
        if root.get('signed',{}).get('_type') != 'root' or not root.get('signatures'):
            raise ValueError('expected signed TUF root metadata')
        if verifier is None or not Path(verifier).is_file():
            raise ValueError('trusted root requires built verifier')
        manifest['trust'] = {'root':target(root_bytes),'verifier':target(Path(verifier).read_bytes())}
    elif 'trust' in manifest:
        raise ValueError('trust metadata requires explicit root and verifier selection')
    destination = payload/'fenn-adblock-assets'
    destination.mkdir(exist_ok=False)
    for filename, raw in [('manifest.json',canonical(manifest)),('baseline.json',data['baseline']),('resources.json',data['resources'])]:
        (destination/filename).write_bytes(raw)
    shutil.copyfile(dll,payload/'FennAdblock.dll')
    if root_bytes is not None:
        (destination/'trusted-root.json').write_bytes(root_bytes)
        shutil.copyfile(verifier,payload/'fenn-adblock-trust.exe')
    return manifest

def stage_production_assets(source, payload, dll, *, verifier, release=False):
    policy = json.loads((ROOT/'configs/adblock/production-policy.json').read_bytes())
    anchor = ROOT/'configs/adblock/production-root.json'
    if hashlib.sha256(anchor.read_bytes()).hexdigest() != policy['root_sha256']:
        raise ValueError('production root pin mismatch')
    if hashlib.sha256((Path(source)/'resources.json').read_bytes()).hexdigest() != policy['catalog_sha256']:
        raise ValueError('production resource catalog mismatch')
    return stage_assets(source, payload, dll, release=release,
                        trusted_root=anchor, verifier=verifier)

def publish(config, receipt, output, sequence, parser):
    if not 0 < sequence <= 2**64-1:
        raise ValueError('invalid sequence')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(dir=output) as temp:
        source, result = Path(temp)/'input.json', Path(temp)/'result.json'
        source.write_bytes(config)
        subprocess.run([str(parser), str(source), str(result)], check=True)
        parsed = json.loads(result.read_bytes())
    baseline = canonical(parsed['configuration'])
    resources = canonical(parsed['configuration']['resources'])
    rules = parsed['configuration']['rules'].encode()
    exclusions = canonical(parsed['excluded'])
    capabilities = canonical(parsed['capabilities'])
    provenance = canonical({'schema':1, 'authentication':'none-development-only',
        'input':target(config), 'preparation':receipt, 'exclusions':target(exclusions), 'capabilities':parsed['capabilities'],
        'generator':{name:target((ROOT/name).read_bytes()) for name in (
            'scripts/Publish-FennAdblockBundle.py', 'scripts/Prepare-FennAdblockCorpus.py',
            'native/FennAdblock/examples/publish_rules.rs', 'native/FennAdblock/Cargo.toml',
            'native/FennAdblock/Cargo.lock', 'native/FennAdblock/src/capabilities.rs',
            'configs/adblock/capabilities.json')}})
    if len(baseline)>10*1024*1024 or len(provenance)>1024*1024:
        raise ValueError('runtime input bounds exceeded')
    descriptor = canonical({'schema':1, 'sequence':sequence, 'stream':'fenn-offline-v1',
        'engine_contract':CONTRACT, 'preprocessor_contract':'resolved-v1',
        'resource_catalog_sha256':target(resources)['sha256'],
        'rules':target(rules), 'provenance':target(provenance)})
    assets = output/'fenn-adblock-assets'; assets.mkdir()
    bundle = output/'bundle'; bundle.mkdir()
    for name, data in {'baseline.json':baseline, 'resources.json':resources,
        'manifest.json':canonical({'schema':1, 'development_only':True,
            'engine_contract':CONTRACT, 'baseline':target(baseline), 'resources':target(resources)})}.items():
        (assets/name).write_bytes(data)
    for name, data in {'descriptor.json':descriptor,'rules.txt':rules,'provenance.json':provenance}.items():
        (bundle/name).write_bytes(data)
    (output/'exclusions.json').write_bytes(exclusions)
    (output/'capabilities.json').write_bytes(capabilities)
    return {'baseline':target(baseline),'catalog':target(resources),'descriptor':target(descriptor),
            'excluded_rules':len(parsed['excluded']), 'capabilities':parsed['capabilities']}

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True); p.add_argument('--output',type=Path,required=True)
    p.add_argument('--sequence',type=int,required=True)
    args=p.parse_args()
    receipt=runpy.run_path(str(ROOT/'scripts/Prepare-FennAdblockCorpus.py'))['prepare'](args.corpus)
    print(json.dumps(publish((args.corpus/'configuration.json').read_bytes(),receipt,args.output,args.sequence,
        ROOT/'build/fenn-adblock/release/examples/publish_rules.exe'),indent=2))
