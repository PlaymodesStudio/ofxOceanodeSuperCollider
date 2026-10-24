#!/usr/bin/env python3
"""Offline count-mixer continuity, live graph promotion and shared tail checks."""
from pathlib import Path
import json,math,subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=Path('/tmp/polycomb-voice-morph-render');out.mkdir(exist_ok=True)
config=out/'config.yaml';config.write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\n  - '+str(ROOT/'ugens/OceanodeAnalog')+'\nexcludeDefaultPaths: true\n')
source=(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd').read_text()
a=source.index('    // Allocated only while morphing');b=source.index('    SynthDef(("PCSpreadWide"',a)
code='Archive.archiveDir='+json.dumps(str(out))+';\n'
# Compile the actual changed source too, into its normal ignored cache.
code+='this.executeFile('+json.dumps(str(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd'))+');\n'
code+='[1,2,16].do { |c|\n'+source[a:b].replace('.writeDefFile(dir)', '.writeDefFile('+json.dumps(str(out))+')')
code+='''
    SynthDef(("CountInput"++c).asSymbol, {
        var sig=SinOsc.ar((0..(c-1))*17+\\freq.kr(440),0,0.05);
        Out.ar(\\out.kr(64),sig);Out.ar(\\ref.kr(512),sig);
    }).writeDefFile('''+json.dumps(str(out))+''');
    SynthDef(("CountShared"++c).asSymbol, {
        var sig=In.ar(256,c);
        Out.ar(0,sig);
        Out.ar(c*2,CombC.ar(sig,0.2,0.1,1.0));
    }).writeDefFile('''+json.dumps(str(out))+''');
};
"COUNT MORPH COMPILED".postln;
0.exit;
'''
wrapper=out/'compile.scd';wrapper.write_text(code)
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(wrapper)],capture_output=True,text=True,timeout=60)
(out/'compile.log').write_text(r.stdout+r.stderr)
assert r.returncode==0 and 'COUNT MORPH COMPILED' in r.stdout,r.stdout+r.stderr
report=[]
for c in (1,2,16):
    definitions=[out/(name+str(c)+'.scsyndef')for name in ('PCVoiceMorph','CountInput','CountShared')]
    score=bundle(0,[*[message('/d_recv',p.read_bytes())for p in definitions],message('/g_new',1,0,0)])
    # Same hierarchy used by the running node: target voices initially live
    # inside the original spread slot, followed by the temporary mixer and FX.
    initial=[message('/g_new',10,1,1),message('/g_new',11,1,1),message('/g_new',20,1,11),message('/g_new',21,1,20),message('/g_new',30,1,1),
             message('/s_new','CountInput'+str(c),100,1,10,'out',64,'freq',440.0),
             message('/s_new','CountInput'+str(c),101,1,21,'out',96,'ref',c,'freq',660.0),
             message('/s_new','PCVoiceMorph'+str(c),300,1,11,'out',256),
             message('/s_new','CountShared'+str(c),301,1,30)]
    inputs=[1000.0]*33;inputs[0]=64.0;inputs[12]=96.0
    initial.append(message('/n_setn',300,'inputs',33,*inputs))
    score+=bundle(.02,initial)
    for frame in range(101):
        t=frame/100
        weights=[0.0]*33;weights[0]=1-t;weights[12]=t
        score+=bundle(.3+frame*.01,[message('/n_setn',300,'weights',33,*weights)])
    # Adopt destination groups without restarting the oscillator/envelope nodes.
    # Both gain slots now read the same bus while coefficients fade home.
    inputs[0]=96.0;inputs[12]=96.0
    weights=[0.0]*33;weights[0]=1.0
    score+=bundle(1.7,[message('/n_before',21,30),message('/n_before',300,30),message('/n_free',20),message('/n_free',10),message('/n_free',11),
                       message('/n_setn',300,'inputs',33,*inputs),message('/n_setn',300,'weights',33,*weights)])
    # After promotion the normal renderer replaces the mixer; shared FX stay.
    score+=bundle(2.0,[message('/n_set',101,'out',256),message('/n_free',300)])
    score+=bundle(2.4,[message('/n_free',21)])
    score+=bundle(3.2,[message('/g_freeAll',1)])
    score+=bundle(3.21,[message('/c_set',0,0.0)])
    osc=out/f'{c}.osc';osc.write_bytes(score);wav=out/f'{c}.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o',str(c*3),'-i','0','-a','2048','-z','64','-w','8192','-D','0','-m','262144'],capture_output=True,text=True,timeout=60)
    log=r.stdout+r.stderr;(out/f'{c}.log').write_text(log)
    assert r.returncode==0 and 'FAILURE' not in log and 'not found' not in log,log
    audio,ch,sr=read_wav(wav);assert all(math.isfinite(x)for x in audio)
    error=step=tail=0.0
    for frame in range(round(1.6*sr),round(2.35*sr)):
        for i in range(c):
            error=max(error,abs(audio[frame*ch+i]-audio[frame*ch+c+i]))
    for frame in range(round(.5*sr),round(2.3*sr)):
        for i in range(c):step=max(step,abs(audio[frame*ch+i]-audio[(frame-1)*ch+i]))
    for frame in range(round(2.5*sr),round(2.8*sr)):
        for i in range(c):tail+=audio[frame*ch+2*c+i]**2
    tail=math.sqrt(tail/(round(.3*sr)*c))
    assert error<2e-6,(c,'promotion or mixer removal changed the live destination',error)
    assert step<.009,(c,'count morph contains a discontinuity',step)
    assert tail>1e-5,(c,'shared tail was lost',tail)
    report.append(dict(channels=c,promotion_error=error,max_sample_step=step,shared_tail_rms=tail))
(out/'measurements.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: real DSP at 1/2/16 channels; smooth count crossfade, sample-identical promotion/removal and continuous shared tails')
