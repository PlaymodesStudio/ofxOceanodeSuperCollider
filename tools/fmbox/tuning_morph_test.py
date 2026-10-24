#!/usr/bin/env python3
"""Audio regression for equal fixed-frequency endpoints with different coarse cells."""
import argparse, importlib.util, json, math, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('fmrender',ROOT/'tools/fmbox/render_test.py')
fm=importlib.util.module_from_spec(spec);spec.loader.exec_module(fm)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=Path('/tmp/fm-tuning-morph'))
    out=parser.parse_args().output.resolve();out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fm-tuning-') as work:
        fm.compile_voice(Path(work))
    # The actual native LFO supplies unity pitch and zero amplitude modulation;
    # an audio-rate constant supplies a held note. Each voice has identical state.
    defs=ROOT/'synthdefs/Defaults/FMBox/CompiledSynthdefs/FMBox'
    mod=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
    events=[(0,fm.message('/d_load',str(defs/name))) for name in ['FMBoxVoice.scsyndef','FMBoxDX7LFO.scsyndef']]
    events += [(0,fm.message('/d_load',str(mod/'ModGate1.scsyndef'))), (0,fm.message('/g_new',1,0,0)),
        (.01,fm.message('/s_new','ModGate1',100,1,1,'out',64,'vel',0,'rnd',16,'gate',1.,'velocity',1.)),
        (.01,fm.message('/s_new','FMBoxDX7LFO',101,1,1,'gatebus',64,'out',200,'lfo_pmd',0.,'lfo_amd',0.))]
    values={'env_model':1.,'osc_sync':1.,'feedback':0.,'master':.5,'gatebus':64,'velbus':0,'nativebus':200,'vis':2048}
    for name in ['mpitch','mscale','mfeedback','mamp']+[f'm{kind}{i}' for kind in ['ratio','detune','level'] for i in range(6)]:values[name]=32
    for op in range(6):
        values.update({f'op{op}_mode':1.,f'op{op}_coarse':2.,f'op{op}_fine':0.,f'op{op}_output':99.,f'op{op}_velocity':0.,f'op{op}_left_depth':0.,f'op{op}_right_depth':0.,f'mix{op}':float(op==0)})
        for k in range(4):values[f'op{op}_l{k}']=99.;values[f'pitch_l{k}']=50.
        for src in range(6):values[f'fm{op}_{src}']=0.
    flat=sum(([k,float(v)] for k,v in values.items()),[])
    points=[0.,.001,.499,.5,.501,1.]
    for i,t in enumerate(points):
        raw=2+4*t;packed=[0.]*93;packed[0]=31.;packed[32]=1.;packed[81]=raw;packed[82]=100.
        events += [(.02,fm.message('/s_new','FMBoxVoice',200+i,1,1,*flat,'op0_coarse',raw,'out',i)),
            (.02,fm.message('/n_setn',200+i,'algorithm',93,*packed))]
    events += [(1.,fm.message('/g_freeAll',1)),(1.01,fm.message('/c_set',32,0.))]
    score=out/'test.osc';score.write_bytes(b''.join(fm.bundle(t,[m]) for t,m in sorted(events,key=lambda e:e[0])))
    command=['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(out/'test.wav'),'48000','WAV','float','-o',str(len(points)),'-i','0','-m','262144','-D','0']
    result=subprocess.run(command,capture_output=True,text=True,timeout=30);log=result.stdout+result.stderr;(out/'render.log').write_text(log)
    assert result.returncode==0 and not any(e in log for e in ['FAILURE','ERROR','not installed']),log
    audio,ch,sr=fm.read_wav(out/'test.wav');assert ch==len(points) and all(math.isfinite(x) for x in audio)
    window=range(int(.4*sr),int(.9*sr))
    peak=max(abs(audio[i*ch]) for i in window);assert 1e-5<peak<1,peak
    error=max(abs(audio[i*ch+k]-audio[i*ch]) for i in window for k in range(ch))
    assert error<1e-6,error
    (out/'report.json').write_text(json.dumps({'points':points,'peak':peak,'maxDifference':error},indent=2)+'\n')
    print('PASS: production FM fixed tuning remains identical from coarse 2 to 6 at 0, .001, .499, .5, .501, 1')

if __name__=='__main__':main()
