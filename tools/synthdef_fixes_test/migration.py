#!/usr/bin/env python3
"""Preset/macro migration regressions, including coexistence and collisions."""
import importlib.util, json, pathlib, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('synth_renames',ROOT/'tools/migrate_synth_renames.py');migration=importlib.util.module_from_spec(spec);spec.loader.exec_module(migration)
def write(path,data):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data,indent=2))
def snapshot(root):return {str(p.relative_to(root)):p.read_bytes() for p in root.rglob('*') if p.is_file()}
class MigrationTests(unittest.TestCase):
    def test_presets_and_macros_keep_controls_and_connections(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp)
            for tree,star in [('Presets','*'),('Macros','')]:
                folder=root/tree/'bank'
                write(folder/'modules.json',{'SC MonoBandPassBank32'+star:{'01':[10,20]},'SC MonoBandPassBank64'+star:{'02':[30,40]}})
                write(folder/('SC_MonoBandPassBank32'+star+'_01.json'),{'N_Chan':2,'pitcharray':list(range(64)),'mix':[.37]})
                write(folder/('SC_MonoBandPassBank64'+star+'_02.json'),{'N_Chan':2,'mix':[.8]})
                write(folder/'connections.json',{'source':'SC_MonoBandPassBank32'+star+'_01','target':'SC_MonoBandPassBank64'+star+'_02'})
            before=snapshot(root)
            self.assertEqual(migration.migrate(str(root),True),2);self.assertEqual(snapshot(root),before)
            self.assertEqual(migration.migrate(str(root),False),2)
            for tree,star in [('Presets','*'),('Macros','')]:
                folder=root/tree/'bank'
                values=json.loads((folder/('SC_MonoBandPassBank64'+star+'_01.json')).read_text())
                self.assertEqual(values,{'N_Chan':2,'pitcharray':list(range(64)),'mix':[.37]})
                modules=json.loads((folder/'modules.json').read_text())
                self.assertEqual(modules,{'SC MonoBandPassBank64'+star:{'01':[10,20],'02':[30,40]}})
                self.assertEqual(json.loads((folder/'connections.json').read_text()),{'source':'SC_MonoBandPassBank64'+star+'_01','target':'SC_MonoBandPassBank64'+star+'_02'})
            for path,content in before.items():self.assertEqual((root/migration.BACKUP/path).read_bytes(),content)
            self.assertEqual(migration.migrate(str(root),False),0)
    def test_filename_collision_refuses_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);folder=root/'Presets/p'
            write(folder/'SC_MonoBandPassBank32*_01.json',{'mix':.2});write(folder/'SC_MonoBandPassBank64*_01.json',{'mix':.8})
            before=snapshot(root)
            with self.assertRaisesRegex(ValueError,'collision'):migration.migrate(str(root),False)
            self.assertEqual(snapshot(root),before)
    def test_module_id_collision_refuses_before_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=pathlib.Path(tmp);folder=root/'Macros/p'
            write(folder/'modules.json',{'SC MonoBandPassBank32*':{'01':[10,20]},'SC MonoBandPassBank64*':{'01':[30,40]}})
            before=snapshot(root)
            with self.assertRaisesRegex(ValueError,'overlapping'):migration.migrate(str(root),False)
            self.assertEqual(snapshot(root),before)
    def test_previous_rename_still_works(self):
        self.assertEqual(migration.rewrite('"SC Pulsar2*" "SC_Pulsar2*_01" "SC RLPF4"'), '"SC Pulsar_B*" "SC_Pulsar_B*_01" "SC RLPF4Pole"')
if __name__=='__main__':unittest.main()
