#!/usr/bin/env python3
"""Build the playable SpaceSystem-Geiger preset and its reusable macros.

    make_preset.py <output bin/data directory>

Uses ScBus fan-in for sums; numeric macro pins remain editable without audio
modulation. The only Arithmetic nodes left implement the LFO power curves.
"""
import json
import pathlib
import shutil
import sys

DATA = pathlib.Path(sys.argv[1])

class Patch:
    def __init__(self):
        self.nodes, self.positions, self.counts, self.conns, self.macros = {}, {}, {}, {}, {}
        self.controls = {}
        self.scalar_controls = set()
        self.custom_gui = None

    def add(self, type_, params, x, y):
        k = self.counts.get(type_, 0) + 1
        self.counts[type_] = k
        key = type_.replace(' ', '_') + '_' + str(k)
        p = dict(params)
        p.setdefault('expanded', True)
        self.nodes[key] = (type_, p)
        self.positions.setdefault(type_, {})['%02d' % k] = [float(x), float(y)]
        return key

    def sc(self, name, n, params, x, y):
        return self.add('SC ' + name + '*', {'N_Chan': str(n), **params}, x, y)

    def bus(self, name, x, y):
        return self.add('Router ScBus', {'Exclude_From_Snapshots': '0', 'Name': name}, x, y)

    def control(self, name, value, lo, hi, x=-1200, y=None):
        if y is None: y = 90 * len(self.controls)
        key = self.add('Router v_f', {'Exclude_From_Snapshots': '0', 'Name': name,
            'Value': value if isinstance(value, list) else [float(value)], 'Min': [float(lo)], 'Max': [float(hi)]}, x, y)
        self.controls[name] = (key, value, lo, hi)
        return key

    def fire(self, name, x, y):
        return self.add('Router v', {'Exclude_From_Snapshots': '1', 'Name': name}, x, y)

    def choice(self, name, value, options):
        key = self.add('Router i', {'Exclude_From_Snapshots': '0', 'Name': name,
            'Value': str(value), 'Min': '0', 'Max': str(len(options)-1)}, -1200, 90 * len(self.controls))
        self.controls[name] = (key, value, 0, len(options)-1)
        self.scalar_controls.add(name)
        return key

    def macro(self, inner, values, name, x, y):
        params = {'Active': '1', 'Button_Size': '28', 'Clear_Container_on_Load_Preset': '0',
            'Color': '0, 0, 0, 255', 'LocalPreset': True, 'Local_Name': name,
            'Reset_Ph_on_Active': '0', 'RetriggerSnapshotOnActive': False,
            'Retrigger_Snapshot_on_Active': '0', 'Show_Names': '1', 'Snapshot': '-1',
            'Snapshot_Cols': '8', 'Snapshot_Rows': '2',
            'RouterSortOrder': [*inner.controls, 'Fire', 'In', 'OctTrig', 'Trig', 'Out']}
        params.update(values)
        params.update(inner.values())
        key = self.add('Macro', params, x, y)
        self.macros[key] = inner
        return key

    def connect(self, src, out, dst, param):
        self.conns.setdefault(src, {}).setdefault(out, {}).setdefault(dst, {})[param] = None

    def bind(self, control, dst, param):
        self.connect(control, 'Value', dst, param)

    def gain(self, n, sources, gain=1, x=0, y=0):
        key = self.sc('LinearGain', n, {'Gain': gain}, x, y)
        for src in sources: self.connect(src, 'Out', key, 'In')
        return key

    def dc(self, control, x, y):
        # Mapper's zero input gives Minoutput, including negative pitches/dB.
        key = self.sc('Mapper', 1, {'Mininput': 0, 'Maxinput': 1, 'Minoutput': 0, 'Maxoutput': 0}, x, y)
        self.bind(control, key, 'Minoutput')
        return key

    def values(self):
        return {k.replace(' ', '_'): str(int(v)) if k in self.scalar_controls else v if isinstance(v, list) else [float(v)]
                for k, (_, v, _, _) in self.controls.items()}

    def write(self, folder):
        folder = pathlib.Path(folder)
        if folder.exists(): shutil.rmtree(folder)
        folder.mkdir(parents=True)
        for key, (_, params) in self.nodes.items():
            (folder / (key + '.json')).write_text(json.dumps(params, indent=4) + '\n')
        for name, value in [('modules', self.positions), ('connections', self.conns),
                            ('comments', {'NumComments': 0}), ('midi', {})]:
            (folder / (name + '.json')).write_text(json.dumps(value, indent=4) + '\n')
        if self.custom_gui:
            (folder / 'custom_guis.json').write_text(json.dumps(self.custom_gui, indent=4) + '\n')
        for key, inner in self.macros.items(): inner.write(folder / key)


