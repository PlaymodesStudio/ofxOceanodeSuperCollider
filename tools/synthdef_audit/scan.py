#!/usr/bin/env python3
"""Read-only, comment-aware inventory of SC source templates; no SC evaluation."""
import re, json, hashlib, csv, collections
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'output/pdf'
CREATOR=re.compile(r'~synthCreator\s*\.\s*(?:value\s*)?\(\s*"([^"\n]+)"')

def clean(s):
    out=list(s); i=0; depth=0; string=False
    while i<len(s):
        if depth:
            if s[i:i+2]=='/*': out[i:i+2]='  '; depth+=1;i+=2
            elif s[i:i+2]=='*/':out[i:i+2]='  ';depth-=1;i+=2
            else:
                if s[i]!='\n':out[i]=' '
                i+=1
        elif string:
            if s[i]=='\\':i+=2
            elif s[i]=='"':string=False;i+=1
            else:i+=1
        elif s[i]=='"':string=True;i+=1
        elif s[i:i+2]=='/*':out[i:i+2]='  ';depth=1;i+=2
        elif s[i:i+2]=='//':
            e=s.find('\n',i); e=len(s) if e<0 else e
            out[i:e]=' '*(e-i);i=e
        else:i+=1
    return ''.join(out)

def endparen(s,start,opening='(',closing=')'):
    depth=0; string=False;i=start
    while i<len(s):
        c=s[i]
        if string:
            if c=='\\':i+=2;continue
            if c=='"':string=False
        elif c=='"':string=True
        elif c==opening:depth+=1
        elif c==closing:
            depth-=1
            if depth==0:return i+1
        i+=1
    return len(s)

def norm(s):
    return ''.join(re.findall(r'"(?:\\.|[^"\\])*"|\S',s))

def details(s):
    params=list(dict.fromkeys(re.findall(r'OceanodeParameter\w*\s*\.\s*(?:ar|kr|ir)\s*\(\s*(?:\\(\w+)|"([^"\n]+)")',s)))
    params=[a or b for a,b in params]
    params=list(dict.fromkeys(params+re.findall(r'\\(\w+)\.(?:ar|kr|ir|tr)\s*\(',s)+re.findall(r'Oceanode(?:Buffer|Input|Output)\.(?:ar|kr|ir)\s*\(\s*\\(\w+)',s)))
    ugens=collections.Counter(re.findall(r'\b([A-Z]\w*)\.(?:ar|kr|ir|new)\s*\(',s))
    for u in list(ugens):
        if u.startswith('Oceanode') or u in ['In','Out','K2A','A2K','DC','Select','SelectX','Array','Mix']:del ugens[u]
    return params,dict(ugens)

def scan():
    files=[]; entries=[]
    for path in sorted((ROOT/'synthdefs').rglob('*.scd')):
        if 'CompiledSynthdefs' in path.parts:continue
        rel=str(path.relative_to(ROOT/'synthdefs'));text=path.read_text(errors='replace');s=clean(text);top=rel.split('/')[0]
        calls=list(CREATOR.finditer(s));default=top=='Defaults'; excluded=top in ['Oceanode','OceanodeCode','WIP'];skip='@oceanode-no-autocompile' in text
        eligible=(default or bool(calls)) and not excluded and not skip
        status=('Runtime' if default else 'Build tools' if top=='Oceanode' else 'Code sandbox' if top=='OceanodeCode' else 'Experiment' if top=='WIP' else 'Compatibility' if top=='Compatibility' else 'Library')
        if skip:status='Binary-dependent'
        raw=[]
        rawdefs=[]
        for rm in re.finditer(r'\bSynthDef(?:\s*\.\s*new)?\s*\(',s):
            ro=s.index('(',rm.start());re_=endparen(s,ro);block=s[ro:re_]
            expr=block[1:].split(',',1)[0].strip()
            rp,ru=details(block)
            rawdefs.append(dict(expression=expr,line=s.count('\n',0,rm.start())+1,parameters=rp,ugens=ru))
            literal=re.fullmatch(r"""(?:\\(\w+)|"([^"]+)"|'([^']+)')""",expr)
            if literal:raw.append((next(x for x in literal.groups() if x),''))
        rec=dict(path=rel,status=status,eligible=eligible,creator_count=len(calls),raw_names=list(dict.fromkeys(a or b for a,b in raw)),raw_calls=len(rawdefs),raw_definitions=rawdefs,sha256=hashlib.sha256(text.encode()).hexdigest(),normalized_hash=hashlib.sha256(norm(s).encode()).hexdigest(),lines=text.count('\n')+1)
        files.append(rec)
        for c in calls:
            op=s.index('(',c.start());end=endparen(s,op);segment=s[c.start():end]
            category=re.search(r'category\s*:\s*"([^"\n]*)"',segment);desc=re.search(r'description\s*:\s*"((?:\\.|[^"\\])*)"',segment)
            bodypos=s.find('{',c.end(),end);body=s[bodypos:endparen(s,bodypos,'{','}')] if bodypos>=0 else segment
            params,ugens=details(body)
            entries.append(dict(name=c.group(1),path=rel,line=s.count('\n',0,c.start())+1,status=status,eligible=eligible,category=category.group(1) if category else '',description=desc.group(1).replace('\\n',' ') if desc else '',parameters=params,ugens=ugens,body_hash=hashlib.sha256(norm(body).encode()).hexdigest(),body=body))
        if not calls:
            params,ugens=details(s)
            # One catalog record per raw/support source. Dynamic SynthDef generation remains a family.
            comments=re.findall(r'(?m)^\s*//\s*(.+)',text[:2500])
            desc=' '.join(comments[:3])
            entries.append(dict(name=path.stem,path=rel,line=1,status=status,eligible=eligible,category=str(Path(rel).parent),description=desc,parameters=params,ugens=ugens,body_hash='',body=s))
    return files,entries

def main():
    OUT.mkdir(exist_ok=True,parents=True);files,entries=scan()
    groups=lambda key,items:[dict(key=k,items=[dict(name=x.get('name',''),path=x['path']) for x in v]) for k,v in sorted(_group(key,items).items()) if len(v)>1 and k]
    data=dict(files=files,entries=[{k:v for k,v in e.items() if k!='body'} for e in entries],summary=dict(source_files=len(files),creator_declarations=sum(f['creator_count'] for f in files),eligible_sources=sum(f['eligible'] for f in files),eligible_creators=sum(e['eligible'] and bool(e['body_hash']) for e in entries),status_files=dict(collections.Counter(f['status'] for f in files))),duplicate_creator_names=groups('name',[e for e in entries if e['body_hash']]),exact_generator_groups=groups('body_hash',[e for e in entries if e['eligible'] and e['body_hash']]),exact_source_groups=groups('normalized_hash',files))
    (OUT/'synthdef-inventory.json').write_text(json.dumps(data,indent=2))
    with (OUT/'synthdef-inventory.csv').open('w') as fh:
        fields=['name','path','line','status','eligible','category','description','parameters','ugens'];w=csv.DictWriter(fh,fieldnames=fields);w.writeheader()
        for e in data['entries']:w.writerow({k:('; '.join(e[k]) if isinstance(e[k],(list,dict)) else e[k]) for k in fields})
    print(json.dumps({k:v for k,v in data.items() if k not in ['files','entries']},indent=2))

def _group(key,items):
    d=collections.defaultdict(list)
    for x in items:d[x[key]].append(x)
    return d
if __name__=='__main__':main()
