#!/usr/bin/env python3
"""Render an Oceanode preset made only of SC nodes, macros and routers, offline.

    render_preset.py <preset folder> <Synthdefs folder> <out.wav> [seconds] [sr] [transport BPM; 0 = stopped]

Mimics Oceanode's wiring: every SC output gets its own audio bus, an input
reads its source's bus (unconnected: a silent bus), a connected parameter is
switched to its audio-rate twin (<name>_sel 1, <name>_ar mapped), routers
pass values or buses through, Router f values set the parameters they feed.
"""
import json, pathlib, struct, subprocess, sys, re, os
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

def string(s):
    b = s.encode() + b'\0'; return b + b'\0' * ((-len(b)) % 4)
def message(address, *args):
    tags = ','; data = b''
    for v in args:
        if isinstance(v, str): tags += 's'; data += string(v)
        elif isinstance(v, bytes): tags += 'b'; data += struct.pack('>I', len(v)) + v + b'\0' * ((-len(v)) % 4)
        elif isinstance(v, int): tags += 'i'; data += struct.pack('>i', v)
        else: tags += 'f'; data += struct.pack('>f', v)
    return string(address) + string(tags) + data
def bundle(t, messages):
    b = b'#bundle\0' + struct.pack('>Q', round(t * 2 ** 32))
    for m in messages: b += struct.pack('>I', len(m)) + m
    return struct.pack('>I', len(b)) + b

def synthdef_controls(path):
    """name -> (index, size) of a SynthDef2 file's controls."""
    b = path.read_bytes(); p = 10
    def i32():
        nonlocal p; v = struct.unpack_from('>i', b, p)[0]; p += 4; return v
    def pstr():
        nonlocal p; n = b[p]; s = b[p + 1:p + 1 + n].decode(); p += 1 + n; return s
    p = 8; ndefs = struct.unpack_from('>h', b, p)[0]; p += 2
    pstr(); nk = i32(); p += 4 * nk; np_ = i32(); p += 4 * np_
    nn = i32(); names = []
    for _ in range(nn): names.append((pstr(), i32()))
    names.sort(key=lambda x: x[1])
    out = {}
    for i, (n, idx) in enumerate(names):
        end = names[i + 1][1] if i + 1 < len(names) else np_
        out[n] = (idx, end - idx)
    return out

preset = pathlib.Path(sys.argv[1]); defroot = pathlib.Path(sys.argv[2]); wav = sys.argv[3]
seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
sr = int(sys.argv[5]) if len(sys.argv) > 5 else 44100
transport_bpm = float(sys.argv[6]) if len(sys.argv) > 6 else 0.
ROOT = pathlib.Path(__file__).resolve().parents[2]
deffiles = {p.stem: p for p in defroot.rglob('*.scsyndef')}
deffiles['tap'] = pathlib.Path(wav).parent / 'tap.scsyndef'
# Load only registered nodes. Auxiliary JSON files and stale node files are
# not part of the patch (matching Oceanode's modules.json loader).
nodes, edges = {}, []
def load(folder, prefix):
    modules = json.loads((folder / 'modules.json').read_text())
    for type_, positions in modules.items():
        for index in positions:
            key = type_.replace(' ', '_') + '_' + str(int(index))
            nodes[prefix + key] = json.loads((folder / (key + '.json')).read_text())
            if type_ == 'Macro': load(folder / key, prefix + key + '/')
    conns = json.loads((folder / 'connections.json').read_text())
    for src, outs in conns.items():
        for out, targets in outs.items():
            for dst, params in targets.items():
                for param in params: edges.append((prefix+src, out, prefix+dst, param))
load(preset, '')
def router_of(macro, name):
    for k, v in nodes.items():
        if k.startswith(macro + '/') and k.count('/') == macro.count('/')+1 and k.split('/')[-1].startswith('Router') and v.get('Name') == name:
            return k
    raise KeyError((macro, name))
resolved = []
for src, out, dst, param in edges:
    if src.split('/')[-1].startswith('Macro_'): src, out = router_of(src, out.replace('_', ' ')), 'Value'
    if dst.split('/')[-1].startswith('Macro_'): dst, param = router_of(dst, param.replace('_', ' ')), 'Value'
    resolved.append((src, out, dst, param))
incoming = {}
for edge in resolved: incoming.setdefault(edge[2], []).append(edge)
def origins(node, out, visited=()):
    if node in visited: raise ValueError('Router cycle: ' + node)
    if node.split('/')[-1].startswith('Beat_Transport_'):
        # The optional fixed-tempo transport starts at beat zero. TransportPulse
        # interpolates its beat position at audio rate after this initial anchor.
        value = (transport_bpm or 120.) if out == 'BPM' else float(transport_bpm > 0) if out == 'Playing' else 0.
        return [('const', [value])]
    def numeric_input(name, fallback):
        ins = [e for e in incoming.get(node, []) if e[3] == name]
        if not ins: return nodes[node].get(name, fallback)
        org = origins(ins[-1][0], ins[-1][1], (*visited, node))
        assert len(org) == 1 and org[0][0] == 'const', ('Expected numeric input', node, name)
        value = org[0][1]
        return value if isinstance(value, list) else [value]
    if node.split('/')[-1].startswith('Vector_Getter_'):
        values = numeric_input('Input', [0, 0])
        return [('const', [values[i] for i in nodes[node]['Index'] if i < len(values)])]
    if node.split('/')[-1].startswith('Vector_Operations_'):
        assert nodes[node]['Op.'] == 6
        return [('const', [len(numeric_input('Input', [0]))])]
    if node.split('/')[-1].startswith('Router'):
        ins = incoming.get(node, [])
        if ins: return [o for s, port, _, _ in ins for o in origins(s, port, (*visited, node))]
        macro = node.rsplit('/', 1)[0] if '/' in node else ''
        data = nodes[node]
        value = nodes.get(macro, {}).get(data['Name'].replace(' ', '_'), data.get('Value', [0]))
        return [('const', value)]
    return [('bus', node, out)]