DIVISIONS = ['1/64', '1/32', '1/16 T', '1/16', '1/8 T', '1/8', '1/4 T',
             '1/8 dotted', '1/4', '1/2 T', '1/4 dotted', '1/2', '1 bar', '2 bars', '4 bars']

def source(kind, range_ms, chance, cutrange, resrange, resmin, level, decay_range=0, compress=False):
    m = Patch()
    defaults = [('On', 1, 0, 1), ('Level', level, 0, 1), ('Chance', chance, 0, 1),
        ('Range ms', range_ms, 1 if kind == 'click' else 0, 2000), ('Min ms', 1, 1, 500),
        ('Curve', 1, 0, 40), ('Cut Range', cutrange, 0, 120), ('Cut Min', 0, 0, 120),
        ('Res Range', resrange, 0, 1), ('Res Min', resmin, 0, .98), ('Gate', 1, 0, 1)]
    if kind != 'click': defaults += [('Decay Range ms', decay_range, 0, 1000), ('Decay Min ms', 1, .1, 100)]
    if kind == 'saw': defaults += [('Attack ms', 1.12, .1, 1000), ('Pitch', 17, 0, 127),
                                  ('Fine', .3, -1, 1), ('Octaves', 8, 0, 8)]
    for name, val, lo, hi in defaults: m.control(name, val, lo, hi)
    m.control('Quantize to Clock', 0, 0, 1)
    m.choice('Clock Division', {'click': 3, 'noise': 6, 'saw': 5}[kind], DIVISIONS)
    for name, val, lo, hi in [('Beat Transport', 0, 0, 1e12), ('BPM', 120, 0, 999),
                              ('Playing', 0, 0, 1), ('Bar Beats', 4, 1, 16)]:
        m.control(name, val, lo, hi)
    out = m.bus('Out', 2200, 0)
    trig = m.bus('Trig', 2200, 250)
    metro = m.sc('RandomMetro', 1, {}, -900, -250)
    pulse = m.sc('TransportPulse', 1, {}, -900, -700)
    for name, param in [('Beat Transport', 'Beattransport'), ('BPM', 'Bpm'),
                        ('Playing', 'Playing'), ('Clock Division', 'Division'), ('Bar Beats', 'Barbeats')]:
        m.bind(m.controls[name][0], pulse, param)
    m.bind(m.controls['Quantize to Clock'][0], metro, 'Quantize')
    m.connect(pulse, 'Out', metro, 'Clock')
    for label, param in [('Range ms', 'Range'), ('Min ms', 'Minms'), ('Curve', 'Curve')]:
        m.bind(m.controls[label][0], metro, param)
    fire = m.fire('Fire', -1200, -200)
    m.connect(fire, 'Val', metro, 'Fire')
    m.connect(metro, 'Out', trig, 'Value')
    gate = m.sc('Chance', 1, {}, -600, -250)
    m.connect(metro, 'Out', gate, 'In')
    m.bind(m.controls['Chance'][0], gate, 'Chance')
    row = 0

    def rnd(lo=0., hi=1., quant=0):
        nonlocal row
        key = m.sc('RandomValues', 1, {'Min': lo, 'Max': hi, 'Bias': .5, 'Iquantize': quant}, -600, row)
        row += 240
        m.connect(metro, 'Out', key, 'Trigger')
        return key

    def ranged(span_label, min_label):
        nonlocal row
        r = rnd()
        scaled = m.gain(1, [r], x=-300, y=row - 240)
        m.bind(m.controls[span_label][0], scaled, 'Gain')
        offset = m.dc(m.controls[min_label][0], -300, row - 100)
        return m.gain(1, [scaled, offset], x=0, y=row - 240)

    amp = rnd()
    if kind == 'click':
        exc = m.gain(1, [gate], x=-300, y=-250)
        m.connect(amp, 'Out', exc, 'Gain')
    else:
        dec = ranged('Decay Range ms', 'Decay Min ms')
        env = m.sc('Envelope', 1, {'Type': 2, 'Att': 1.12 if kind == 'saw' else 0,
            'Curve': -6.9, 'Level': 1 if kind == 'saw' else .5}, 300, -250)
        m.connect(gate, 'Out', env, 'Gate')
        m.connect(amp, 'Out', env, 'Peak')
        m.connect(dec, 'Out', env, 'Rel')
        if kind == 'noise':
            exc = m.sc('Noise', 1, {'Type': 4}, 600, -250)
            m.connect(env, 'Out', exc, 'Levels')
        else:
            m.bind(m.controls['Attack ms'][0], env, 'Att')
            octave = rnd(0., 8., 1)
            m.bind(m.controls['Octaves'][0], octave, 'Max')
            oct_pitch = m.sc('Mapper', 1, {'Mininput': 0, 'Maxinput': 8, 'Minoutput': 0, 'Maxoutput': 96}, 0, -500)
            m.connect(octave, 'Out', oct_pitch, 'In')
            base = m.dc(m.controls['Pitch'][0], 0, -650)
            fine = m.dc(m.controls['Fine'][0], 0, -800)
            pitch = m.gain(1, [oct_pitch, base, fine], x=300, y=-500)
            exc = m.sc('Analog', 1, {'Type': 0, 'Inputmode': 1, 'Levels': 1}, 600, -250)
            m.connect(env, 'Out', exc, 'In')
            m.connect(pitch, 'Out', exc, 'Pitch')
    cut, res, mode = ranged('Cut Range', 'Cut Min'), ranged('Res Range', 'Res Min'), rnd(0., 5., 1)
    filt = m.sc('ReaktorFilter4', 1, {'Drywet': 1}, 900, -250)
    m.connect(exc, 'Out', filt, 'In')
    for src, param in [(cut, 'Pitch'), (res, 'Res'), (mode, 'Type')]: m.connect(src, 'Out', filt, param)
    gated = m.gain(1, [filt], x=1150, y=-250)
    m.bind(m.controls['Gate'][0], gated, 'Gain')
    sat = m.sc('ReaktorSaturator', 1, {}, 1400, -250)
    m.connect(gated, 'Out', sat, 'In')
    pan = m.sc('Panner', 1, {'NumSpeakers': '4', 'Width': 2, 'Level': level}, 1650, -250)
    m.connect(sat, 'Out', pan, 'In')
    m.connect(rnd(), 'Out', pan, 'Position')
    m.bind(m.controls['Level'][0], pan, 'Level')
    last = pan
    if compress:
        last = m.sc('CompressorPro', 4, {'Threshold': -44.5, 'Ratio': 2, 'Knee': 15.5,
            'Attack': .79, 'Release': 100, 'Makeup': -.4, 'Link': 1, 'Mix': 1}, 1850, -250)
        m.connect(pan, 'Out', last, 'In')
    enabled = m.gain(4, [last], x=2050, y=-250)
    m.bind(m.controls['On'][0], enabled, 'Gain')
    m.connect(enabled, 'Out', out, 'Value')
    return m


