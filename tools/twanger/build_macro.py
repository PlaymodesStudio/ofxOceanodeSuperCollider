#!/usr/bin/env python3
"""Writes the global Oceanode macro TWANGER_GUI: the Reaktor ensemble t'wanGer.

  build_macro.py <output folder>      e.g. <app>/bin/data/Macros/AUDIO/REAKTOR/TWANGER_GUI

Inside: one router per Reaktor panel control (its name, range and the value of
preset 001) plus Pitch, Gate and the ScBus output; SC Twanger* (8 notes, 2
voices each) -> SC TwangerFX* -> out; a Lissajous showing X = L, Y = R - L like
the ensemble's XY display; the custom GUI's Randomizer widget is patCh, with the
knobs grouped by the MIDI CC they shared in Reaktor. The custom GUI follows the
Reaktor panel's layout (10 px per screenshot pixel pair) with the display on top
and the 38 Reaktor snapshots in a matrix below it.
"""
import json, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import twanger_presets

PANEL_ID = 'custom_gui_20261122_twanger_1'
PANEL_NAME = "t'wanGer"
ORANGE = [168, 84, 0, 255]
FRAME = [150, 74, 0, 255]
KNOB_BODY = [55, 55, 55, 255]
LIT = [255, 160, 60, 255]
BLACK = [0, 0, 0, 255]

# (router name, preset key, SC node, SC parameter, min, max, kind, (x, y) of the knob centre in the screenshot)
# kind: f knob, b toggle, i radio
CONTROLS = [
    ('atT', 'att', 'T', 'Att', 0, 30, 'f', (112, 390)),
    ('reL', 'rel', 'T', 'Rel', 30, 65, 'f', (200, 390)),
    ('cuT', 'cut', 'T', 'Cut', 0, 160, 'f', (112, 487)),
    ('reS', 'res', 'T', 'Res', 0, 0.995, 'f', (200, 487)),
    ('veL', 'vel', 'T', 'Vel', 24, 100, 'f', (112, 580)),
    ('spreAd', 'spread', 'T', 'Spread', 0, 7, 'f', (200, 580)),
    ('filTer', 'filter', 'T', 'Filter', 0, 7, 'i', None),
    ('suB', 'sub', 'T', 'Sub', -20, 20, 'f', (441, 430)),
    ('baSs', 'bass', 'T', 'Bass', -20, 20, 'f', (520, 430)),
    ('miD', 'mid', 'T', 'Mid', -16, 16, 'f', (441, 517)),
    ('frEq', 'freq', 'T', 'Freq', 200, 2000, 'f', (520, 517)),
    ('treB', 'treb', 'T', 'Treb', -8, 8, 'f', (441, 605)),
    ('suP', 'sup', 'T', 'Sup', -8, 8, 'f', (520, 605)),
    ('r1 daMp', 'r1damp', 'T', 'R1damp', 0.02, 6, 'f', (712, 430)),
    ('r1 undaMp', 'r1undamp', 'T', 'R1undamp', 0, 12, 'f', (800, 430)),
    ('r1 cuT', 'r1cut', 'T', 'R1cut', -60, 0, 'f', (680, 517)),
    ('r1 reS', 'r1res', 'T', 'R1res', 0, 0.6, 'f', (760, 517)),
    ('r1 aMt', 'r1amt', 'T', 'R1amt', 0, 12700, 'f', (840, 517)),
    ('r1 deC', 'r1dec', 'T', 'R1dec', 0, 65, 'f', (720, 605)),
    ('r1 reL', 'r1rel', 'T', 'R1rel', 0, 65, 'f', (808, 605)),
    ('r2 hiCut', 'r2hicut', 'T', 'R2hicut', 24, 100, 'f', (990, 430)),
    ('r2 tuNe', 'r2tune', 'T', 'R2tune', -36, 36, 'f', (1088, 430)),
    ('r2 daMp', 'r2damp', 'T', 'R2damp', -2, -0.1, 'f', (990, 517)),
    ('r2 oN', 'r2on', 'T', 'R2on', 0, 1, 'b', (1094, 517)),
    ('timBre', 'timbre', 'T', 'Timbre', -60, 60, 'f', (113, 783)),
    ('scaLe', 'scale', 'T', 'Scale', 0.1, 2, 'f', (200, 783)),
    ('traNs', 'trans', 'T', 'Trans', -60, 60, 'f', (272, 783)),
    ('dt oN', 'dton', 'T', 'Dton', 0, 1, 'b', (333, 781)),
    ('delAy', 'delay', 'X', 'Delay', 5, 500, 'f', (464, 790)),
    ('tiMe', 'time', 'X', 'Time', 0.1, 1, 'f', (544, 790)),
    ('reV', 'rev', 'X', 'Rev', 0.1, 10, 'f', (624, 790)),
    ('lPf', 'lpf', 'X', 'Lpf', 80, 135, 'f', (705, 790)),
    ('dryWet', 'drywet', 'X', 'Drywet', 0, 1, 'f', (786, 790)),
    ('rv oN', 'rvon', 'X', 'Rvon', 0, 1, 'b', (870, 790)),
    ('leVel', 'level', 'X', 'Level', -60, 0, 'f', (1065, 715)),
]
# Reaktor MIDI CC of each knob (shared CC = same random draw on patCh); leVel has none
CC = {'r1damp': 78, 'r1cut': 81, 'r1undamp': 79, 'r1res': 84, 'r1dec': 82, 'r1rel': 83, 'r1amt': 85,
      'rel': 74, 'att': 71, 'cut': 75, 'res': 76, 'vel': 77, 'r2damp': 72, 'r2hicut': 73, 'r2tune': 71,
      'spread': 73, 'treb': 74, 'sub': 71, 'freq': 73, 'mid': 72, 'bass': 70, 'sup': 75,
      'scale': 71, 'trans': 71, 'timbre': 73, 'delay': 72, 'time': 73, 'rev': 71, 'lpf': 74, 'drywet': 75}
