#!/usr/bin/env python3
"""Offline regressions for rational-curve singularities, RBPF bandwidth and cleanup.
No live server, audio device or installed SynthDefs are touched.
"""
import argparse, importlib.util, json, math, pathlib, shutil, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
HERE=pathlib.Path(__file__).parent
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
sys.path.insert(0,str(ROOT/'tools/synthdef_audit'))
from scan import clean,CREATOR,endparen

def bodies(path):
    src=clean(path.read_text());result={}
    for match in CREATOR.finditer(src):
        start=src.index('{',match.end())
        result[match.group(1)]=src[start:endparen(src,start,'{','}')]
    return result

def payload(path):
    b=path.read_bytes();assert b[:4]==b'SCgf'
    return b[:10]+b[11+b[10]:] # Strip only the embedded SynthDef name.

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/synthdef-fixes-test'))
    parser.add_argument('--server',default='/Applications/SuperCollider.app/Contents/Resources/scsynth')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    classes=out/'classes';classes.mkdir(exist_ok=True)
    shutil.copyfile(ROOT/'synthdefs/Oceanode/oceanodeParameter.sc',classes/'OceanodeParameter.sc')
    shutil.copyfile(ROOT/'synthdefs/Oceanode/ugenExtensions.sc',classes/'ugenExtensions.sc')
    shutil.copyfile(ROOT/'sclang/Extensions/user/BufFFT/Classes/BufFFT.sc',classes/'BufFFT.sc')
    config=out/'sclang_conf.yaml'
    config.write_text('includePaths:\n'+''.join('    - '+json.dumps(str(p))+'\n' for p in [ROOT/'sclang/SCClassLibrary',classes])+'excludeDefaultPaths: true\n')
    paths={'SigmoidShaper':('SigmoidShaper.scd','Effect/Distortion/sigmoidshaper.scd'),
           'RBPF':('ResonantFilters.scd','Effect/Filter/resonantfilters.scd'),
           'CircularBufferFFT':('CircularBufferFFT.scd','Effect/Time/Looping/CircularBufferFFT.scd'),
           'MonoBandPassBank64':('BandpassBanks.scd','Effect/Filter/monobandpassbank256.scd')}
    script=['Archive.archiveDir = '+json.dumps(str(out))+';'];expected=[]
    for name,(fixture,current) in paths.items():
        old=bodies(HERE/'fixtures'/fixture);new=bodies(ROOT/'synthdefs'/current)
        for n in [1,2]:
            for prefix,body in [('Old',old[name]),('New',new[name])]:
                defname=prefix+name+str(n);expected.append(out/(defname+'.scsyndef'))
                script.append('SynthDef('+json.dumps(defname)+', { SynthDef.wrap('+body+', prependArgs: ['+str(n)+', []]) }).writeDefFile('+json.dumps(str(out))+');')
    oldbank=bodies(HERE/'fixtures/BandpassBanks.scd')
    for n in [1,2]:
        name='OldMonoBandPassBank32'+str(n);expected.append(out/(name+'.scsyndef'))
        script.append('SynthDef('+json.dumps(name)+', { SynthDef.wrap('+oldbank['MonoBandPassBank32']+', prependArgs: ['+str(n)+', []]) }).writeDefFile('+json.dumps(str(out))+');')
    script.append('SynthDef(\\FixProbeInput, { Out.ar(32, (SinOsc.ar([261.625565, 731]) * [0.4, 0.03]).sum * \\inputGain.kr(1)); Out.ar(33, LFTri.ar(500)); Out.ar(34, LFTri.ar(317)); }).writeDefFile('+json.dumps(str(out))+');')
    script.append('"FIX_PROBES_OK".postln; 0.exit;')
    probe=out/'compile.scd';probe.write_text('try {\n'+'\n'.join(script)+'\n} { |error| error.reportError; 1.exit; };\n')
    result=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(probe)],capture_output=True,text=True,timeout=90,cwd=out)
    (out/'compile.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0 and 'FIX_PROBES_OK' in result.stdout,result.stdout+result.stderr
    assert all(p.exists() for p in expected)
    for n in [1,2]:
        assert payload(out/f'OldCircularBufferFFT{n}.scsyndef')==payload(out/f'NewCircularBufferFFT{n}.scsyndef')
        assert payload(out/f'OldMonoBandPassBank32{n}.scsyndef')==payload(out/f'NewMonoBandPassBank64{n}.scsyndef')
    cases=[]
    for steep in [-1.,-.5,0.,.5,1.]:
        for inflec in [-1.,0.,.25,.5,.75,1.]:
            cases.append(('sigmoid',dict(steep=steep,inflec=inflec),False))
    cases.append(('sigmoid',dict(steep=.5,inflec=0.,inputGain=4.),True))
    cases.append(('rbpf',{},False))
    cases.append(('rbpf',dict(q=.4),False))
    cases.append(('rbpf',dict(q=.999),False))
    report=[]
    defs=[p for p in expected if p.name.endswith('1.scsyndef') and ('SigmoidShaper' in p.name or 'RBPF' in p.name)]+[out/'FixProbeInput.scsyndef']
    for block in [1,64]:
        score=bundle(0,[message('/d_recv',p.read_bytes()) for p in defs]+[message('/g_new',1,0,0)])
        for i,(kind,params,modulate) in enumerate(cases):
            start=i*.3+.01
            events=[message('/s_new','FixProbeInput',1000,1,1,'inputGain',params.get('inputGain',1.))]
            model='SigmoidShaper' if kind=='sigmoid' else 'RBPF'
            controls=[v for k,x in params.items() if k!='inputGain' for v in [k,x]]
            for j,prefix in enumerate(['Old','New']):
                events.append(message('/s_new',prefix+model+'1',1001+j,1,1,'in',32,'out',j,*controls))
                if modulate:
                    events += [message('/n_set',1001+j,'inflec_sel',1,'steep_sel',1),message('/n_mapa',1001+j,'inflec_ar',33,'steep_ar',34)]
            score+=bundle(start,events)+bundle(start+.28,[message('/g_freeAll',1)])
        score+=bundle(len(cases)*.3+.1,[message('/c_set',0,0.)])
        osc=out/f'fixes-{block}.osc';wav=out/f'fixes-{block}.wav';osc.write_bytes(score)
        result=subprocess.run([args.server,'-N',str(osc),'_',str(wav),'48000','WAV','float','-o','2','-i','0','-a','128','-D','0','-z',str(block)],capture_output=True,text=True,timeout=90)
        (out/f'render-{block}.log').write_text(result.stdout+result.stderr)
        assert result.returncode==0 and 'FAILURE' not in result.stdout+result.stderr,result.stdout+result.stderr
        audio,ch,rate=read_wav(wav);assert ch==2
        assert all(math.isfinite(v) for v in audio[1::2]),'nonfinite fixed output during startup/modulation'
        rows=[]
        for i,(kind,params,modulate) in enumerate(cases):
            start=int((i*.3+.08)*rate)*2;end=int((i*.3+.26)*rate)*2
            old=audio[start:end:2];new=audio[start+1:end:2]
            assert all(math.isfinite(v) for v in new),(block,kind,params,'nonfinite')
            rms=lambda a: math.sqrt(sum(v*v for v in a)/len(a))
            row=dict(kind=kind,parameters=params,audio_modulation=modulate,new_rms=rms(new),old_nonfinite=sum(not math.isfinite(v) for v in old))
            if kind=='sigmoid':
                assert max(map(abs,new))<4,(block,params,'unbounded output')
                if all(math.isfinite(v) for v in old) and not modulate:
                    error=max(abs(a-b) for a,b in zip(old,new));assert error==0,(params,error)
                    row['healthy_max_difference']=error
            else:
                if not params:
                    assert rms(new)>1e-4,(block,'default RBPF still silent',rms(new))
                    row['old_rms']=rms(old) if all(math.isfinite(v) for v in old) else None
                elif params['q']==.4:
                    error=max(abs(a-b) for a,b in zip(old,new));assert error==0,(block,'ordinary RBPF changed',error)
                    row['healthy_max_difference']=error
            rows.append(row)
        report.append(dict(block_size=block,cases=rows))
    summary=dict(passed=True,block_sizes=[1,64],cases_per_block=len(cases),circular_buffer_dsp_unchanged=True,duplicate32_is_exactly64=True,results=report)
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k!='results'},indent=2))
if __name__=='__main__':main()
