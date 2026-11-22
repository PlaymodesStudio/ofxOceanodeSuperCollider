"""Reaktor t'wanGer snapshots (decoded by decode_snapshots.py) as panel values.

Each preset maps to the Twanger / TwangerFX controls in the Reaktor panel's own
units. Switches are stored 1-based: position k selects input k-1.
"""
import json, pathlib
HERE = pathlib.Path(__file__).resolve().parent

# snapshot control id -> (control name, kind); kinds: 'knob', 'switch'
CONTROLS = {
    0: 'r1damp', 1: 'r1cut', 2: 'r1undamp', 3: 'r1res', 4: 'r1dec', 5: 'r1rel', 6: 'r1amt',
    10: 'rel', 11: 'att', 12: 'cut', 13: 'res', 14: 'vel',
    15: 'r2damp', 16: 'r2hicut', 17: 'r2tune', 21: 'r2on',
    22: 'spread', 23: 'filter',
    24: 'treb', 25: 'sub', 26: 'freq', 27: 'mid', 28: 'bass', 29: 'sup',
    30: 'scale', 31: 'trans', 32: 'dton', 33: 'timbre',
    34: 'ap1dly', 35: 'ap1pol', 36: 'ap1fine', 37: 'ap2dly', 38: 'ap2pol', 39: 'ap2fine',
    40: 'ap3dly', 41: 'ap3pol', 42: 'ap3fine', 43: 'ap4dly', 44: 'ap4pol', 45: 'ap4fine',
    46: 'delay', 47: 'ap5dly', 48: 'ap5pol', 49: 'ap5fine',
    50: 'time', 51: 'rev', 52: 'rndamp', 53: 'rndrate', 54: 'lpf', 55: 'drywet', 56: 'rvon',
    58: 'level',
}
# switches whose position 1 (input 0) is "on": the processed path
ON_FIRST = {'r2on', 'dton', 'rvon'}
# allpass feedback polarity: position 1 = "+", 2 = "-"
POLARITY = {'ap1pol', 'ap2pol', 'ap3pol', 'ap4pol', 'ap5pol'}

def load():
    data = json.loads((HERE / 'twanger_snapshots.json').read_text())
    controls = {c['sid']: c for c in data['controls']}
    presets = []
    for snap in data['snapshots']:
        values = {}
        for sid_s, v in snap['values'].items():
            sid = int(sid_s)
            name = CONTROLS.get(sid)
            if name is None or v is None: continue
            c = controls[sid]
            if c['kind'] == 'Switch':
                k = int(v)
                if name == 'filter': values[name] = k - 1
                elif name in ON_FIRST: values[name] = 1 if k == 1 else 0
                elif name in POLARITY: values[name] = 1 if k == 1 else -1
            else:
                values[name] = c['min'] + float(v) * (c['max'] - c['min'])
        presets.append({'name': snap['name'], 'values': values})
    return presets, controls

if __name__ == '__main__':
    presets, _ = load()
    for p in presets[:3]:
        print(p['name'], json.dumps({k: round(v, 3) for k, v in p['values'].items()}))
