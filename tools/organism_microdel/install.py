#!/usr/bin/env python3
"""Install Organism and migrate the installed SpaceSystem names without regenerating it.

Every changed file/folder is backed up. Run with:
  install.py ORGANISM_BUILD GEIGER_BUILD OCEANAUDIO_DATA
"""
import datetime,importlib.util,json,pathlib,re,shutil,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
work,geiger,data=map(lambda x:pathlib.Path(x).resolve(),sys.argv[1:4])
library_only='--library-only' in sys.argv[4:]
backup=data/'Supercollider/_backups'/('generic-dsp-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
log=[]
def copy(src,dst):
 if dst.exists() and src.read_bytes()==dst.read_bytes():return
 if dst.exists():
  saved=backup/dst.relative_to(data);saved.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dst,saved)
 dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst);log.append(str(dst.relative_to(data)))
sources={}
for p in (ROOT/'synthdefs').rglob('*.scd'):
 if 'Compatibility' in p.parts or 'WIP' in p.parts:continue
 for name in re.findall(r'~synthCreator.value\("([^"]+)"',p.read_text(errors='replace')):sources[name]=p
# Install exact preset requirements, discovery metadata for new nodes, and generic SpaceSystem aliases.
for base in [work/'defs',work/'metadata',geiger/'metadata']:
 for p in base.rglob('*'):
  if not p.is_file() or p.suffix not in ('.scsyndef','.txarcmeta'):continue
  name=p.parent.name
  if base==geiger/'metadata' and name not in ('LadderMultimode','ParabolicSaturator'):continue
  source=sources[name];category=source.relative_to(ROOT/'synthdefs').parent
  copy(p,data/'Supercollider/Synthdefs'/category/name/p.name)
  copy(source,data/'Supercollider/SynthdefSources'/source.relative_to(ROOT/'synthdefs'))
for name in ('LadderMultimode','ParabolicSaturator'):
 for p in (geiger/'defs'/name).glob('*.scsyndef'):
  source=sources[name];copy(p,data/'Supercollider/Synthdefs'/source.relative_to(ROOT/'synthdefs').parent/name/p.name)
# Migration rewrites existing values in place: do not regenerate/replace SpaceSystem.
spec=importlib.util.spec_from_file_location('migrate_names',ROOT/'tools/migrate_synth_renames.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
for tree in ('Presets','Macros'):
 for folder in sorted((data/tree).rglob('*')):
  if not folder.is_dir() or not mod.affected(str(folder)):continue
  for p in list(folder.glob('*.json')):
   newtext=mod.rewrite(p.read_text());newname=mod.rewrite(p.name)
   if newtext==p.read_text() and newname==p.name:continue
   saved=backup/p.relative_to(data);saved.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,saved)
   dest=p.with_name(newname)
   if dest!=p and dest.exists():raise RuntimeError('Rename conflict: '+str(dest))
   dest.write_text(newtext)
   if dest!=p:p.unlink()
   log.append(str(dest.relative_to(data)))
# Old discovery entries and sources live in the backup, not in the active library.
for old,category,filename in [('ClockedNoise','Source/Noise','clockedNoise.scd'),('ReaktorFilter4','Effect/Filter','reaktorFilter4.scd'),('ReaktorSaturator','Effect/Distortion','reaktorSaturator.scd')]:
 for p in [data/'Supercollider/Synthdefs'/category/old,data/'Supercollider/SynthdefSources'/category/filename]:
  if not p.exists():continue
  saved=backup/p.relative_to(data);saved.parent.mkdir(parents=True,exist_ok=True);shutil.move(str(p),str(saved));log.append('retired '+str(p.relative_to(data)))
owned=[pathlib.Path('Presets/DEBUG/23--ORGANISM_MICRODEL')]
owned += [p.relative_to(work/'data') for p in (work/'data/Macros/AUDIO/REAKTOR').iterdir()]
for relative in ([] if library_only else owned):
 src,dst=work/'data'/relative,data/relative
 if dst.exists():
  saved=backup/relative;saved.parent.mkdir(parents=True,exist_ok=True);shutil.move(str(dst),str(saved))
 dst.parent.mkdir(parents=True,exist_ok=True);shutil.copytree(src,dst);log.append(str(relative))
for name in ('OceanodePrimitives.sc','OceanodePrimitives.scx'):
 copy(ROOT/'ugens/OceanodePrimitives'/name,data/'Supercollider/Extensions/OceanodePrimitives'/name)
copy(ROOT/'data/Supercollider/Synthdefs/Defaults/AudioRouting/audioBusMixer.scsyndef',data/'Supercollider/Synthdefs/Defaults/AudioRouting/audioBusMixer.scsyndef')
backup.mkdir(parents=True,exist_ok=True);(backup/'install-manifest.json').write_text(json.dumps(log,indent=2)+'\n')
print('Installed:',data/'Presets/DEBUG/23--ORGANISM_MICRODEL');print('Migrated existing presets/macros; settings preserved.');print('Backup:',backup)
