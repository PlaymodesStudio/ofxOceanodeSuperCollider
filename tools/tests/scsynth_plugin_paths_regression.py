#!/usr/bin/env python3
"""Test engine-specific UGen search paths; optionally render with real scsynth (no audio device)."""
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--source-root',type=Path,default=ROOT/'src')
 parser.add_argument('--scsynth',type=Path)
 parser.add_argument('--extensions',type=Path)
 args=parser.parse_args()
 with tempfile.TemporaryDirectory(prefix='sc-plugin-regression-') as folder:
  folder=Path(folder); exe=folder/'test'
  subprocess.run(['clang++','-std=c++17','-g','-fsanitize=address,undefined','-I'+str(args.source_root),str(Path(__file__).with_name('scsynth_plugin_paths_test.cpp')),'-o',str(exe)],check=True)
  if not args.scsynth:
   subprocess.run([str(exe)],check=True);return
  assert args.extensions
  host=subprocess.Popen([str(exe),str(args.extensions),str(args.scsynth.parent)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
  try:
   paths=host.stdout.readline().strip();assert paths
   # Last timestamp determines the NRT duration. No audio hardware is opened.
   message=b'/c_set\0\0'+b',if\0'+struct.pack('>if',0,0.0)
   bundle=b'#bundle\0'+struct.pack('>Q',int(.02*(1<<32)))+struct.pack('>I',len(message))+message
   score=folder/'score.osc';score.write_bytes(struct.pack('>I',len(bundle))+bundle)
   def render(paths,name):
    result=subprocess.run([str(args.scsynth),'-N',str(score),'_',str(folder/(name+'.wav')),'44100','WAV','float','-D','0','-U',paths],text=True,capture_output=True,timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout+result.stderr
   original=render(str(args.extensions)+':'+str(args.scsynth.parent),'original')
   filtered=render(paths,'filtered')
   assert 'Class CocoaEditorWindow is implemented in both' in original,'Original bundle did not reproduce the duplicate class warning'
   assert 'is implemented in both' not in filtered,filtered
   assert 'VSTPlugin 0.6.1' in filtered,filtered
   assert 'UGen' not in filtered or 'duplicate' not in filtered.lower(),filtered
   print('REAL_SCSYNTH_TEST_OK: reproduced original duplicate classes; filtered NRT render loads VSTPlugin without duplicates.')
  finally:
   host.communicate('\n',timeout=10)
   assert host.returncode==0
if __name__=='__main__':main()
