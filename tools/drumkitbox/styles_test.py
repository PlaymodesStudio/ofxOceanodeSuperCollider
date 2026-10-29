#!/usr/bin/env python3
"""DrumKitBox: every Model x Style of scDrumKitBoxData.cpp, rendered through
the real synths (DKTrig -> its layers -> DKVoice_2), one hit each. Checks that
each sounds (peak above -40 dBFS), stays below +3 dBFS, is finite and silent
before its hit. The templates and the controls' defaults are read from the
C++ source, so this follows any edit there.

    python3 tools/drumkitbox/styles_test.py
"""
import json, os, re, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "../../src/nodes/boxSynths/drumKitBox/scDrumKitBoxData.cpp")
DEFS = os.path.abspath(os.path.join(HERE, "../../synthdefs/Defaults/DrumKitBox/CompiledSynthdefs/DrumKitBox"))
SCLANG = os.environ.get("SCLANG", "/Applications/SuperCollider.app/Contents/MacOS/sclang")
SOUND = {"piece", "body", "bodyx", "click", "noise", "metal", "sample", "filter", "char", "trans", "out"}
LAYERS = [("Body On", "DKBody"), ("Click On", "DKClick"), ("Noise On", "DKNoise"), ("Metal On", "DKMetal")]
MODELS = ["Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Tom", "Rim", "Cowbell", "Cymbal", "Perc", "808 Sub", "FX", "Sample"]

def parse():
    src = open(SRC).read()
    specs = {}
    for m in re.finditer(r'\{"(\w+)", "([^"]+)", "([^"]*)", \'(\w)\', ([-\d.f]+),', src):
        section, label, sc, typ, d = m.groups()
        specs[label] = (section, sc, typ, float(d.rstrip("f")))
    t = src[src.index('R"JSON([', src.index("const ofJson& templates()")) + 7:]
    t = t[:t.index(')JSON"')]
    return specs, json.loads(t)

def read_float_wav(path):
    import struct
    data = open(path, "rb").read()
    pos, ch = 12, 2
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"fmt ": ch = struct.unpack("<H", data[pos + 10:pos + 12])[0]
        if cid == b"data": return np.frombuffer(data[pos + 8:pos + 8 + size], dtype="<f4").reshape(-1, ch)
        pos += 8 + size + (size & 1)

def main():
    specs, templates = parse()
    out = tempfile.mkdtemp(prefix="drumkitbox_styles_")
    cases = []
    for m, model in enumerate(templates):
        for s, (style, over) in enumerate(model["styles"]):
            v = {l: d for l, (sec, sc, typ, d) in specs.items() if sec in SOUND}
            v.update(model["base"]); v.update(over)
            if v.get("Sample On", 0): continue          # needs a file: not rendered here
            args = []
            for l, x in v.items():
                if l in specs and specs[l][1] and specs[l][0] in SOUND: args += [specs[l][1], float(x)]
            layers = [d for on, d in LAYERS if v.get(on, 0) > 0.5]
            cases.append((f"{MODELS[m]} / {style}", layers, args))
    lines = ['(', 'var defDir = "%s", outDir = "%s", cases;' % (DEFS, out), 'cases = [']
    for name, layers, args in cases:
        lines.append("  [%s, %s, [%s]]," % (json.dumps(name), "[" + ", ".join("\\" + l for l in layers) + "]",
                                           ", ".join(("\\" + a if isinstance(a, str) else repr(a)) for a in args)))
    lines += ['];', 'fork {', ' cases.do { |c, i|',
              '  var cond = Condition(), score = [[0.0, [\\d_loadDir, defDir]],',
              '    [0.01, [\\s_new, \\DKTrig, 1000, 0, 0, \\hitbus, 20, \\vis, -1, \\gatebus, 40, \\velbus, 300, \\rndbus, 301] ++ c[2]]];',
              '  c[1].do { |l, k| score = score.add([0.01, [\\s_new, l, 1001 + k, 1, 0, \\hitbus, 20, \\out, 30, \\modbus, 200] ++ c[2]]) };',
              '  score = score.add([0.01, [\\s_new, \\DKVoice_2, 1100, 1, 0, \\raw, 30, \\hitbus, 20, \\out, 0, \\modbus, 200, \\keybus, -1] ++ c[2]]);',
              '  score = score.add([0.2, [\\n_set, 1000, \\gate, 1, \\fire, 1]]);',
              '  score = score.add([0.25, [\\n_set, 1000, \\gate, 0]]);',
              '  score = score.add([3.5, [\\c_set, 0, 0]]);',
              '  Score(score).recordNRT(nil, outDir +/+ (i.asString ++ ".wav"), sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",',
              '    options: ServerOptions.new.numOutputBusChannels_(2).sampleRate_(48000), duration: 3.5, action: { cond.unhang });',
              '  cond.hang;', ' };', ' 0.exit;', '};', ')']
    scd = os.path.join(out, "styles.scd")
    open(scd, "w").write("\n".join(lines))
    subprocess.run([SCLANG, scd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1200)
    failures = 0
    for i, (name, layers, _) in enumerate(cases):
        path = os.path.join(out, f"{i}.wav")
        if not os.path.exists(path):
            print(f"FAIL {name}: not rendered"); failures += 1; continue
        a = read_float_wav(path)
        pre = float(np.abs(a[:int(0.18 * 48000)]).max())
        peak = float(np.abs(a).max())
        db = 20 * np.log10(max(peak, 1e-9))
        ok = np.isfinite(a).all() and pre < 1e-6 and -40 < db < 3
        print(f"{'ok  ' if ok else 'FAIL'} {name:28s} {db:+6.1f} dB  {'+'.join(l[2:] for l in layers)}")
        failures += 0 if ok else 1
    print(f"{len(cases)} sounds; " + ("PASS" if failures == 0 else f"{failures} FAILED"))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
