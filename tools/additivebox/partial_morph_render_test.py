#!/usr/bin/env python3
"""Render the real C++ AdditiveBox graph morphing 8 <-> 256 partials.

The native fixture verifies retained node IDs, stable note writers, spectrum
normalization and idle cleanup, and captures its production OSC. No audio device
or live SuperCollider server is used. Generator iteration ranges alone are reduced.
"""
import argparse,importlib.util,json,math,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('osc',ROOT/'tools/aliasingbox/render_test.py')
osc=importlib.util.module_from_spec(spec);spec.loader.exec_module(osc)
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output',type=Path,default=Path('/tmp/additive-partial-morph'))
 parser.add_argument('--native-binary',type=Path,help='Reuse a binary built from the current production engine-contract fixture')
 args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text()
 source=source.replace('caps = [8, 16, 32, 64, 128, 256]','caps = [8, 256]').replace('chunks = [8, 16, 32, 64]','chunks = [8, 64]').replace('maxOut = 16','maxOut = 1')
 (out/'additiveBox.scd').write_text(source)
 (out/'modulation.scd').write_text((ROOT/'synthdefs/Defaults/Modulation/modulation.scd').read_text())
 config=out/'sclang.yaml';config.write_text('includePaths:\n  - '+json.dumps(str(ROOT/'sclang/SCClassLibrary'))+'\nexcludeDefaultPaths: true\n')
 wrapper=out/'compile.scd';wrapper.write_text('Archive.archiveDir='+json.dumps(str(out))+';"CAP DSP START".postln;try{'+''.join('this.executeFile('+json.dumps(str(out/name))+');'for name in ['additiveBox.scd','modulation.scd'])+'"CAP DSP DONE".postln;0.exit;}{|error|error.reportError;1.exit;};\n')
 with (out/'compile.log').open('w')as log:
  compiled=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(wrapper)],stdout=log,stderr=subprocess.STDOUT,timeout=180)
 log=(out/'compile.log').read_text().split('CAP DSP START')[-1]
 assert compiled.returncode==0 and 'ERROR:'not in log and 'CAP DSP DONE'in log,log[-6000:]
 with (out/'native.log').open('w')as log:
  if args.native_binary:
   with tempfile.TemporaryDirectory(prefix='additive-cap-capture-')as scratch:
    fixture=subprocess.run([str(args.native_binary.resolve()),'unused',scratch],stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1','BOX_MORPH_SCORE':str(out/'additive-cap.osc'),'BOX_MORPH_DEFS':str(out/'CompiledSynthdefs')},timeout=60)
  else:
   fixture=subprocess.run([sys.executable,str(ROOT/'tools/snapshot_morph_test/engine_contract_test.py'),'--capture-dir',str(out)],stdout=log,stderr=subprocess.STDOUT,timeout=600)
 assert fixture.returncode==0,(out/'native.log').read_text()[-5000:]
 command=['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'additive-cap.osc'),'_',str(out/'audio.wav'),'48000','WAV','float','-o','1','-i','0','-a','256','-c','65536','-b','1024','-m','262144','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins']
 render=subprocess.run(command,capture_output=True,text=True,timeout=60);log=render.stdout+render.stderr;(out/'render.log').write_text(log)
 assert render.returncode==0 and 'FAILURE'not in log and 'not found'not in log,log[-6000:]
 audio,ch,sr=osc.read_wav(out/'audio.wav');assert ch==1 and all(math.isfinite(x)for x in audio)
 peak=max(map(abs,audio));assert 1e-5<peak<1,peak
 # At the two former midpoint switches, use adjacent ordinary audio windows
 # as the reference slope. A retained sine bank has no new phase-reset impulse.
 measurements=[]
 for when in [.8,2.0]:
  def slope(start,end):
   return max(abs(audio[i]-audio[i-1])for i in range(int(start*sr),int(end*sr)))
  nearby=max(slope(when-.05,when-.005),slope(when+.015,when+.06))
  switch=slope(when-.002,when+.01)
  assert switch<max(1e-5,nearby*3),(when,switch,nearby)
  measurements.append({'time':when,'switchSlope':switch,'nearbySlope':nearby})
 (out/'report.json').write_text(json.dumps({'peak':peak,'midpoints':measurements},indent=2)+'\n')
 print('PASS: production C++/SC graph renders 8 <-> 256 partial morphs; retained IDs, linear early presence, stable notes, clean retirement, finite audio and no midpoint impulse')
if __name__=='__main__':main()