NOTES = [33, 48, 52, 53, 57, 71]

def resonator(detune, feedback, color, octrange, space, drywet, level):
    m = Patch()
    r_in = m.bus('In', -1400, -200)
    r_oct = m.bus('OctTrig', -1400, -400)
    defaults = [('On', 1, 0, 1), ('Level', level, -50, 10), ('DryWet', drywet, 0, 1),
        ('Transpose', .3, -48, 48), ('Detune', detune, 0, 1), ('Feedback', feedback, -1, 1),
        ('Col', color, 72, 127), ('LowPass', 87, 72, 127), ('HighPass', 16, 16, 127), ('OctRange', octrange, 0, 4)]
    defaults += [('Notes', NOTES, 0, 127)]
    defaults += [(name, val, lo, hi) for name, val, lo, hi in zip(
        ['Size', 'Mod', 'Lo', 'Hi', 'Position', 'Mix'], space, [0, 0, -12, -12, 0, 0], [100, 1, 12, 12, 1, 1])]
    for name, val, lo, hi in defaults: m.control(name, val, lo, hi)
    out = m.bus('Out', 1700, -200)
    octave = m.sc('RandomValues', 1, {'Min': 0, 'Iquantize': 1, 'Bias': .5}, -1100, -500)
    m.connect(r_oct, 'Value', octave, 'Trigger')
    m.bind(m.controls['OctRange'][0], octave, 'Max')
    oct_pitch = m.sc('Mapper', 1, {'Mininput': 0, 'Maxinput': 4, 'Minoutput': 0, 'Maxoutput': 48}, -850, -500)
    m.connect(octave, 'Out', oct_pitch, 'In')
    transpose = m.dc(m.controls['Transpose'][0], -850, -700)
    transposed = m.gain(1, [oct_pitch, transpose], x=-600, y=-500)
    lp = m.sc('OnePole', 4, {'Type': 0, 'Drywet': 1}, -100, -200)
    for i, spread in enumerate([-3, -2, -1, 1, 2, 3]):
        # The piano selects a chord. Each voice takes one note; absent notes
        # stop exciting that voice and let its existing resonance decay.
        note = m.add('Vector Getter', {'Input': NOTES, 'Index': [i]}, -1000, i * 360)
        m.bind(m.controls['Notes'][0], note, 'Input')
        active = m.add('Vector Operations', {'Op.': 6}, -800, 150 + i * 360)
        m.connect(note, 'Output', active, 'Input')
        comb = m.sc('ColorComb', 4, {'Pitch': NOTES[i], 'Spread': spread * .33, 'Res': 0, 'Send': 1, 'Mix': 1}, -600, -100 + i * 360)
        m.connect(r_in, 'Value', comb, 'In')
        m.connect(note, 'Output', comb, 'Pitch')
        m.connect(active, 'Output', comb, 'Send')
        for name, param in [('Detune', 'Detune'), ('Feedback', 'Feedback'), ('Col', 'Color')]:
            m.bind(m.controls[name][0], comb, param)
        m.connect(transposed, 'Out', comb, 'Transpose')
        # The six quad voices sum straight into the low-pass inlet.
        m.connect(comb, 'Out', lp, 'In')
    m.bind(m.controls['LowPass'][0], lp, 'Pitch')
    hp = m.sc('OnePole', 4, {'Type': 1, 'Drywet': 1}, 150, -200)
    m.connect(lp, 'Out', hp, 'In')
    m.bind(m.controls['HighPass'][0], hp, 'Pitch')
    xf = m.sc('CrossfadeAB', 4, {'Linear': 1, 'Levels': 1}, 400, -200)
    m.connect(r_in, 'Value', xf, 'Ina')
    m.connect(hp, 'Out', xf, 'Inb')
    m.bind(m.controls['DryWet'][0], xf, 'Xfade')
    db = m.sc('Conversions', 4, {'Conversion': 7}, 400, 200)
    m.bind(m.controls['Level'][0], db, 'Input')
    gain = m.gain(4, [xf], x=650, y=-200)
    m.connect(db, 'Out', gain, 'Gain')
    ms = m.sc('MicroSpace', 4, {}, 900, -200)
    m.connect(gain, 'Out', ms, 'In')
    for name in ['Size', 'Mod', 'Lo', 'Hi', 'Position', 'Mix']: m.bind(m.controls[name][0], ms, name)
    enabled = m.gain(4, [ms], x=1250, y=-200)
    m.bind(m.controls['On'][0], enabled, 'Gain')
    m.connect(enabled, 'Out', out, 'Value')
    return m


