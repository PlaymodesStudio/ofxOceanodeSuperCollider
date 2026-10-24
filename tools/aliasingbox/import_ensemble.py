#!/usr/bin/env python3
"""Recover Aliasing Synth controls and all snapshots using ReaktorPatchExtractor.

The original ensemble's legacy NRKT records have stable panel IDs at +0xd2
(or +0xd6 when an extra link word is present), biased by 10000. Snapshot
state IDs PRECEDE each state record. The generic extractor's older v3/v4
reports incorrectly attach the following record's ID and infer an offset
against traversal IDs. This importer deliberately ignores that inferred map.

Only the known Aliasing Synth control IDs are translated; the raw snapshot
records (including isolated/null states) are retained for auditing. Run from
any directory. Does not change the source ensemble or the extractor binary.
"""
import argparse,json,struct,pathlib,hashlib,subprocess,math
root=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('ensemble',type=pathlib.Path)
parser.add_argument('--extractor',type=pathlib.Path,default=root/'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor')
args=parser.parse_args()
report=subprocess.run([str(args.extractor.resolve()),str(args.ensemble.resolve())],check=True,capture_output=True,text=True)
j=json.loads(report.stdout);b=args.ensemble.read_bytes();modules=[]
def walk(a):
 for m in a: modules.append(m);walk(m['children'])
walk(j['top_level']);ms={m['object_id']:m for m in modules}
# Each entry is a traced Primary panel control, not a guessed snapshot order.
items=[(14,'Level','Output'),(192,'Write Pitch','Write'),(195,'Write Fine','Write'),(194,'Write Symmetry','Write'),(231,'Write Quality','Write'),(263,'Write Spread','Write'),(200,'Write LFO Amount','Write'),(203,'Write LFO Rate','Write'),(269,'Read Pitch','Read'),(272,'Read Fine','Read'),(271,'Read Bidirectional','Read'),(270,'Read Quality','Read'),(308,'Read Spread','Read'),(275,'Read LFO Amount','Read'),(278,'Read LFO Rate','Read'),(384,'Transpose','Output'),(361,'Stereo','Output'),(17,'LP Pitch','Filter'),(30,'LP Resonance','Filter'),(33,'LP FM','Filter'),(23,'HP Pitch','Filter'),(21,'HP Resonance','Filter'),(26,'HP FM','Filter'),(20,'Drive','Filter'),(27,'Filter On','Filter'),(40,'DC Block','Filter'),(166,'Notch Resonance','Notch'),(170,'Notch Gain','Notch'),(172,'Notch Pitch','Notch'),(176,'Notch Keytrack','Notch'),(185,'Notch On','Notch'),(45,'Filter Attack','Filter envelope'),(46,'Filter Decay','Filter envelope'),(47,'Filter Sustain','Filter envelope'),(48,'Filter Release','Filter envelope'),(133,'Filter LFO Amount','Filter envelope'),(163,'Filter Env Amount','Filter envelope'),(55,'Filter LFO Shape','Filter envelope'),(61,'Filter LFO Wave','Filter envelope'),(59,'Filter LFO Numerator','Filter envelope'),(67,'Filter LFO Denominator','Filter envelope'),(76,'Filter LFO Smoothing','Filter envelope'),(139,'Filter LFO Gate Sync','Filter envelope'),(147,'Filter LFO Reset Phase','Filter envelope'),(463,'Amp Attack','Amp envelope'),(464,'Amp Decay','Amp envelope'),(465,'Amp Sustain','Amp envelope'),(466,'Amp Release','Amp envelope'),(481,'Envelope Keytrack','Amp envelope'),(492,'Velocity Sensitivity','Amp envelope'),(504,'Fine Tune','Output'),(453,'Table Range','Output')]
controls=[];idmap={}
for mid,name,group in items:
 m=ms[mid]; p=int(m['offset'],16)
 # A Primary control may carry an extra link word at +0x3c.
 shift=4 if struct.unpack_from('<I',b,p+0x3c)[0] else 0
 lo,hi=struct.unpack_from('<ff',b,p+0x5c+shift)
 raw=struct.unpack_from('<I',b,p+0x112+shift)[0]
 norm=struct.unpack_from('<f',b,p+0x112+shift)[0] if m['kind'] in [20,25] else float(raw>0)
 stable=struct.unpack_from('<I',b,p+0xd2+shift)[0]-10000
 assert 0<stable<1000,(mid,stable)
 # Quality switches select three/two inputs; their state is an integer index.
 mode='switch' if m['kind'] in [22,23,24] else 'float'
 if mid==61:lo,hi,norm=0,7,max(0,min(raw,7))/7
 if mid==231:lo,hi,norm=0,2,max(0,min(raw-1,2))/2
 if mid==270:lo,hi,norm=0,1,max(0,min(raw-1,1))
 if mid in [27,40,185]:norm=float(raw==1)
 if hi<lo:lo,hi=hi,lo;norm=1-norm
 default=lo+(hi-lo)*max(0,min(1,norm))
 # The original button outputs symmetry W=1 for forward, W=0 for triangle.
 # Expose the intuitive boolean: checked means bidirectional.
 if mid==271:default=1-default
 c=dict(id=mid,snapshot_id=stable,name=name,sc=name.lower().replace(' ','_'),group=group,min=lo,max=hi,default=default,mode=mode)
 controls.append(c);idmap[stable]=c
