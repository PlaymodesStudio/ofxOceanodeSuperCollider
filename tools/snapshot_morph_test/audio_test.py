#!/usr/bin/env python3
"""Compile the modified DSP and verify fractional bypasses with offline audio.

No running audio server/device is used. Definitions compile into their normal
ignored cache folders; scores, renders and logs stay under --output.
"""
import argparse
import importlib.util
import json
import math
import shutil
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('osc_helpers',ROOT/'tools/aliasingbox/render_test.py')
osc=importlib.util.module_from_spec(spec)
spec.loader.exec_module(osc)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-compile',action='store_true')
    parser.add_argument('--output',type=Path,default=Path('/tmp/snapshot-morph-audio'))
    args=parser.parse_args()
    out=args.output
    out.mkdir(parents=True,exist_ok=True)
    config=out/'sclang.yaml'
    classes=out/'classes'
    classes.mkdir(exist_ok=True)
    for path in [ROOT/'synthdefs/Oceanode/oceanodeParameter.sc',ROOT/'synthdefs/Oceanode/oceanodeMicroSpace.sc',ROOT/'ugens/OceanodeSteamPipe/OceanodeSteamPipe.sc',ROOT/'ugens/OceanodeAnalog/OceanodeAnalog.sc']:
        shutil.copyfile(path,classes/path.name)
    includes=[ROOT/'sclang/SCClassLibrary',classes]
    config.write_text('includePaths:\n'+''.join('  - '+json.dumps(str(p))+'\n' for p in includes)+'excludeDefaultPaths: true\n')
    paths=['MixerPro/mixerpro.scd','AnalogBox/analogBox.scd','AliasingBox/aliasingBox.scd',
           'SampleBox/samplebox.scd','SlicerBox/slicerBox.scd','SteamPipeBox/steampipeBox.scd']
    script='Archive.archiveDir='+json.dumps(str(out))+';\n\"MORPH DSP START\".postln;\n'
    for path in paths:
        script+='this.executeFile('+json.dumps(str(ROOT/'synthdefs/Defaults'/path))+');\n'
    # The legacy Echo generator's unchanged external API, including 1/2/16 channels.
    script+='''~synthCreator = { |name, func, description, category, variables, variableDimensions|
        [1,2,16].do { |n| SynthDef((name++n).asSymbol, { func.value(n) }).writeDefFile('''+json.dumps(str(out))+''') };
    };
'''
    script+='this.executeFile('+json.dumps(str(ROOT/'synthdefs/Effect/Time/Delay/echo.scd'))+');\n'
    script+='SynthDef(\\MorphTestInput, { Out.ar(64, SinOsc.ar(440, 0, 0.05)) }).writeDefFile('+json.dumps(str(out))+');\n'
    script+='"MORPH DSP COMPILED".postln;\n0.exit;\n'
    wrapper=out/'compile.scd'
    wrapper.write_text(script)
    if not args.skip_compile:
        with (out/'compile.log').open('w') as logfile:
            r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(wrapper)],stdout=logfile,stderr=subprocess.STDOUT,text=True,timeout=180)
        log=(out/'compile.log').read_text()
        # Sandboxed sclang may report network/default-user-cache errors during
        # startup. Validate the actual compilation section independently.
        relevant=log.split('MORPH DSP START',1)[-1]
        assert r.returncode==0 and 'ERROR:' not in relevant and 'MORPH DSP COMPILED' in relevant,relevant[-6000:]
    defs=ROOT/'synthdefs/Defaults'
    # Each processor gets the same sine and independent identical initial DSP
    # state. At settled activation a its output must equal (1-a)*dry+a*wet.
    cases=[('EQ',defs/'MixerPro/CompiledSynthdefs/mixerpro/mixerProEQ1.scsyndef','mixerProEQ1','in',{'b3gain':12.0}),
           ('SampleComp',defs/'SampleBox/CompiledSynthdefs/SampleBox/SampleBoxComp_1.scsyndef','SampleBoxComp_1','bus',{'compthreshold':-40.0,'compratio':8.0}),
           ('SampleSat',defs/'SampleBox/CompiledSynthdefs/SampleBox/SampleBoxSat_1.scsyndef','SampleBoxSat_1','bus',{'satdrive':18.0})]
    results=[]
    for label,path,name,input_name,params in cases:
        messages=[osc.message('/d_recv',(out/'MorphTestInput.scsyndef').read_bytes()),osc.message('/d_recv',path.read_bytes()),osc.message('/g_new',1,0,0)]
        source=[osc.message('/s_new','MorphTestInput',100,1,1)]
        # SampleBox inserts ReplaceOut into their own buses; copy the source
        # to each bus through a standard routing synth for comparable outputs.
        if input_name=='bus':
            copy_script='Archive.archiveDir='+json.dumps(str(out))+'; SynthDef(\\MorphCopy,{Out.ar(\\out.kr(0),In.ar(64,1))}).writeDefFile('+json.dumps(str(out))+');0.exit;'
            copy_file=out/'copy.scd';copy_file.write_text(copy_script)
            rr=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(copy_file)],capture_output=True,text=True,timeout=30)
            assert rr.returncode==0 and (out/'MorphCopy.scsyndef').exists(),rr.stdout
            messages.append(osc.message('/d_recv',(out/'MorphCopy.scsyndef').read_bytes()))
            for i in range(3): source.append(osc.message('/s_new','MorphCopy',110+i,1,1,'out',i))
        for i,activation in enumerate((0.0,0.4,1.0)):
            controls=[input_name,64 if input_name=='in' else i,'on',activation]
            if input_name=='in': controls+=['out',i]
            controls+=sum(([k,v] for k,v in params.items()),[])
            source.append(osc.message('/s_new',name,120+i,1,1,*controls))
        score=osc.bundle(0,messages)+osc.bundle(0.01,source)+osc.bundle(0.7,[osc.message('/g_freeAll',1)])+osc.bundle(0.71,[osc.message('/c_set',0,0.0)])
        score_path=out/(label+'.osc');score_path.write_bytes(score)
        wav=out/(label+'.wav')
        rr=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score_path),'_',str(wav),'48000','WAV','float','-o','3','-i','0','-z','64','-D','0','-m','262144'],capture_output=True,text=True,timeout=30)
        text=rr.stdout+rr.stderr
        (out/(label+'.log')).write_text(text)
        assert rr.returncode==0 and 'FAILURE' not in text,text
        audio,ch,sr=osc.read_wav(wav)
        assert all(math.isfinite(v) for v in audio)
        error=0.0;difference=0.0
        for frame in range(round(0.25*sr),round(0.65*sr)):
            dry,partial,wet=audio[frame*ch:frame*ch+3]
            error=max(error,abs(partial-(0.6*dry+0.4*wet)))
            difference=max(difference,abs(wet-dry))
        assert error<2e-6,(label,'fractional bypass did not interpolate',error)
        assert difference>1e-4,(label,'processor inaudible',difference)
        results.append(dict(processor=label,max_interpolation_error=error,processed_difference=difference))
    # Echo's filter transition must be continuous across the old switch point.
    echo=defs/'MixerPro/CompiledSynthdefs/mixerpro/mixerProEcho1.scsyndef'
    messages=[osc.message('/d_recv',(out/'MorphTestInput.scsyndef').read_bytes()),osc.message('/d_recv',echo.read_bytes()),osc.message('/g_new',1,0,0)]
    nodes=[osc.message('/s_new','MorphTestInput',100,1,1)]
    for i,t in enumerate((0.0,0.49,0.51,1.0)):
        nodes.extend([osc.message('/s_new','mixerProEcho1',120+i,1,1,'in',64,'out',i,'on',1.0,'mix',1.0,'delay',0.02,'feed',0.0,'cutoff',80.0),
                      osc.message('/n_setn',120+i,'filterweights',4,1-t,0.0,t,0.0)])
    path=out/'mode.osc'
    path.write_bytes(osc.bundle(0,messages)+osc.bundle(0.01,nodes)+osc.bundle(0.7,[osc.message('/g_freeAll',1)])+osc.bundle(0.71,[osc.message('/c_set',0,0.0)]))
    wav=out/'mode.wav'
    rr=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(path),'_',str(wav),'48000','WAV','float','-o','4','-i','0','-z','64','-D','0','-m','262144'],capture_output=True,text=True,timeout=30)
    assert rr.returncode==0 and 'FAILURE' not in rr.stdout+rr.stderr,rr.stdout+rr.stderr
    audio,ch,sr=osc.read_wav(wav)
    assert all(math.isfinite(v) for v in audio)
    endpoint=near_midpoint=0.0
    for frame in range(round(0.25*sr),round(0.65*sr)):
        a,b,c,d=audio[frame*ch:frame*ch+4]
        endpoint+=(d-a)**2
        near_midpoint+=(c-b)**2
    ratio=math.sqrt(near_midpoint/endpoint)
    assert endpoint>1e-6 and ratio<0.04,('mode still switches at midpoint',ratio)
    results.append(dict(processor='Echo filter modes',midpoint_to_endpoint_difference_ratio=ratio))
    (out/'measurements.json').write_text(json.dumps(results,indent=2)+'\n')
    print('PASS: modified DSP compiles; EQ and SampleBox compressor/saturator render exact fractional bypasses; Echo modes cross 0.5 continuously')

if __name__=='__main__': main()
