#!/usr/bin/env python3
"""Render the production resonator tuning expression with fractional map/follow."""
from pathlib import Path
import json,math,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
source=(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd').read_text()
start=source.index('(1..32).do',source.index('// PCCombR<N>'))
comb=source[start:source.index('// PCFilterR<N>',start)]
probe=comb.replace('(1..32).do','[1,4,12,32].do').replace('"PCCombR"','"PitchMapProbe"')
probe=probe.replace('var wet = Sanitize.ar(ReaktorComb.ar(sig, fc, fb, level, mix, 1));','var wet = cnote;')
assert probe!=comb and 'var wet = cnote;' in probe
with tempfile.TemporaryDirectory(prefix='polycomb-pitch-map-render-') as directory:
    out=Path(directory)
    code='Archive.archiveDir='+json.dumps(str(out))+';\n'
    # Compile the actual changed definitions as well as the instrumented pitch
    # expression. The latter exposes cnote without changing its calculation.
    code+='this.executeFile('+json.dumps(str(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd'))+');\n'
    code+=probe.replace('.writeDefFile(dir)', '.writeDefFile('+json.dumps(str(out))+')')
    code+='''
    [1,4,12,32].do { |n|
        SynthDef(("PitchMapSource"++n).asSymbol,{
            var v=NamedControl.kr(\\values,60!n).asArray;
            Out.kr(128,v++v++v);
        }).writeDefFile('''+json.dumps(str(out))+''');
    };
    "PITCH MAP COMPILED".postln;
    0.exit;
    '''
    (out/'compile.scd').write_text(code)
    (out/'config.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\n  - '+str(ROOT/'ugens/OceanodeAnalog')+'\nexcludeDefaultPaths: true\n')
    try:
        r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(out/'config.yaml'),str(out/'compile.scd')],capture_output=True,text=True,timeout=60)
    except subprocess.TimeoutExpired as e:
        raise AssertionError((e.stdout or b'').decode(errors='replace')+(e.stderr or b'').decode(errors='replace')) from e
    assert r.returncode==0 and 'PITCH MAP COMPILED' in r.stdout,r.stdout+r.stderr
    for n in (1,4,12,32):
        score=bundle(0,[message('/d_recv',(out/(name+str(n)+'.scsyndef')).read_bytes())for name in ('PitchMapSource','PitchMapProbe')]+[message('/g_new',1,0,0)])
        score+=bundle(.02,[message('/s_new','PitchMapSource'+str(n),100,1,1),message('/s_new','PitchMapProbe'+str(n),101,1,1,'out',0,'note',128,'combglide',0.)])
        cases=[]
        for j,t in enumerate((0,.00101,.1,.25,.499,.501,.75,1)):
            when=.05+j*.2
            a=[min(135.,63+3*i)for i in range(n)]
            b=[36+12*math.log2(i+1)for i in range(n)]
            target=[48+12*math.log2(i+1)for i in range(n)]
            osc=[(1-t)*x+t*y for x,y in zip(a,b)]
            res=[(1-t)*(x-7)+t*y for x,y in zip(a,target)]
            residual=[r-(1-t)*o for r,o in zip(res,osc)]
            score+=bundle(when,[message('/n_setn',100,'values',n,*[v+3 for v in osc]),message('/n_set',101,'combmode',float(t)),message('/n_setn',101,'combpitch',n,*residual)])
            cases.append((when,[r+(1-t)*3 for r in res]))
        score+=bundle(1.8,[message('/g_freeAll',1)])+bundle(1.81,[message('/c_set',0,0.)])
        path=out/(str(n)+'.osc');path.write_bytes(score);wav=path.with_suffix('.wav')
        r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(path),'_',str(wav),'48000','WAV','float','-o',str(n),'-i','0','-a','256','-c','1024','-w','4096','-D','0'],capture_output=True,text=True,timeout=40)
        assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
        samples,ch,sr=read_wav(wav);assert all(math.isfinite(x)for x in samples)
        for when,expected in cases:
            for frame in range(round((when+.15)*sr),round((when+.17)*sr)):
                for i in range(n):assert abs(samples[frame*ch+i]-expected[i])<.0001,(n,when,i,samples[frame*ch+i],expected[i])
        max_step=max(abs(samples[frame*ch+i]-samples[(frame-1)*ch+i])for frame in range(round(.2*sr),round(1.7*sr))for i in range(n))
        assert max_step<.5,(n,max_step)
    print('PASS: actual resonator pitch expression at 1/4/12/32 voices; fractional map/follow tuning, inherited transpose and audio smoothing across 0.5')
