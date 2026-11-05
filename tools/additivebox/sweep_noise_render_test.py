#!/usr/bin/env python3
"""Render AdditiveBoxSweep8's Noise path: a landscape that evolves, never
repeats, is smooth across the partials, stays in range, and adds nothing at
nmix 0 (bit-exact 1.0 with a table of ones)."""
from pathlib import Path
import json, math, os, re, statistics, subprocess, sys, tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav

with tempfile.TemporaryDirectory(prefix='additive-sweep-noise-') as directory:
    work=Path(directory); (work/'home').mkdir()
    source=(ROOT/'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text()
    source=source.replace('caps = [8, 16, 32, 64, 128, 256]','caps = [8]').replace('chunks = [8, 16, 32, 64]','chunks = [8]').replace('maxOut = 16','maxOut = 1')
    (work/'additiveBox.scd').write_text(source)
    (work/'run.scd').write_text('Archive.archiveDir='+json.dumps(str(work))+';this.executeFile('+json.dumps(str(work/'additiveBox.scd'))+');'
        'SynthDef(\\srcOnes, { ReplaceOut.kr(\\bus.kr(0), 1 ! 8) }).writeDefFile('+json.dumps(str(work))+');'
        'SynthDef(\\capSweep, { Out.ar(0, K2A.ar(In.kr(\\bus.kr(0), 8))) }).writeDefFile('+json.dumps(str(work))+');'
        '"SWEEP COMPILED".postln;0.exit;')
    (work/'sclang.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
    r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(work/'sclang.yaml'),str(work/'run.scd')],capture_output=True,text=True,timeout=120,
                     stdin=subprocess.DEVNULL,env={**os.environ,'HOME':str(work/'home')})
    assert r.returncode==0 and 'SWEEP COMPILED' in r.stdout,r.stdout+r.stderr
    sweep=(work/'CompiledSynthdefs/AdditiveBox/AdditiveBoxSweep8.scsyndef').read_bytes()
    events=bundle(0,[message('/d_recv',sweep),message('/d_recv',(work/'srcOnes.scsyndef').read_bytes()),
                     message('/d_recv',(work/'capSweep.scsyndef').read_bytes()),message('/g_new',1,0,0),message('/b_alloc',10,257,1)])
    nph=[-0.25*i for i in range(8)]   # 2 hills across the 8 partials
    events+=bundle(.01,[message('/b_fill',10,0,257,1.0),
        message('/s_new','srcOnes',100,0,1,'bus',1000),
        message('/s_new','AdditiveBoxSweep8',101,1,1,'bus',1000,'sbuf',10,'swamt',1.0,'swrate',0.5,'swevolve',2.0,'nmix',1.0,'gate',1.0),
        message('/n_setn',101,'nph',8,*nph),
        message('/s_new','capSweep',102,1,1,'bus',1000)])
    # a fresh sweep at nmix 0 (never touched): must leave the table alone exactly
    events+=bundle(4.0,[message('/n_free',101),
        message('/s_new','AdditiveBoxSweep8',103,3,100,'bus',1000,'sbuf',10,'swamt',1.0,'swrate',0.5,'swevolve',2.0,'nmix',0.0,'gate',1.0),
        message('/n_setn',103,'nph',8,*nph)])
    events+=bundle(5.0,[message('/g_freeAll',1)])+bundle(5.01,[message('/c_set',0,0.0)])
    score=work/'test.osc';score.write_bytes(events);wav=work/'test.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(wav),'48000','WAV','float','-o','8','-i','0','-z','64','-D','0','-m','131072','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=60)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,(r.returncode,r.stdout+r.stderr)
    audio,ch,sr=read_wav(wav)
    assert ch==8 and all(math.isfinite(x) for x in audio)
    sample=lambda t,p:audio[round(t*sr)*ch+p]
    # 1. range (cubic interpolation may overshoot 1 a little, never below 0)
    vals=[sample(t/200,p) for t in range(100,800) for p in range(8)]
    assert min(vals)>=0.0 and max(vals)<=1.05,(min(vals),max(vals))
    print('range %.3f .. %.3f'%(min(vals),max(vals)))
    # 2. it moves: every partial's level changes over 0.5 .. 4 s
    for p in range(8):
        series=[sample(t/100,p) for t in range(50,400)]
        assert statistics.pstdev(series)>0.03,(p,statistics.pstdev(series))
    print('time std per partial', ' '.join('%.2f'%statistics.pstdev([sample(t/100,p) for t in range(50,400)]) for p in range(8)))
    # 3. no two one-second windows alike (it never loops)
    windows=[[sample(a+t/100,0) for t in range(100)] for a in (0.5,1.5,2.5)]
    for i in range(3):
        for j in range(i+1,3):
            assert max(abs(x-y) for x,y in zip(windows[i],windows[j]))>0.05,(i,j)
    # 4. smooth across the partials: neighbours closer than partials 4 apart
    near=statistics.mean(abs(sample(t/100,p)-sample(t/100,p+1)) for t in range(50,400) for p in range(7))
    far=statistics.mean(abs(sample(t/100,p)-sample(t/100,p+4)) for t in range(50,400) for p in range(4))
    assert near<far*0.6,(near,far)
    print('neighbour diff %.3f, 4-apart diff %.3f'%(near,far))
    # 5. no change from block to block beyond a smooth slope (no jumps where the
    #    read position crosses the ring's seam), up to the swap at 4 s
    steps=[(abs(sample(t/750,p)-sample((t+1)/750,p)),t/750,p) for t in range(400,2950) for p in range(8)]
    step,when,part=max(steps)
    assert step<0.05,(step,when,part)
    print('max step per control block %.4f (at %.2f s, partial %d)'%(step,when,part))
    # 6. nmix 0 from birth: exactly the table (ones)
    ones=[sample(t/100,p) for t in range(415,500) for p in range(8)]
    assert all(x==1.0 for x in ones),(min(ones),max(ones))
    print('nmix 0: exactly 1.0 on every partial')
    print('SWEEP NOISE OK')
