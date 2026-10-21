#!/usr/bin/env python3
"""Check SteamPipe's stored constants, including misleading renamed labels."""
import argparse,json,pathlib,struct,subprocess
HERE=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('ensemble',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);a=p.parse_args()
report=json.loads(subprocess.check_output([str(HERE/'dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'),str(a.ensemble)],text=True));raw=a.ensemble.read_bytes();modules={}
def walk(nodes):
 for n in nodes:modules[n['object_id']]=n;walk(n['children'])
walk(report['top_level'])
assert report['counts']['modules']==950
assert report['counts']['connections']==report['counts']['resolved_connections']==991
assert report['counts']['snapshots']==172
assert modules[212126]['constant_numeric_value']==-60,'Feedback keytrack constant is labelled 0, but stores -60.'
assert modules[212501]['constant_numeric_value']==112,'R66 damping constant is labelled 96, but stores 112.'
constants=[n for n in modules.values() if n['kind']==100]
for n in constants:
 actual=struct.unpack_from('<f',raw,int(n['offset'],16)+0x44)[0]
 assert n['constant_numeric_value']==actual,(n['object_id'],n.get('constant_numeric_value'),actual)
if a.output:a.output.write_text(json.dumps(report))
print(f'PASS: {len(constants)} stored constants, including feedback -60 and R66 112; 991 wires; 172 snapshots.')
