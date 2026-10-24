"""Use the same idempotent migration for imported banks and runtime recalls."""
import json, pathlib, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
def migrate(controls):
    build=pathlib.Path('/tmp/polycomb-controls-migration');build.mkdir(exist_ok=True)
    exe=build/'migrate'
    sources=[ROOT/'tools/polycomb/migrate_controls.cpp',ROOT/'src/nodes/boxSynths/polyComb/scPolyCombMigration.h',ROOT/'src/nodes/boxSynths/polyComb/scPolyCombPitchMap.h']
    if not exe.exists() or any(p.stat().st_mtime>exe.stat().st_mtime for p in sources):
        subprocess.run(['c++','-std=c++17','-O2','-I'+str(ROOT/'src'),'-I'+str(ROOT.parent.parent/'libs/json/include'),str(sources[0]),'-o',str(exe)],check=True)
    return json.loads(subprocess.check_output([str(exe)],input=json.dumps(controls).encode()))