# In NRKT a state's ID precedes its state record. The old CLI used the next ID.
snaps=[]; recovered=[]
for s in j['snapshots']:
 cursor=int(s['offset'],16)+4+len(s['name'].encode())+108+s.get('metadata_extension_bytes',0)
 vals={c['name']:c['default'] for c in controls};raws=[]
 for rec in s['parameters']:
  sid=struct.unpack_from('<I',b,cursor-4)[0];size=struct.unpack_from('<I',b,cursor)[0]
  c=idmap.get(sid)
  if c and 'value' in rec:
   value=rec['value']; lo=c['min'];hi=c['max']
   if rec['value_type']=='integer':
    if c['id'] in [231,270]:value=value-1
    elif c['id'] in [27,40,185]:value=float(value==1)
    value=max(lo,min(hi,value))
   else:value=lo+(hi-lo)*value
   if c['id']==194 or c['id']==271:value=hi-(value-lo) # inverted source ranges
   if c['id']==271:value=1-value
   vals[c['name']]=value
  raws.append(dict(snapshot_id=sid,control=c['name'] if c else None,type=rec['value_type'],value=rec.get('value')))
  cursor+=size+8
 snaps.append(dict(name=s['name'],values=vals));recovered.append(dict(name=s['name'],records=raws))
assert len(idmap)==len(controls), 'Duplicate stable panel ID'
assert len(snaps)==38, 'Unexpected ensemble bank; inspect its structure before importing'
for preset in snaps:
 for c in controls:
  value=preset['values'][c['name']]
  assert math.isfinite(value) and c['min']-1e-5<=value<=c['max']+1e-5,(preset['name'],c['name'],value)
# Independent anchors from the first snapshot's bytes: these catch a shifted
# state-ID association, a false traversal-ID match, or wrong control ranges.
assert snaps[0]['name']=='quak'
assert abs(snaps[0]['values']['Write Pitch']+32)<1e-4
assert abs(snaps[0]['values']['Read Pitch']+51)<1e-4
assert snaps[0]['values']['Level']==0 and snaps[0]['values']['Notch On']==0
assert snaps[0]['values']['Write Quality']==0 and snaps[0]['values']['Read Quality']==0

dest=root/'synthdefs/Defaults/AliasingBox';dest.mkdir(parents=True,exist_ok=True)
(dest/'controls.json').write_text(json.dumps(controls,indent=2)+'\n')
(dest/'factory.json').write_text(json.dumps(snaps,indent=2)+'\n')
(dest/'reaktor-snapshots.json').write_text(json.dumps(dict(source=pathlib.Path(j['source']).name,sha256=hashlib.sha256(b).hexdigest(),snapshots=recovered),indent=2)+'\n')
(root/'src/nodes/boxSynths/aliasingBox/scAliasingBoxData.h').write_text('// Generated by tools/aliasingbox/import_ensemble.py.\n#pragma once\nnamespace aliasingBoxData {\ninline constexpr const char* controls = R"ALIAS('+json.dumps(controls,separators=(',',':'))+')ALIAS";\ninline constexpr const char* factory = R"ALIAS('+json.dumps(snaps,separators=(',',':'))+')ALIAS";\n}\n')
print('Recovered',len(snaps),'snapshots;',len(controls),'sound controls')
