"""Prepare the pinned compatibility corpus; no browser defaults or shipping assets.

--download explicitly fetches the immutable files named by the checked-in lock.
Unknown directives/tokens/includes fail instead of silently loading both branches.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
TOKENS={'env_firefox':True,'env_chromium':False,'env_safari':False,'env_mobile':False,
        'cap_html_filtering':False,'cap_ipaddress':False,'ext_ubol':False}
INCLUDES={**{f'filters-{y}.txt':f'ublock-filters-{y}.txt' for y in range(2020,2027)},
          'filters-general.txt':'ublock-filters-general.txt','ubo-link-shorteners.txt':'ublock-link-shorteners.txt'}


def prepare(folder, download=False, lock_path=None):
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=True)
    lock=json.loads(Path(lock_path or ROOT/'tests/fixtures/adblock-corpus.json').read_text(encoding='utf-8'))
    assets={}
    for entry in lock:
        path=folder/entry['name']
        if download and not path.exists():
            with urllib.request.urlopen(entry['url'],timeout=45) as response:
                data=response.read(4*1024*1024+1)
            assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256'], entry['name']
            path.write_bytes(data)
        data=path.read_bytes()
        assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256'], entry['name']
        assets[entry['name']]=data.decode('utf-8')
    stats={'tokens':TOKENS,'files':[], 'excluded_lines':0}
    def expand(name, ancestry=()):
        if name in ancestry: raise ValueError('include cycle')
        stats['files'].append(name)
        output=[]; stack=[]; active=True
        for line in assets[name].splitlines():
            if line.startswith('!#if '):
                expression=line[5:].strip()
                if not re.fullmatch('!?[a-z_]+',expression): raise ValueError(line)
                token=expression; negated=token.startswith('!'); token=token.lstrip('!')
                if not re.fullmatch('[a-z_]+',token) or token not in TOKENS: raise ValueError(line)
                branch=TOKENS[token] != negated
                stack.append([active,branch,False]);active=active and branch
            elif line=='!#else':
                if not stack or stack[-1][2]: raise ValueError('unmatched/duplicate else')
                stack[-1][2]=True;active=stack[-1][0] and not stack[-1][1]
            elif line=='!#endif':
                if not stack: raise ValueError('unmatched endif')
                active=stack.pop()[0]
            elif line.startswith('!#include '):
                if active: output.extend(expand(INCLUDES[line[10:].strip()],(*ancestry,name)))
            elif line.startswith('!#'): raise ValueError('unsupported directive: '+line)
            elif active: output.append(line)
            else: stats['excluded_lines']+=1
        if stack: raise ValueError('unterminated conditional')
        return output
    rules='\n'.join(line for name in ['easylist.txt','easyprivacy.txt','ublock-filters.txt','ublock-unbreak.txt'] for line in expand(name))
    primary=json.loads(assets['ubo-scriptlets.json'])
    names={name for resource in primary for name in [resource['name'],*resource.get('aliases',[])]}
    supplementary=json.loads(assets['resources.json'])
    stats['resource_overrides']=[r['name'] for r in supplementary if names.intersection([r['name'],*r.get('aliases',[])])]
    # The generated uBO implementation wins explicitly, never by the engine's
    # silent duplicate rejection. Preserve the decision in the corpus receipt.
    resources=primary+[r for r in supplementary if not names.intersection([r['name'],*r.get('aliases',[])])]
    config={'version':1,'rules':rules,'resources':resources}
    data=json.dumps(config,ensure_ascii=False).encode('utf-8')
    assert len(data)<=10*1024*1024, len(data)
    (folder/'configuration.json').write_bytes(data)
    stats.update(config_bytes=len(data),config_sha256=hashlib.sha256(data).hexdigest(),rule_lines=len(rules.splitlines()),resources=len(resources),assets=lock)
    (folder/'preparation.json').write_text(json.dumps(stats,indent=2),encoding='utf-8')
    return stats


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--folder',required=True);p.add_argument('--download',action='store_true')
    args=p.parse_args();s=prepare(args.folder,args.download);print(json.dumps({k:v for k,v in s.items() if k!='assets'},indent=2))
