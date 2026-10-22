#!/usr/bin/env python3
"""Read the global module state only; snapshot/bank records are deliberately ignored."""
import hashlib,json,pathlib,sys
report=json.loads(pathlib.Path(sys.argv[1]).read_text())
def walk(ms):
 for m in ms:
  yield m
  yield from walk(m.get('children',[]))
controls={str(m['object_id']):{'path':m['hierarchy_path'],'value':m['saved_control_value'],'normalized':m['saved_control_normalized']} for m in walk(report['top_level']) if 'saved_control_value' in m}
assert report['counts']['modules']==382 and report['counts']['resolved_connections']==451
assert not report['warnings'],report['warnings']
# Cross-checked against the actual opening panel, including the *modified* micro8 state.
for mid,v in [(1950,-38),(1956,-6),(1484,945),(1542,8),(1558,1),(1571,1),(23,100)]:assert abs(controls[str(mid)]['value']-v)<.001,(mid,controls[str(mid)])
state={'source':pathlib.Path(report['source']).name,'sha256':hashlib.sha256(pathlib.Path(report['source']).read_bytes()).hexdigest(),'scope':'saved opening state only; no snapshots imported','voices':1,'voice_count_evidence':'Reaktor Instrument properties: Voices 1, Min Voices 1, Max Voices 1 (2026-10-22)','bpm':100,'controls':controls,'selected_lists':{'wave':'noise','delay_filter':'HP','delay_wave':'RND'},'counts':report['counts']}
(pathlib.Path(__file__).parent/'opening-state.json').write_text(json.dumps(state,indent=2)+'\n')
print('Recovered',len(controls),'global controls; no snapshot state used')