top = Patch()
clix = source('click', 1496, .9, 120, .5, .4, 1, compress=True)
# Sustained excitation builds much more resonator/reverb energy than a click.
# Equal-rate isolation measured noise/saw 12–14 dB above click after the banks.
noise = source('noise', 2000, .2, 115.28, .41, .4, .0625, 480.32)
saw = source('saw', 2000, .2, 115.28, .41, .27, .0625, 62.99)
a = resonator(.2, .97, 121, 0, (71.6535, .244094, .0944881, -5.38583, .755906, .307087), 1, -10)
b = resonator(.19, .99, 111, 1, (71.6535, 0, 9.73228, .0944881, .755906, .984252), .71, -8)
sources = [top.macro(m, m.values(), label, -1300, y) for m, label, y in
           [(clix, 'Geiger Click', -500), (noise, 'Geiger Noise', 0), (saw, 'Geiger Saw', 500)]]
transport = top.add('Beat Transport', {}, -1750, -600)
bar_beats = top.control('Bar Beats', 4, 1, 16, -1750, 200)
for src in sources:
    for port in ['Beat Transport', 'BPM', 'Playing']:
        top.connect(transport, port.replace(' ', '_'), src, port.replace(' ', '_'))
    top.connect(bar_beats, 'Value', src, 'Bar_Beats')