FILTERS = ['hp4', 'hp1', 'bh4', 'bp4', 'bl4', 'lp1', 'lp4', 'off']
TYPE = {'f': 'f', 'i': 'i', 'b': 'b'}
VEC = 'NSt3__16vectorIfNS_9allocatorIfEEEE'
TW, FX = 'SC_Twanger*_1', 'SC_TwangerFX*_1'

def diffusers(v):
    return [round(float(v.get('ap%ddly' % i, 0)), 4) for i in range(1, 6)] + \
           [round(float(v.get('ap%dfine' % i, 0)), 4) for i in range(1, 6)] + \
           [float(v.get('ap%dpol' % i, 1)) for i in range(1, 6)] + \
           [round(float(v.get('rndamp', 0)), 4), round(float(v.get('rndrate', 1)), 4)]

def num(x): return round(float(x), 6)

def main():
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    presets = twanger_presets.load()[0]
    first = presets[0]['values']

    # ---- routers -------------------------------------------------------------
    counters = {}
    routers = []   # (file key, module type, router name, path of its Value)
    def router(kind_name):
        counters[kind_name] = counters.get(kind_name, 0) + 1
        n = counters[kind_name]
        return 'Router_%s_%d' % (kind_name.replace(' ', '_'), n), n
    files = {}; connections = {}; modules = {}
    def place(mtype, n, x, y):
        modules.setdefault(mtype, {})['%02d' % n] = [float(x), float(y)]
    def connect(src, out_param, dst, in_param):
        connections.setdefault(src, {}).setdefault(out_param, {}).setdefault(dst, {})[in_param] = None

    sort_order = []
    ctl_router = {}
    for i, (name, key, node, param, lo, hi, kind, _) in enumerate(CONTROLS):
        rtype = {'f': 'f', 'i': 'i', 'b': 'b'}[kind]
        fkey, n = router(rtype)
        value = first[key]
        if kind == 'f':
            files[fkey] = {'Exclude_From_Snapshots': '0', 'Max': str(hi), 'Min': str(lo), 'Name': name, 'Value': str(num(value)), 'expanded': True}
        elif kind == 'i':
            files[fkey] = {'Exclude_From_Snapshots': '0', 'Max': str(hi), 'Min': str(lo), 'Name': name, 'Value': str(int(value)), 'expanded': True}
        else:
            files[fkey] = {'Exclude_From_Snapshots': '0', 'Name': name, 'Value': '1' if value else '0', 'expanded': True}
        place('Router ' + rtype, n, 0, 40 * i)
        connect(fkey, 'Value', TW if node == 'T' else FX, param)
        ctl_router[key] = fkey
        sort_order.append(name)

    fkey, n = router('v_f')       # Pitch
    files[fkey] = {'Exclude_From_Snapshots': '1', 'Max': 127.0, 'Min': 0.0, 'Name': 'Pitch', 'Value': [60.0] * 8, 'expanded': True}
    place('Router v_f', n, 0, -120); connect(fkey, 'Value', TW, 'Pitch'); sort_order.insert(0, 'Pitch'); pitch_router = fkey
    fkey, n = router('v_f')       # Gate
    files[fkey] = {'Exclude_From_Snapshots': '1', 'Max': 1.0, 'Min': 0.0, 'Name': 'Gate', 'Value': [0.0] * 8, 'expanded': True}
    place('Router v_f', n, 0, -80); connect(fkey, 'Value', TW, 'Gate'); sort_order.insert(1, 'Gate'); gate_router = fkey
    fkey, n = router('ScBus')     # output
    files[fkey] = {'Exclude_From_Snapshots': '0', 'Name': 'Out', 'expanded': True}
    place('Router ScBus', n, 1500, 300); sort_order.append('Out')
    out_router = fkey

    # ---- nodes -----------------------------------------------------------------
    files[TW] = {'N_Chan': '8', 'Unison': '2', 'expanded': True}
    place('SC Twanger*', 1, 600, 200)
    files[FX] = {'N_Chan': '1', 'Diffusers': diffusers(first), 'expanded': True}
    place('SC TwangerFX*', 1, 1000, 300)
    connect(TW, 'Out', FX, 'In')
    connect(FX, 'Out', out_router, 'Value')
    connect(FX, 'Out', 'Lissajous_1', 'In')
    files['Lissajous_1'] = {'Axes': '1', 'Color': '120, 190, 255, 230', 'Dot_Size': 2.0, 'Draw': '1', 'Fade': 0.5, 'Gain': 1.0,
                            'Server': '0', 'Stroke': 1.0, 'Time_Window': 0.02, 'Trails': '256',
                            'Widget_Height': 120.0, 'Widget_Width': 480.0, 'expanded': True}
    place('Lissajous', 1, 1400, 500)
    # patCh = the panel's Randomizer widget: knobs that shared a Reaktor MIDI CC share a draw;
    # what Reaktor's patCh did not touch (leVel, the switches, the hidden diffusers) is excluded
    rand_groups = {ctl_router[key] + '/Value': CC[key] for (name, key, *_r) in CONTROLS if key in CC}
    rand_exclude = [ctl_router[key] + '/Value' for (name, key, *_r) in CONTROLS if key not in CC] + [FX + '/Diffusers']

    for k, v in files.items():
        (out / (k + '.json')).write_text(json.dumps(v, indent=4))
    (out / 'modules.json').write_text(json.dumps(modules, indent=4))
    (out / 'connections.json').write_text(json.dumps(connections, indent=4))
    # the ensemble's sections as separators among the macro's parameters
    SECTIONS = [('notes', 'Pitch'), ("t'Wanger", 'atT'), ('eQualizer', 'suB'), ('reSonator 1', 'r1 daMp'),
                ('reSonator 2', 'r2 hiCut'), ('(de)Tuner', 'timBre'), ('reVerberator', 'delAy'), ('maesTro', 'leVel')]
    ordered = []
    for entry in sort_order:
        for title, first_name in SECTIONS:
            if entry == first_name: ordered.append('__SEP__:%s:255,140,0,255' % title)
        ordered.append(entry)
    (out / 'router_sort_order.json').write_text(json.dumps({'RouterSortOrder': ordered}, indent=4))
    (out / 'comments.json').write_text(json.dumps({'NumComments': 0}, indent=4))
    (out / 'midi.json').write_text('{}')
    (out / 'scope_config.json').write_text(json.dumps({'parameters': []}, indent=4))

    # ---- custom GUI ------------------------------------------------------------
    def ref(path):
        node, param = path.split('/')
        return {'nodeDisplayName': node.replace('_', ' '), 'parameterDisplayName': param.replace('_', ' '), 'parameterPath': path}
    def label_cfg(scale=1.0, color=BLACK):
        return {'labelColor': color, 'labelFontScale': scale, 'valueFontScale': scale}
    bg, fg = [], []
    def static(wtype, label, x, y, w, h, color, config=None):
        (bg if wtype == 'BackgroundPanel' else fg).append({
            'color': color, 'config': config or {}, 'gridX': x, 'gridY': y, 'label': label,
            'parameterRef': {'nodeDisplayName': '', 'parameterDisplayName': '', 'parameterPath': ''},
            'spanH': h, 'spanW': w, 'type': wtype})
    def widget(wtype, label, path, x, y, w, h, color, config):
        fg.append({'color': color, 'config': config, 'gridX': x, 'gridY': y, 'label': label,
                   'parameterRef': ref(path), 'spanH': h, 'spanW': w, 'type': wtype})

    # Layout: dark cards on a 10 px grid, one knob size everywhere (4 x 5 cells,
    # 5 cells apart), the sound's path from top to bottom: display; exciter,
    # filter, resonator, string; equalizer, reverb; (de)tuner, master; piano; presets.
    BG, CARD, TEXT, DIM, ACCENT, KNOBC = [24, 24, 28, 255], [38, 38, 44, 255], [215, 215, 220, 255], [140, 140, 150, 255], [255, 140, 40, 255], [62, 62, 70, 255]
    COLS = 64
    title_cfg = {'showLabel': True, 'labelColor': ACCENT, 'labelFontScale': 0.75}
    knob_cfg = {'labelColor': TEXT, 'labelFontScale': 0.7, 'valueFontScale': 0.6, 'bodyColor': KNOBC,
                'interactive': True, 'showValue': True, 'useCustomRange': False}
    LABELS = {'atT': 'attack', 'reL': 'release', 'veL': 'velocity', 'spreAd': 'spread', 'cuT': 'cutoff', 'reS': 'reso',
              'suB': 'sub', 'baSs': 'bass', 'miD': 'mid', 'frEq': 'freq', 'treB': 'treble', 'suP': 'air',
              'r1 daMp': 'damp', 'r1 undaMp': 'undamp', 'r1 cuT': 'cutoff', 'r1 reS': 'reso', 'r1 aMt': 'env amt',
              'r1 deC': 'decay', 'r1 reL': 'release', 'r2 hiCut': 'hicut', 'r2 tuNe': 'tune', 'r2 daMp': 'damp',
              'timBre': 'timbre', 'scaLe': 'scale', 'traNs': 'transpose', 'delAy': 'delay', 'tiMe': 'diffuse',
              'reV': 'decay', 'lPf': 'tone', 'dryWet': 'mix', 'leVel': 'level'}
    by_name = {c[0]: c for c in CONTROLS}
    def path_of(name): return ctl_router[by_name[name][1]] + '/Value'
    def card(title, x, y, w, h=8):
        static('BackgroundPanel', title, x, y, w, h, CARD, title_cfg)
    def knobs(names, x, y):
        for i, n in enumerate(names):
            widget('Knob', LABELS[n], path_of(n), x + 1 + 5 * i, y + 2, 4, 5, ACCENT, dict(knob_cfg))
        return x + 1 + 5 * len(names)
    def toggle(name, x, y):
        widget('Toggle', 'on', path_of(name), x, y + 3, 3, 2, ACCENT,
               {'labelColor': TEXT, 'labelFontScale': 0.7, 'bodyColor': KNOBC, 'interactive': True, 'showValue': False})

    static('Label', "t'wanGer", 1, 0, 20, 2, ACCENT, label_cfg(0.9, ACCENT))
    widget('CustomRegion', '', 'Lissajous_1/XY', 1, 2, COLS - 2, 9, [255, 255, 255, 255], {})
    # row 1: exciter | filter | string (reSonator 2)
    y = 12
    card('exciter', 1, y, 21); knobs(['atT', 'reL', 'veL', 'spreAd'], 1, y)
    card('filter', 23, y, 17)
    widget('CustomDropdown', '', path_of('filTer'), 24, y + 3, 6, 2, ACCENT,
           {'customOptions': FILTERS, 'labelColor': TEXT, 'labelFontScale': 0.7, 'valueFontScale': 0.7,
            'bodyColor': KNOBC, 'interactive': True, 'showValue': True})
    knobs(['cuT', 'reS'], 29, y)
    card('string', 41, y, 22); toggle('r2 oN', 42, y); knobs(['r2 hiCut', 'r2 tuNe', 'r2 daMp'], 45, y)
    # row 2: resonator (reSonator 1) | (de)tuner
    y = 21
    card('resonator', 1, y, 37); knobs(['r1 daMp', 'r1 undaMp', 'r1 cuT', 'r1 reS', 'r1 aMt', 'r1 deC', 'r1 reL'], 1, y)
    card('detune', 39, y, 24); toggle('dt oN', 40, y); knobs(['timBre', 'scaLe', 'traNs'], 43, y)
    # row 3: equalizer | reverb
    y = 30
    card('eq', 1, y, 32); knobs(['suB', 'baSs', 'miD', 'frEq', 'treB', 'suP'], 1, y)
    card('reverb', 34, y, 29); toggle('rv oN', 35, y); knobs(['delAy', 'tiMe', 'reV', 'lPf', 'dryWet'], 37, y)
    # row 4: master | reverb diffusers (preset data the Reaktor panel hid)
    y = 39
    card('master', 1, y, 21)
    knobs(['leVel'], 1, y)
    widget('VUMeter', '', FX + '/Out', 7, y + 2, 3, 5, ACCENT, {'channels': 2, 'interactive': False, 'showValue': False, 'vertical': True})
    static('Randomizer', 'patch', 11, y + 3, 10, 3, ACCENT,
           {'labelColor': TEXT, 'labelFontScale': 0.75, 'bodyColor': KNOBC, 'amount': 1.0,
            'groups': rand_groups, 'exclude': rand_exclude})
    card('diffusers  (allpass ms · fine · polarity · wobble)', 23, y, 40)
    widget('MultiSlider', '', FX + '/Diffusers', 24, y + 3, 38, 4, [200, 120, 60, 255],
           {'bodyColor': KNOBC, 'interactive': True, 'showValue': False, 'useCustomRange': False, 'vertical': False})
    # piano and presets
    y = 48
    widget('PianoKeyboard', '', gate_router + '/Value', 1, y, COLS - 2, 5, ACCENT,
           {'play': True, 'pitchPath': pitch_router + '/Value', 'velocity': 0.8, 'loNote': 36, 'hiNote': 96,
            'bodyColor': ACCENT, 'interactive': True, 'showValue': False})
    y += 6
    rows = (len(presets) + 7) // 8
    static('SnapshotMatrix', '', 1, y, COLS - 2, 2 * rows, [255, 255, 255, 255],
           {'rows': rows, 'cols': 8, 'showSnapshotNames': True, 'valueFontScale': 0.65,
            'activeColor': ACCENT, 'filledColor': [52, 52, 60, 255], 'emptyColor': [34, 34, 38, 255]})
    total_rows = y + 2 * rows + 1
    panel = {'designMode': False, 'id': PANEL_ID, 'name': PANEL_NAME,
             'layout': {'backgroundColor': BG, 'cellHeight': 10.0, 'cellWidth': 10.0, 'columns': COLS,
                        'rows': total_rows, 'zoom': 1.0, 'widgets': bg + fg},
             'windowState': {'height': total_rows * 10.0 + 40, 'isOpen': False, 'posX': 40.0, 'posY': 40.0, 'width': COLS * 10.0 + 20}}
    (out / 'custom_guis.json').write_text(json.dumps({'panels': [panel]}, indent=4))

    # ---- snapshots: the 38 Reaktor presets ----------------------------------------
    snaps = []
    for slot, p in enumerate(presets):
        v = p['values']; pv = {}
        for (name, key, node, param, lo, hi, kind, _) in CONTROLS:
            path = ctl_router[key] + '/Value'
            if kind == 'f': pv[path] = {'type': 'f', 'value': num(min(max(v[key], min(lo, hi)), max(lo, hi)))}
            elif kind == 'i': pv[path] = {'type': 'i', 'value': float(int(v[key]))}
            else: pv[path] = {'type': 'b', 'value': bool(v[key])}
        pv[FX + '/Diffusers'] = {'type': VEC, 'value': diffusers(v)}
        snaps.append({'id': 'twanger_reaktor_%02d' % (slot + 1), 'name': p['name'], 'slot': slot, 'parameterValues': pv})
    bank = {'customGuiId': PANEL_ID, 'customGuiName': PANEL_NAME, 'currentSnapshotId': snaps[0]['id'],
            'morph': {'a': -1, 'b': -1, 'amount': 0.0, 'time': 4.0, 'beats': True, 'glide': False, 'curve': 1, 'switch': 1},
            'snapshots': snaps}
    (out / 'custom_gui_snapshots.json').write_text(json.dumps({'banks': [bank]}, indent=4))
    print('wrote', out, '-', len(files), 'nodes,', len(bg) + len(fg), 'widgets,', len(snaps), 'snapshots')

