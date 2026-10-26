#!/usr/bin/env python3
"""Render the production PhysicalVoice across fractional modal counts.

An optional --baseline-plugin directory enables sample-exact integer endpoint
comparisons against a previously built plugin. Nothing is installed globally.
"""
import argparse, importlib.util, json, math, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('osc', ROOT/'tools/aliasingbox/render_test.py')
osc = importlib.util.module_from_spec(spec); spec.loader.exec_module(osc)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/tmp/physical-modal-morph'))
    parser.add_argument('--baseline-plugin', type=Path)
    parser.add_argument('--plugin-dir', type=Path, default=ROOT/'ugens/OceanodePhysical')
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    config = out/'sclang.yaml'
    config.write_text('includePaths:\n  - '+json.dumps(str(ROOT/'sclang/SCClassLibrary'))+'\n  - '+json.dumps(str(ROOT/'ugens/OceanodePhysical'))+'\nexcludeDefaultPaths: true\n')
    script = out/'compile.scd'
    script.write_text('Archive.archiveDir='+json.dumps(str(out))+';try { SynthDef(\\ModalMorph, {\n'
        'var seed=RandSeed.ir(1,1234); var pairs=PhysicalVoice.names.collect{|n|[n,NamedControl.kr(n,PhysicalVoice.defaults[PhysicalVoice.names.indexOf(n)])]}.flatten;\n'
        'Out.ar(0,PhysicalVoice.ar(0,pairs));}).writeDefFile('+json.dumps(str(out))+'); "MODAL DONE".postln;0.exit;} {|e|e.reportError;1.exit;};\n')
    with (out/'compile.log').open('w') as log:
        result = subprocess.run([str(ROOT/'sclang/osx/sclang'), '-u', '0', '-D', '-l', str(config), str(script)], stdout=log, stderr=subprocess.STDOUT, timeout=60)
    assert result.returncode == 0 and 'MODAL DONE' in (out/'compile.log').read_text()
    definition = out/'CompiledSynthdefs/ModalMorph.scsyndef'
    if not definition.exists(): definition = out/'ModalMorph.scsyndef'
    stock = '/Applications/SuperCollider.app/Contents/Resources/plugins'
    def render(name, count, plugin, transitions=()):
        events = [(0,osc.message('/d_recv',definition.read_bytes())), (0,osc.message('/g_new',1,0,0)),
            (.02,osc.message('/s_new','ModalMorph',100,1,1,'bmodes',float(count),'bfamily',0.,'freq',65.,'excite',1.,'listen',1.,'bdecay',4.,'hardness',1.,'cascade',.15)),
            (.05,osc.message('/n_set',100,'gate',.8))]
        for when, value in transitions: events.append((when,osc.message('/n_set',100,'bmodes',float(value))))
        events += [(1.5,osc.message('/g_freeAll',1)), (1.51,osc.message('/c_set',0,0.))]
        score = out/(name+'.osc'); score.write_bytes(b''.join(osc.bundle(t,[m]) for t,m in sorted(events,key=lambda e:e[0])))
        command = ['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(out/(name+'.wav')),'48000','WAV','float','-o','2','-i','0','-m','65536','-D','0','-U',stock+':'+str(plugin)]
        run = subprocess.run(command,capture_output=True,text=True,timeout=30)
        log = run.stdout+run.stderr; (out/(name+'.log')).write_text(log)
        assert run.returncode == 0 and 'FAILURE' not in log and 'not found' not in log,log
        samples,ch,sr = osc.read_wav(out/(name+'.wav'))
        assert ch == 2 and all(math.isfinite(v) for v in samples)
        assert 1e-6 < max(map(abs,samples)) < 2
        return samples,sr
    plugin = args.plugin_dir.resolve()
    endpoint_errors = {}
    for count in [1,7,8,11,12,32,48]:
        samples,sr = render('count'+str(count),count,plugin)
        if args.baseline_plugin:
            old,_ = render('baseline'+str(count),count,args.baseline_plugin.resolve())
            error = max(abs(a-b) for a,b in zip(samples,old)); endpoint_errors[count] = error
            assert error == 0,(count,error)
    # Compare just around former count/cascade boundaries. Both runs have
    # identical histories before the control change; the short comparison
    # avoids long-term divergence from the model's chaotic feedback.
    errors = {}
    for boundary in [8,12,32]:
        a,sr = render('below'+str(boundary),boundary-1,plugin,[(.5,boundary-.0001)])
        b,_ = render('above'+str(boundary),boundary-1,plugin,[(.5,boundary+.0001)])
        error = max(abs(a[i]-b[i]) for i in range(int(.5*sr)*2,int(.503*sr)*2))
        errors[boundary] = error
        assert error < .0002,(boundary,error)
    (out/'report.json').write_text(json.dumps({'integerEndpointErrors':endpoint_errors,'boundaryErrors':errors},indent=2)+'\n')
    print('PASS: PhysicalVoice fractional mode counts, cascade boundaries and finite audio; integer endpoint null tests:',len(endpoint_errors))

if __name__ == '__main__': main()
