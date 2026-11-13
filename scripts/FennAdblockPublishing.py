"""Publication gates and source locations shared by the trial and production publishers.

Imported by both; keep it free of state paths, credentials and side effects.
"""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

def source_url(name,commit):
    """Upstream URL of a locked .txt list at an exact uAssets commit."""
    if name in ('easylist.txt','easyprivacy.txt'):return f'https://easylist.to/easylist/{name}'
    upstream=name.removeprefix('ublock-') if name!='ublock-link-shorteners.txt' else 'ubo-link-shorteners.txt'
    return f'https://raw.githubusercontent.com/uBlockOrigin/uAssets/{commit}/filters/{upstream}'

# Rules that run code or rewrite pages rather than only blocking or hiding.
RISKY=re.compile(r'#\+js\(|[$,]redirect(?:-rule)?=|:remove(?:-attr|-class)?\(|:style\(|[$,]csp=|[$,]removeparam|[$,]replace=')
COSMETIC=re.compile(r'^([^#]*)#[@?$%]*#')
# Upstream adds roughly 4-33 risky rules and up to ~140 rules a day, all scoped to
# specific sites (14 days measured 2026-11-12). Hold anything well outside that.
HOLD_RISKY=60
HOLD_ADDED=500

class HeldForReview(Exception):
    def __init__(self,review):
        super().__init__('Held for review: '+'; '.join(review['hold'])+'. Approve with SHA-256 '+review['sha256'])
        self.review=review

def all_site(line):
    """Cosmetic/scriptlet rule without domains, or network rule without URL or domain scope."""
    if (m:=COSMETIC.match(line)):return m[1].strip() in ('','*')
    pattern,_,options=line.removeprefix('@@').partition('$')
    return pattern.strip() in ('','*','|','||') and not re.search(r'(^|,)(domain|from|to)=',options)

def review(rules,old):
    """Classify additions since the published rules. Approval binds to the exact added set."""
    added=sorted(rules-old)
    risky=[line for line in added if RISKY.search(line)]
    all_sites=[line for line in risky if all_site(line)]
    hold=[]
    if all_sites:hold.append(f'{len(all_sites)} new risky rules apply to every site')
    if len(risky)>HOLD_RISKY:hold.append(f'{len(risky)} new risky rules exceed {HOLD_RISKY}')
    if len(added)>HOLD_ADDED:hold.append(f'{len(added)} new rules exceed {HOLD_ADDED}')
    return {'added':len(added),'risky_count':len(risky),'risky':risky[:1000],'all_sites':all_sites[:1000],
        'hold':hold,'sha256':hashlib.sha256('\n'.join(added).encode()).hexdigest()}

def require_review(report,approved,output):
    """Raise HeldForReview unless the held diff was approved by its exact hash."""
    current=report.get('review')
    if not current or not current['hold'] or approved==current['sha256']:return
    lines=current['all_sites'] if current['all_sites'] else current['risky']
    (Path(output)/'held-for-review.txt').write_text('\n'.join(current['hold']+['',*lines])+'\n',encoding='utf-8')
    raise HeldForReview(current)

def compatibility(publication,previous):
    config=json.loads((publication/'fenn-adblock-assets/baseline.json').read_bytes())
    names={name for resource in config['resources'] for name in [resource['name'],*resource.get('aliases',[])]}
    rules={line for line in config['rules'].splitlines() if line and not line.startswith(('!','['))}
    old={line for line in (Path(previous['bundle'])/'rules.txt').read_text(encoding='utf-8').splitlines()
        if line and not line.startswith(('!','['))} if previous.get('bundle') else set()
    missing=Counter()
    for line in rules:
        scriptlet=re.search(r'#(?:@)?#\+js\(([^,)]+)',line)
        if scriptlet and scriptlet[1].strip()+'.js' not in names:missing[scriptlet[1].strip()]+=1
        redirect=re.search(r'(?:\$|,)redirect(?:-rule)?=([^,]+)',line)
        if redirect:
            name=redirect[1].rsplit(':',1)[0]
            if name!='none' and name not in names:missing[name]+=1
    excluded=json.loads((publication/'exclusions.json').read_bytes())
    report={'rules':len(rules),'previous_rules':len(old),'added':len(rules-old),'removed':len(old-rules),
        'missing_resources':dict(missing),'excluded':len(excluded),'exclusion_reasons':dict(Counter(x['reason'] for x in excluded)),
        'catalog':hashlib.sha256((publication/'fenn-adblock-assets/resources.json').read_bytes()).hexdigest()}
    if old:report['review']=review(rules,old)
    (publication/'compatibility.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    if missing:raise ValueError('List requires resources absent from the pinned catalog')
    if len(rules)<1000 or (old and len(rules)<len(old)*.8):raise ValueError('Unexpected filter-count collapse')
    if len(excluded)>max(100,len(rules)*.05):raise ValueError('Exclusion budget exceeded')
    return report