sc = {k: v for k, v in nodes.items() if k.split('/')[-1].startswith('SC_') and not k.split('/')[-1].startswith('SC_Output')}
def defname(key):
    v = nodes[key]; base = re.sub(r'^SC_|\*_\d+$', '', key.split('/')[-1])
    return base + v['N_Chan'] + ('_' + v['NumSpeakers'] if 'NumSpeakers' in v else '')
nextbus, silent = 256, 32000
buses = {}
def allocate():
    global nextbus
    result = nextbus; nextbus += 128
    assert nextbus < silent, 'Not enough test buses'
    return result
def bus_for(node, out):
    if (node, out) not in buses: buses[(node, out)] = allocate()
    return buses[(node, out)]
order, seen, visiting = [], set(), set()
def visit(key):
    if key in seen: return
    if key in visiting: raise ValueError('SC cycle: ' + key)
    visiting.add(key)
    for src, out, dst, param in resolved:
        if dst == key:
            for origin in origins(src, out):
                if origin[0] == 'bus': visit(origin[1])
    visiting.remove(key); seen.add(key); order.append(key)
for key in sc: visit(key)
required = {deffiles[defname(k)] for k in sc} | {deffiles['tap'], ROOT / 'data/Supercollider/Synthdefs/Defaults/AudioRouting/audioBusMixer.scsyndef'}
score = [bundle(0, [message('/d_load', str(p)) for p in sorted(required)])]
setup = [message('/g_new', 1, 0, 0)]; nid = 2000; sum_count = 0

def mix_buses(values):
    global nid, sum_count
    result = values[0]
    for value in values[1:]:
        target = allocate()
        setup.append(message('/s_new', 'oceanodeAudioBusMixer128', nid, 1, 1,
            'inA', result, 'inB', value, 'out', target, 'gainA', 1., 'gainB', 1., 'smoothing', 0.))
        nid += 1; sum_count += 1; result = target
    return result

def set_value(node_id, control, value, controls):
    if isinstance(value, str): value = [float(x.strip()) for x in value.split(',')]
    if not isinstance(value, list): value = [value]
    if len(value) == 1: value *= controls[control][1]
    assert len(value) == controls[control][1], (control, value, controls[control])
    return message('/n_setn', node_id, control, len(value), *[float(x) for x in value])
for key in order:
    v = nodes[key]; name = defname(key); ctl = synthdef_controls(deffiles[name])
    args, late = [name, nid, 1, 1], []
    node_id = nid; nid += 1
    input_names = ('in', 'ina', 'inb', 'in1', 'in2', 'trigin', 'sidechain', 'clock')
    for c in ctl:
        if c in input_names: args += [c, silent]
    for pk, pv in v.items():
        if pk in ('N_Chan', 'expanded', 'NumSpeakers'): continue
        c = pk.lower()
        if c in ctl: late.append(set_value(node_id, c, pv, ctl))
    for out in [c for c in ctl if c in ('out', 'trigout', 'grout')]:
        args += [out, bus_for(key, out[0].upper()+out[1:])]
    by_param = {}
    for src, out, dst, param in resolved:
        if dst == key: by_param.setdefault(param.lower(), []).extend(origins(src, out))
    for c, orgs in by_param.items():
        bus_origins = [o for o in orgs if o[0] == 'bus']
        if not bus_origins:
            # Oceanode leaves the last synth value when a vector becomes empty.
            if c in ctl and orgs[-1][1] != []: late.append(set_value(node_id, c, orgs[-1][1], ctl))
            continue
        bus = mix_buses([bus_for(o[1], o[2]) for o in bus_origins])
        if c in input_names: late.append(message('/n_set', node_id, c, bus))
        elif c+'_ar' in ctl:
            late += [message('/n_set', node_id, c+'_sel', 1), message('/n_mapan', node_id, c+'_ar', bus, ctl[c+'_ar'][1])]
        else: raise ValueError(('Unmapped input', key, c))
    setup.append(message('/s_new', *args)); setup += late
finals = []
for src, out, dst, param in resolved:
    if dst.split('/')[-1].startswith('SC_Output'):
        finals += [o for o in origins(src, out) if o[0] == 'bus']
final = mix_buses([bus_for(o[1], o[2]) for o in finals])
setup.append(message('/s_new', 'tap', nid, 1, 1, 'from', final))
score += [bundle(.01, setup), bundle(seconds-.01, [message('/g_freeAll', 1)]), bundle(seconds, [message('/c_set', 0, 0)])]
osc = pathlib.Path(wav).with_suffix('.osc'); osc.write_bytes(b''.join(score))
print('SC nodes:', len(order), 'implicit sums:', sum_count, 'audio buses:', nextbus)
plugins = '/Applications/SuperCollider.app/Contents/Resources/plugins:' + str(ROOT / 'ugens/OceanodeAnalog') + ':' + str(pathlib.Path.home() / 'Library/Application Support/SuperCollider/Extensions/SC3plugins')
r = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(osc), '_', wav, str(sr), 'WAV', 'float',
    '-o', '4', '-i', '0', '-a', '32768', '-z', '64', '-m', '262144', '-w', '8192', '-D', '0', '-U', plugins],
    capture_output=True, text=True, timeout=300)
print(r.stdout[-3000:] + r.stderr[-3000:])
if r.returncode or any(x in r.stdout+r.stderr for x in ('FAILURE', 'exception', 'ERROR:')):
    raise RuntimeError('scsynth failed: ' + str(r.returncode))