resonators = [top.macro(m, m.values(), label, -300, y) for m, label, y in [(a, 'Resonator A', -300), (b, 'Resonator B', 600)]]
for dest in resonators:
    for src in sources:
        top.connect(src, 'Out', dest, 'In')
        top.connect(src, 'Trig', dest, 'OctTrig')


def lfo(freq, power, y):
    l = top.sc('LFO', 1, {'Shape': 4, 'Freq': freq, 'Min': -1, 'Max': 1}, -1300, y)
    ab = top.sc('UnaryOps', 1, {'Operation': 0}, -1000, y)
    p = top.sc('Arithmetic', 1, {'Operation': 7}, -700, y)
    power_sig = top.sc('Sig', 1, {'Value': power}, -1000, y + 300)
    top.connect(l, 'Out', ab, 'In'); top.connect(ab, 'Out', p, 'In1'); top.connect(power_sig, 'Out', p, 'In2')
    return l, power_sig, p


def scaled(src, lo, hi, x, y):
    k = top.sc('Mapper', 1, {'Mininput': 0, 'Maxinput': 1, 'Minoutput': lo, 'Maxoutput': hi}, x, y)
    top.connect(src, 'Out', k, 'In')
    dup = top.sc('ChannelDuplicator', 4, {'Idx': [0, 0, 0, 0]}, x + 300, y)
    top.connect(k, 'Out', dup, 'In')
    return dup


lfo_b, power_b, l1 = lfo(.349, 1.4, 1600)
lfo_a, power_a, l2 = lfo(.285, .2, 2400)
for src, lo, hi, dest, param, y in [
    (l1, .566929, 1, resonators[1], 'DryWet', 1600),
    (l1, -50+60*.566929, 10, resonators[1], 'Level', 2000),
    (l2, -50+60*.496063, -50+60*.850394, resonators[0], 'Level', 2400)]:
    top.connect(scaled(src, lo, hi, -300, y), 'Out', dest, param)

# Original output fold: (ch0+ch1)/2, (ch2+ch3)/2, ch2, ch3.
folds = []
for idx, y in [([0, 2, -1, -1], 0), ([1, 3, -1, -1], 450), ([-1, -1, 2, 3], 900)]:
    dup = top.sc('ChannelDuplicator', 4, {'Idx': idx}, 600, y)
    for src in resonators: top.connect(src, 'Out', dup, 'In')
    folds.append(dup)
fold_gain = top.gain(4, folds[:2], [.5, .5, 0, 0], x=1000, y=0)
master = top.gain(4, [fold_gain, folds[2]], x=1300, y=0)
master_control = top.control('Master', 1, 0, 2, 1000, 900)
top.bind(master_control, master, 'Gain')
output = top.add('SC Output', {'Chan': '0', 'Server': '0'}, 1600, 0)
top.connect(master, 'Out', output, 'In')

# Build a native Oceanode Custom GUI; controls bind to the exposed macro pins.
widgets = []
COLORS = [[241, 174, 91, 255], [82, 200, 186, 255], [135, 160, 245, 255]]
def widget(kind, path, label, x, y, w, h, color, **config):
    widgets.append({'type': kind, 'parameterRef': {'parameterPath': path,
        'nodeDisplayName': path.split('/')[0].replace('_', ' ') if path else '',
        'parameterDisplayName': label}, 'label': label, 'gridX': x, 'gridY': y,
        'spanW': w, 'spanH': h, 'color': color,
        'config': {'interactive': True, 'showValue': True, 'labelColor': [232, 235, 242, 255],
                   'bodyColor': [32, 38, 48, 255], **config}})
