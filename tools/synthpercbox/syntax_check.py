#!/usr/bin/env python3
"""-fsyntax-only one addon .cpp with an app's clang flags (from its newest Xcode build log).
  syntax_check.py FILE.cpp [App]   (App: BLOOBS default; the log must have compiled scAnimaBox.cpp)"""
import glob, gzip, os, re, shlex, subprocess, sys, pathlib
target = os.path.abspath(sys.argv[1]); app = sys.argv[2] if len(sys.argv) > 2 else 'BLOOBS'
logs = sorted(glob.glob(os.path.expanduser(f'~/Library/Developer/Xcode/DerivedData/{app}-*/Logs/Build/*.xcactivitylog')), key=os.path.getmtime, reverse=True)
line = None
for lg in logs:
    try: text = gzip.open(lg).read().decode('utf-8', 'replace')
    except Exception: continue
    for part in re.split(r'[\r\n]', text):
        if 'cc.sh -x c++' in part and 'scAnimaBox.cpp' in part: line = part; break
    if line: break
if not line: sys.exit('no compile line for scAnimaBox.cpp in the logs')
args = shlex.split(line[line.index('cc.sh') + len('cc.sh'):])
out, skip = [], 0
two = {'-MT', '-MF', '--serialize-diagnostics', '-index-store-path', '-index-unit-output-path', '-o'}
for i, a in enumerate(args):
    if skip: skip -= 1; continue
    if a in two: skip = 1; continue
    if a in ('-c', '-MMD'): continue
    if a.endswith('scAnimaBox.cpp'): a = target
    out.append(a)
out.append("-fsyntax-only"); out += sys.argv[3:]
cwd = pathlib.Path(__file__).resolve().parents[4] / 'apps/Playmodes' / app
clang = '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
r = subprocess.run([clang] + out, cwd=cwd, capture_output=True, text=True)
msgs = [l for l in r.stderr.splitlines() if os.path.basename(target).split('.')[0] in l or 'synthPerc' in l.lower() or 'error' in l]
print('\n'.join(msgs[:80])); print('exit', r.returncode)
