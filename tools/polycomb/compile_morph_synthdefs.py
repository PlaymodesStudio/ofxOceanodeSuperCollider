#!/usr/bin/env python3
"""Build the production PolyComb definitions needed by the audio regressions.

Writes only the addon's ignored CompiledSynthdefs cache, never the app library.
"""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
SOURCES=ROOT/'synthdefs/Defaults/PolyComb'
with tempfile.TemporaryDirectory(prefix='polycomb-morph-compile-') as directory:
    work=Path(directory)
    config=work/'config.yaml'
    config.write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+
                      '\n  - '+str(ROOT/'ugens/OceanodeAnalog')+'\nexcludeDefaultPaths: true\n')
    script='Archive.archiveDir='+json.dumps(str(work))+';\ntry {\n'
    for name in ('polyCombParts.scd','polyCombExtras.scd'):
        script+='if(this.executeFile('+json.dumps(str(SOURCES/name))+').isNil) { Error("PolyComb compile failed").throw; };\n'
    script+='"POLYCOMB MORPH DEFINITIONS COMPILED".postln; 0.exit;\n} { |error| error.reportError; 1.exit; };\n'
    wrapper=work/'compile.scd';wrapper.write_text(script)
    try:
        result=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(wrapper)],
                              capture_output=True,text=True,timeout=60)
    except subprocess.TimeoutExpired as error:
        raise AssertionError((error.stdout or b'').decode(errors='replace')+
                             (error.stderr or b'').decode(errors='replace')) from error
    assert result.returncode==0 and 'POLYCOMB MORPH DEFINITIONS COMPILED' in result.stdout,result.stdout+result.stderr
    cache=SOURCES/'CompiledSynthdefs/PolyComb'
    required=[cache/(prefix+str(n)+'.scsyndef')
              for prefix,count in (('PCPitch',32),('PCOscSine',32),('PCCombR',32),
                                   ('PCVoiceMorph',16),('PolyCombSpread',16),('PCSpreadWide',16))
              for n in range(1,count+1)]
    assert all(path.is_file() and path.stat().st_size>0 for path in required), 'Missing compiled PolyComb definitions'
print(f'PASS: production Parts/Extras compile; {len(required)} required pitch, resonator, mixer and spread definitions verified')
