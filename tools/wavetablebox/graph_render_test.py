#!/usr/bin/env python3
"""Render the real WavetableBox graph captured by box_gate_mode_test.py.

Compile the Defaults/WavetableBox, Modulation, MixerPro and PolyComb sources
first (or sync SynthDefs), then build the native test with OCEANODE_TEST_BOX=
wavetableBox and OCEANODE_TEST_BUILD_DIR set. Pass its regression binary here.
"""
from pathlib import Path
import argparse,json,math,os,struct,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import bundle,message,read_wav
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('native_binary',type=Path)
parser.add_argument('--output',type=Path,default=Path('/private/tmp/wavetable-routing-audio'))
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
defs=out/'defs';defs.mkdir(exist_ok=True)
folders=['WavetableBox/CompiledSynthdefs/WavetableBox','Modulation/CompiledSynthdefs/Modulation','MixerPro/CompiledSynthdefs/mixerpro','PolyComb/CompiledSynthdefs/PolyComb']
for folder in folders:
    for file in (ROOT/'synthdefs/Defaults'/folder).glob('*.scsyndef'):
        name=file.stem
        if not (name=='WavetableOsc3' or name.startswith(('WavetableBoxSpread','WavetableBoxOut','Mod')) or name in {base+str(c) for base in ('mixerProComp','mixerProEQ','PCBlur','mixerProEcho','mixerProRvb') for c in (2,4)}):continue
        dest=defs/file.name
        if not dest.exists():dest.symlink_to(file)
assert (defs/'WavetableBoxSpread2.scsyndef').exists(),'Compile wavetableBox.scd first'
scratch=tempfile.TemporaryDirectory(prefix='wavetable-capture-')
r=subprocess.run([str(args.native_binary.resolve()),'unused',scratch.name],capture_output=True,text=True,timeout=60,
    env={**os.environ,'BOX_WAVETABLE_SCORE':str(out/'score.osc'),'BOX_WAVETABLE_DEFS':str(defs)})
(out/'native.log').write_text(r.stdout+r.stderr)
assert r.returncode==0,(r.stdout+r.stderr)[-4000:]
raw=(out/'score.osc').read_bytes();packets={}
def collect(packet):
    hi,lo=struct.unpack_from('>II',packet,8);when=hi+lo/2**32;offset=16
    while offset<len(packet):
        size=struct.unpack_from('>i',packet,offset)[0];item=packet[offset+4:offset+4+size]
        if item.startswith(b'#bundle'):collect(item)
        else:packets.setdefault(when,[]).append(item)
        offset+=4+size
offset=0
while offset<len(raw):
    size=struct.unpack_from('>i',raw,offset)[0];collect(raw[offset+4:offset+4+size]);offset+=4+size
(out/'score.osc').write_bytes(b''.join(bundle(t,packets[t]) for t in sorted(packets) if t<14)+bundle(14,[message('/g_freeAll',0)])+bundle(14.1,[message('/c_set',0,0.0)]))
plugins='/Applications/SuperCollider.app/Contents/Resources/plugins:'+str(Path.home()/'Library/Application Support/SuperCollider/Extensions/GrainUtils/macOS')
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'score.osc'),'_',str(out/'audio.wav'),'48000','WAV','float','-o','4','-i','0','-a','1024','-c','65536','-b','1024','-m','262144','-w','4096','-D','0','-U',plugins],capture_output=True,text=True,timeout=60,cwd=scratch.name)
log=r.stdout+r.stderr;(out/'render.log').write_text(log)
assert r.returncode==0 and not any(s in log for s in ('FAILURE','not found','exception','could not be opened')),log[-4000:]
audio,channels,sr=read_wav(out/'audio.wav');assert channels==4 and all(math.isfinite(x) for x in audio)
def rms(a,b,c=0):
    values=audio[round(a*sr)*channels+c:round(b*sr)*channels:channels]
    return math.sqrt(sum(x*x for x in values)/len(values))
levels={name:rms(a,b) for name,a,b in [('held',1,1.9),('released',3.5,3.9),('drone',4.4,4.9),('effects',7.5,7.9),('droneOff',13.5,13.9)]}
print(json.dumps(levels,indent=2))
assert all(levels[k]>1e-5 for k in ('held','drone','effects'))
assert levels['released']<1e-5 and levels['droneOff']<1e-5
assert rms(5.4,5.9,0)>1e-5 and rms(5.4,5.9,1)<1e-6,'Manual pan left'
assert rms(6.4,6.9,1)>1e-5 and rms(6.4,6.9,0)<1e-6,'Manual pan right'
assert any(rms(8.4,8.9,c)>1e-5 for c in (2,3)),'Four-channel output must reach upper channels'
assert max(map(abs,audio))<2
print('PASS: bundled table plays, notes sustain/release, Drone holds/stops, panning and effects route across independent output widths')

scratch.cleanup()
