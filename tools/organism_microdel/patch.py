"""Oceanode macro/preset serialization, following the installed native format."""
import json, pathlib, shutil

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
        self.conns.setdefault(src, {}).setdefault(out.replace(' ', '_'), {}).setdefault(dst, {})[param.replace(' ', '_')] = None

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