def param(node, name): return node + '/' + name.replace(' ', '_')
widget('Label', '', 'SPACESYSTEM / GEIGER', 1, 0, 32, 2, [242,244,249,255], valueFontScale=1.5)
widget('Label', '', 'stochastic sources  /  six-note resonators  /  quad space', 34, 0, 25, 2, [160,171,190,255])
for i, (key, name) in enumerate(zip(sources, ['CLICK', 'NOISE', 'SAW'])):
    x = i * 20
    widget('BackgroundPanel', '', name, x, 3, 20, 13, [24,30,39,255], showLabel=False)
    widget('Label', '', name, x+1, 3, 17, 1, COLORS[i])
    widget('MultiToggle', param(key,'On'), 'ON', x+1, 5, 3, 2, COLORS[i], rows=1, cols=1)
    for label, title, dx in [('Level','LEVEL',4),('Chance','CHANCE',9),('Range ms','INTERVAL ms',14)]:
        widget('Knob', param(key,label), title, x+dx, 5, 5, 4, COLORS[i])
    widget('Slider', param(key,'Cut Range'), 'FILTER', x+1, 10, 9, 2, COLORS[i], labelFontScale=.75)
    label = 'Curve' if i == 0 else 'Decay Range ms'
    widget('Slider', param(key,label), 'CURVE' if i == 0 else 'DECAY', x+11, 10, 8, 2, COLORS[i], labelFontScale=.75)
    widget('MultiToggle', param(key,'Quantize to Clock'), 'CLOCK', x+1, 13, 4, 2, COLORS[i], rows=1, cols=1, labelFontScale=.75)
    widget('CustomDropdown', param(key,'Clock Division'), 'RATE', x+6, 13, 13, 2, COLORS[i],
           customOptions=DIVISIONS, showLabel=False)
for i, key in enumerate(resonators):
    x = i * 30; color = COLORS[1+i]
    widget('BackgroundPanel', '', '', x, 14, 30, 14, [24,30,39,255], showLabel=False)
    widget('Label', '', 'RESONATOR ' + 'AB'[i] + ' / HARMONY', x+1, 14, 24, 1, color)
    widget('MultiToggle', param(key,'On'), 'ON', x+26, 14, 3, 2, color, rows=1, cols=1)
    widget('PianoKeyboard', param(key,'Notes'), 'HARMONY', x+1, 16, 28, 3, color,
           loNote=24, hiNote=84, showLabel=False, bodyColor=color)
    for j, (label,title) in enumerate([('Feedback','FEEDBACK'),('Col','COLOR'),('DryWet','DRY / WET'),
                                      ('Level','LEVEL dB'),('Transpose','TRANSPOSE'),('Detune','DETUNE')]):
        widget('Knob', param(key,label), title, x+j*5, 20, 5, 4, color)
    widget('Slider', param(key,'LowPass'), 'LP', x+1, 25, 9, 2, color, labelFontScale=.75)
    widget('Slider', param(key,'Mix'), 'VERB', x+11, 25, 9, 2, color, labelFontScale=.75)
    widget('Slider', param(key,'Size'), 'SIZE', x+21, 25, 8, 2, color, labelFontScale=.75)
# Make room for each source's clock controls.
for w in widgets:
    if w['gridY'] >= 14 and w['parameterRef']['parameterPath'].split('/')[0] not in sources:
        w['gridY'] += 3
for i, (node, name, label) in enumerate([(master_control,'Value','MASTER'),(lfo_a,'Freq','LFO A RATE'),
        (power_a,'Value','LFO A POWER'),(lfo_b,'Freq','LFO B RATE'),(power_b,'Value','LFO B POWER')]):
    widget('Knob', param(node,name), label, 1+i*5, 32, 5, 4, [223,191,121,255])
widget('Label', '', 'Click keys to select up to six notes per bank.\nDisconnect a Level / DryWet LFO cable for manual control.',
       28, 32, 30, 2, [163,176,195,255], labelFontScale=.8)
widget('DragNumber', param(bar_beats,'Value'), 'BEATS / BAR', 28, 35, 15, 2, [223,191,121,255], integerDisplay=True)
widget('Label', '', 'CLOCK uses transport Play / Stop + BPM', 44, 35, 15, 2, [163,176,195,255], labelFontScale=.7)
top.custom_gui = {'panels': [{'id': 'spacesystem-geiger-live', 'name': 'SpaceSystem Geiger — LIVE',
    'designMode': False, 'windowState': {'posX': 30, 'posY': 25, 'width': 1120, 'height': 785, 'isOpen': True},
    'layout': {'columns': 60, 'rows': 38, 'cellWidth': 18, 'cellHeight': 18, 'zoom': 1,
               'backgroundColor': [16,20,27,255], 'widgets': widgets}}]}

top.write(DATA / 'Presets/DEBUG/22--SPACESYSTEM_GEIGER')
for name, m in [('GEIGER_CLICK',clix),('GEIGER_NOISE',noise),('GEIGER_SAW',saw),('NI_RESONATOR_QUAD',b)]:
    m.write(DATA / 'Macros/AUDIO/REAKTOR' / name)
print('Built preset:', len(top.nodes), 'top-level nodes; only LFO power Arithmetic remains.')