def check(out):
    """Every connection, widget and snapshot path must name a generated node and a real parameter."""
    import re
    root = pathlib.Path(__file__).resolve().parents[2] / 'synthdefs'
    def sc_params(src):
        names = re.findall(r'OceanodeParameter(?:Dropdown)?\.(?:kr|ar)\(\\(\w+)', src.read_text())
        return {n[0].upper() + n[1:] for n in names} | {'N Chan', 'Out', 'In'}
    params = {TW: sc_params(root / 'Source/PhysicalModelling/twanger.scd'),
              FX: sc_params(root / 'Effect/Time/Reverb/twangerFX.scd'),
              'Lissajous_1': {'In', 'XY'}}
    nodes = {f.stem for f in out.glob('*.json')} - {'modules', 'connections', 'router_sort_order', 'comments', 'midi',
                                                   'scope_config', 'custom_guis', 'custom_gui_snapshots'}
    for n in nodes:
        if n.startswith('Router_'): params[n] = {'Val'} if n.startswith('Router_v_') and not n.startswith('Router_v_f') else {'Value'}
    bad = []
    def ok(node, param):
        if node not in nodes or param not in params.get(node, set()): bad.append(node + '/' + param)
    for src, outs in json.loads((out / 'connections.json').read_text()).items():
        for op, dsts in outs.items():
            ok(src, op)
            for dst, ins in dsts.items():
                for ip in ins: ok(dst, ip)
    for w in json.loads((out / 'custom_guis.json').read_text())['panels'][0]['layout']['widgets']:
        path = w['parameterRef']['parameterPath']
        if path: ok(*path.split('/'))
    for snap in json.loads((out / 'custom_gui_snapshots.json').read_text())['banks'][0]['snapshots']:
        for path in snap['parameterValues']: ok(*path.split('/'))
    for w in json.loads((out / 'custom_guis.json').read_text())['panels'][0]['layout']['widgets']:
        if w['type'] == 'Randomizer':
            for path in list(w['config']['groups']) + w['config']['exclude']: ok(*path.split('/'))
    if bad: raise SystemExit('unresolved: ' + ', '.join(sorted(set(bad))))
    print('check: all connections, widgets, snapshots and randomizer targets resolve')

if __name__ == '__main__':
    main()
    check(pathlib.Path(sys.argv[1]))
