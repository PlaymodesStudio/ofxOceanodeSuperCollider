#!/usr/bin/env python3
"""Check production resonator vibrato and zero-depth legacy audio offline."""
import pathlib
import subprocess
import sys
import tempfile
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
SOURCE='synthdefs/Defaults/PolyComb/polyCombExtras.scd'

def comb_block(source):
    start=source.index('(1..32).do',source.index('// PCCombR<N>'))
    return source[start:source.index('// PCFilterR<N>',start)]

with tempfile.TemporaryDirectory(prefix='polycomb-resonator-vibrato-') as directory:
    out=pathlib.Path(directory)
    current=comb_block((ROOT/SOURCE).read_text())
    old=(ROOT/'tools/polycomb/fixtures/PCCombR_legacy.scd').read_text()
    old=old.replace('(1..32).do','[1,4,32].do').replace('"PCCombR"','"LegacyComb"')
    probe=current.replace('(1..32).do','[1,4,32].do').replace('"PCCombR"','"VibratoProbe"')
    probe=probe.replace('var wet = Sanitize.ar(ReaktorComb.ar(sig, fc, fb, level, mix, 1));',
                        'var wet = SinOsc.ar(fc, 0, 0.05);')
    code=(current+old+probe).replace('.writeDefFile(dir)',f'.writeDefFile("{out}")')
    (out/'compile.scd').write_text(f'Archive.archiveDir="{out}";\n'+code+
        f'\nSynthDef(\\CombInput,{{Out.ar(64,SinOsc.ar(173,0,0.05)!32)}}).writeDefFile("{out}");\n0.exit;\n')
    (out/'config.yaml').write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\n    - {ROOT}/ugens/OceanodeAnalog\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(out/'config.yaml'),str(out/'compile.scd')],capture_output=True,text=True,timeout=45)
    assert r.returncode==0 and (out/'PCCombR32.scsyndef').exists(),r.stdout+r.stderr
    for n in range(1,33):
        data=(out/f'PCCombR{n}.scsyndef').read_bytes()
        assert all(key in data for key in (b'combvibrate',b'combvibrand',b'combvibamt'))

    def render(name,n,cases,duration=4):
        score=bundle(0,[message('/d_recv',(out/f'{name}{n}.scsyndef').read_bytes()),
                        message('/d_recv',(out/'CombInput.scsyndef').read_bytes()),message('/g_new',1,0,0)])
        for i,(rate,amount,random) in enumerate(cases):
            settings=[message('/s_new','CombInput',100,0,1),
                      message('/s_new',name+str(n),101,1,1,'in',64,'out',0,'combmode',1.,
                              'combpitch',69.,'combfeed',.7,'satlevel',12.,'combmix',1.,
                              'mcpitch',1000,'mcfb',1000,'mcmix',1000,
                              'combvibrate',float(rate),'combvibrand',float(random),'combvibamt',float(amount)),
                      message('/n_setn',101,'combpitch',n,*([69.]*n))]
            # Current controls are per voice; legacy ones are scalars. A scalar
            # /s_new argument initializes only the first lane of an array.
            if name!='LegacyComb':
                for key,value in [('satlevel',12.),('combvibrate',rate),('combvibrand',random),('combvibamt',amount)]:
                    settings.append(message('/n_setn',101,key,n,*([float(value)]*n)))
            score+=bundle(i*duration+.01,settings)
            score+=bundle((i+1)*duration-.01,[message('/g_freeAll',1)])
        score+=bundle(len(cases)*duration+.01,[message('/c_set',0,0.)])
        path=out/(name+str(n)+'.osc');path.write_bytes(score);wav=path.with_suffix('.wav')
        r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(path),'_',str(wav),'48000','WAV','float',
            '-o',str(n),'-i','0','-a','256','-c','2048','-D','0','-w','2048','-m','65536',
            '-U',str(ROOT/'ugens/OceanodeAnalog')+':/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=40)
        assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
        values,ch,sr=read_wav(wav);audio=np.asarray(values).reshape(-1,ch)
        assert np.isfinite(audio).all() and np.max(np.abs(audio))<2
        return audio,sr

    # Adding zero-depth vibrato must preserve the original resonator's sound.
    for n in (1,4,32):
        old_audio,sr=render('LegacyComb',n,[(5,0,0)],1)
        new_audio,_=render('PCCombR',n,[(5,0,0)],1)
        assert np.max(np.abs(old_audio-new_audio))<1e-6, n
    changed,_=render('PCCombR',1,[(5,.5,0)],1)
    baseline,_=render('LegacyComb',1,[(5,0,0)],1)
    assert np.max(np.abs(changed-baseline))>.001
    print('PASS: all 32 variants compile; zero depth preserves legacy audio at 1/4/32 voices; vibrato changes the actual comb output.',flush=True)

    audio,sr=render('VibratoProbe',4,[(5,0,0),(5,.5,0),(8,.5,0),(5,.5,.5)])
    observed=[]
    for case,(expected,amount,random) in enumerate([(0,0,0),(5,.5,0),(8,.5,0),(5,.5,.5)]):
        rates=[]
        for voice in range(4):
            x=audio[int((case*4+.5)*sr):int((case*4+3.5)*sr),voice]
            size=len(x);weights=np.zeros(size);weights[0]=1;weights[1:(size+1)//2]=2
            if size%2==0:weights[size//2]=1
            phase=np.unwrap(np.angle(np.fft.ifft(np.fft.fft(x)*weights)))
            frequency=np.diff(phase)*sr/(2*np.pi);frequency=frequency[int(.1*sr):-int(.1*sr)]
            block=240;frequency=frequency[:len(frequency)//block*block].reshape(-1,block).mean(axis=1)
            semitones=12*np.log2(frequency/440.)
            if amount==0:assert np.std(semitones)<.001;continue
            assert .42<np.std(semitones)*2**.5<.58
            bins=np.fft.rfftfreq(len(semitones),block/sr)
            power=np.abs(np.fft.rfft(semitones-semitones.mean()))
            rate=bins[1+np.argmax(power[1:])];rates.append(rate)
            assert (2.1<rate<7.9) if random else abs(rate-expected)<.4
        if random:assert max(rates)-min(rates)>.1
        observed.append(rates)
    print('PASS: independent resonator vibrato has the requested depth, rate, randomized per-voice rates and a zero-depth bypass.')
