#!/usr/bin/env python3
"""Every key of AnimaBox's factory sounds (reaktor/factory.json) must be a control AnimaBox
registers: its add("...") sound controls, Bands / Scale, or a shared LFO 1-2 control
(scModulation.cpp: L + "..." + X). A typo would silently fall back to the default."""
import json, re, sys, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
esc = lambda n: n.replace(' ', '_')
node = (ROOT / 'src/nodes/boxSynths/animaBox/scAnimaBox.cpp').read_text()
modc = (ROOT / 'src/nodes/boxSynths/shared/scModulation.cpp').read_text()
names = {esc(n) for n in re.findall(r'\badd\("([^"]+)"', node)} | {'Bands', 'Scale'}
lfo = set(re.findall(r'L \+ "([^"]+)" \+ X', modc)) | set(re.findall(r'L\+"([^"]+)"\+X', modc))
for i in (1, 2): names |= {'LFO%d_%s' % (i, esc(n)) for n in lfo}
names |= {'animaModLfos'}
bad = set()
for snap in json.load(open(ROOT / 'tools/animabox/reaktor/factory.json')):
    bad |= {k for k in snap['values'] if k not in names}
print('unknown keys:', sorted(bad) if bad else 'none')
sys.exit(1 if bad else 0)
