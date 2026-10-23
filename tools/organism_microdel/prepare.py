#!/usr/bin/env python3
"""Generate the opening preset, compile exact variants and node discovery metadata."""
import json, pathlib,re,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2];HERE=pathlib.Path(__file__).parent
work=pathlib.Path(sys.argv[1]).resolve();work.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(HERE/'make_preset.py'),str(work/'data')],check=True)
conf=work/'sclang_conf.yaml'
paths=[ROOT/p for p in ['sclang/SCClassLibrary','sclang/Extensions','synthdefs/Oceanode','ugens/OceanodeAnalog','ugens/OceanodePrimitives']]
conf.write_text('includePaths:\n'+''.join(f'    - "{p}"\n' for p in paths)+'excludeDefaultPaths: true\n')
sources={}
for p in (ROOT/'synthdefs').rglob('*.scd'):
 if 'Compatibility' in p.parts or 'WIP' in p.parts:continue
 for name in re.findall(r'~synthCreator.value\("([^"]+)"',p.read_text(errors='replace')):sources[name]=p
builds=set()
for p in (work/'data/Presets').rglob('SC_*.json'):
 if p.stem.startswith('SC_Output'):continue
 name=re.sub(r'^SC_|\*_\d+$','',p.stem);v=json.loads(p.read_text());variables=tuple(int(v[x]) for x in ['NumSpeakers','NumOut','nInputs'] if x in v)
 builds.add((name,int(v['N_Chan']),variables))
def run(args,marker):
 r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(conf),*map(str,args)],capture_output=True,text=True,timeout=180,cwd=work)
 if r.returncode or marker not in r.stdout or 'OCEANODE_SYNC_FAILED' in r.stdout:raise RuntimeError(r.stdout+r.stderr)
for name,n,vs in sorted(builds):
 expected=work/'defs'/name/(name+str(n)+''.join('_'+str(v) for v in vs)+'.scsyndef')
 expected.unlink(missing_ok=True)
 run([ROOT/'synthdefs/Oceanode/compileExact.scd',work/'defs',sources[name],n,*vs],'COMPILE_EXACT_OK')
 expected=work/'defs'/name/(name+str(n)+''.join('_'+str(v) for v in vs)+'.scsyndef')
 assert expected.exists(),expected
 print('Compiled',name,n,vs,flush=True)
new=['CongruentialRandom','SaturatingCompressor','CompensatedSVF','FilteredFeedbackDelay','EventADSR','RandomBeatClock']
run([ROOT/'synthdefs/Oceanode/syncCompile.scd','stereo',work/'metadata',*[str(sources[name])+'::1,2,18' for name in new]],'OCEANODE_SYNC_OK')
for i,name in enumerate(new):
 for filename in [name+'.txarcmeta',name+'.scsyndef',name+'1.scsyndef',name+'2.scsyndef',name+'18.scsyndef']:
  assert (work/'metadata'/str(i)/name/filename).exists(),(name,filename)
tap=work/'tap.scd';tap.write_text('SynthDef(\\tap, { Out.ar(0, In.ar(\\from.kr(0), 2)) }).writeDefFile("'+str(work)+'"); 0.exit;\n')
run([tap],'')
(work/'builds.json').write_text(json.dumps([dict(name=name,n=n,variables=vs,source=str(sources[name].relative_to(ROOT))) for name,n,vs in sorted(builds)],indent=2)+'\n')
print('Prepared',len(builds),'variants and six new atomic library nodes')
