#!/usr/bin/env python3
"""Prove the two SpaceSystem library renames preserve the compiled DSP payload."""
import json,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
work=pathlib.Path(sys.argv[1]).resolve();work.mkdir(parents=True,exist_ok=True)
conf=work/'sclang_conf.yaml';base=ROOT/'tools/spacesystem_geiger'
paths=[ROOT/p for p in ['sclang/SCClassLibrary','sclang/Extensions','synthdefs/Oceanode','ugens/OceanodeAnalog']]
conf.write_text('includePaths:\n'+''.join(f'    - "{p}"\n' for p in paths)+'excludeDefaultPaths: true\n')
pairs=[('ReaktorFilter4','LadderMultimode','Effect/Filter/reaktorFilter4.scd','Effect/Filter/ladderMultimode.scd'),('ReaktorSaturator','ParabolicSaturator','Effect/Distortion/reaktorSaturator.scd','Effect/Distortion/parabolicSaturator.scd')]
def payload(path):
 b=path.read_bytes();return b[:10]+b[11+b[10]:]
results=[]
for old,new,oldsrc,newsrc in pairs:
 legacy=work/(old+'.scd');legacy.write_bytes(subprocess.check_output(['git','show','HEAD:synthdefs/'+oldsrc],cwd=ROOT))
 for n in [1,4]:
  for name,source in [(old,legacy),(new,ROOT/'synthdefs'/newsrc)]:
   expected=work/'defs'/name/(name+str(n)+'.scsyndef');expected.unlink(missing_ok=True)
   r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(conf),str(ROOT/'synthdefs/Oceanode/compileExact.scd'),str(work/'defs'),str(source),str(n)],capture_output=True,text=True,timeout=120)
   assert expected.exists() and r.returncode==0,r.stdout+r.stderr
  assert payload(work/'defs'/old/(old+str(n)+'.scsyndef'))==payload(work/'defs'/new/(new+str(n)+'.scsyndef')),(old,new,n)
  results.append(dict(old=old,new=new,channels=n,dsp_payload_identical=True))
(work/'rename-validation.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
