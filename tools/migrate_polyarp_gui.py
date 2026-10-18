#!/usr/bin/env python3
"""Replace the Polyphonic Arpeggiator GUI (ofxSantiNodes) with SC ArpeggiatorBox in Oceanode presets.

ArpeggiatorBox (2026-10-18) keeps the GUI node's inputs (Scale, NotePool, Root,
Reset, Trigger) and vector outputs (PitchOut, GateOut, VelOut, DurOut,
GateVelOut, Euc*Out) under the same names, and its settings under the GUI
node's own keys, so a preset is converted by renaming:
  modules.json        "Polyphonic Arpeggiator GUI" -> "SC ArpeggiatorBox"
  connections.json    "Polyphonic_Arpeggiator_GUI_<n>" -> "SC_ArpeggiatorBox_<n>"
  Polyphonic_Arpeggiator_GUI_<n>.json -> SC_ArpeggiatorBox_<n>.json (settings converted:
                      its editorState becomes the node's "ArpeggiatorBox" settings, the
                      clock toggles become Clock, old millisecond lengths become steps)
Outputs: Round robin over Voices = 2 x the largest chord (4..16).
Each affected folder is copied first to <data>/Presets_backup_ArpeggiatorBox/<its path>.
Running it twice is harmless.

  tools/migrate_polyarp_gui.py [data folder ...] [--dry-run]
"""
import json
import os
import re
import shutil
import sys

OLD_KEY, NEW_KEY = "Polyphonic Arpeggiator GUI", "SC ArpeggiatorBox"
OLD_FILE, NEW_FILE = "Polyphonic_Arpeggiator_GUI_", "SC_ArpeggiatorBox_"
BACKUP = "Presets_backup_ArpeggiatorBox"
HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "src", "nodes", "scArpeggiatorBox.h")
# GUI parameter names -> ArpeggiatorBox's (connections)
PARAM_RENAMES = {"ResetNext": "Reset Next", "Reset_Next": "Reset Next", "Snapshot Recall": "Snapshot", "Snapshot_Recall": "Snapshot"}


def control_keys():
    text = open(HEADER, encoding="utf-8").read()
    keys = set(re.findall(r'ctl[IFBV]\("([A-Za-z]+)"', text))
    return keys


def num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def convert_node(old, keys):
    state = old.get("editorState", {}) if isinstance(old.get("editorState"), dict) else {}
    s = {k: v for k, v in state.items() if k in keys}
    # node parameters of the GUI node
    if "BeatDiv" in old:
        s["beatDiv"] = num(old["BeatDiv"], 1.0)
    if "One_Shot" in old:
        s["oneShotMode"] = str(old["One_Shot"]) == "1"
    if "Clock_BPM" in old:
        s["internalClockBpm"] = num(old["Clock_BPM"], 120.0)
    bd = max(0.001, num(s.get("beatDiv", state.get("beatDiv", 1.0)), 1.0))
    step_ms = 60000.0 / (120.0 * bd)
    # the GUI node's legacy rules
    if "pulseMode" not in state:
        s["pulseMode"] = 1
    if "bassPatternMode" not in state and "bassOnAccent" in state:
        s["bassPatternMode"] = 3 if state.get("bassOnAccent") else 0
    if any(num(s.get(k, 0)) > 4 for k in ("durBase", "durRndm", "durEucStrength")):
        for k in ("durBase", "durRndm", "durEucStrength"):
            if k in s:
                s[k] = num(s[k]) / step_ms
    for k in ("strum", "strumRndm", "positionRndm"):
        if num(s.get(k, 0)) > 1:
            s[k] = min(1.0, num(s[k]) / step_ms)
    for k, v in list(s.items()):
        if isinstance(v, (int, float)) and not isinstance(v, bool) and v != v:
            del s[k]
    poly = int(num(s.get("polyphony", 1), 1)) + int(num(s.get("polyAccent", 0), 0))
    voices = max(4, min(16, 2 * poly))
    s["voiceMode"] = 1
    s["bassVoices"] = max(1, min(4, int(num(s.get("bassPolyphony", 1), 1))))
    transport = str(old.get("Transport_Clock", "0")) == "1" or bool(state.get("internalClockMode", False))
    free = str(old.get("Internal_Clock", "0")) == "1" or bool(state.get("freeRunClock", False))
    clock = (2 if free else 1) if transport else 0
    published = [k for k in old.get("publishedEditorParameters", []) or [] if k in keys]
    new = {
        "Show": str(old.get("Show", "0")),
        "Clock": str(clock),
        "Trig_In": "0",
        "NotePool": old.get("NotePool", [60.0, 64.0, 67.0]),
        "Scale": old.get("Scale", state.get("scale", [0, 2, 4, 5, 7, 9, 11])),
        "Root": str(old.get("Root", state.get("root", "0"))),
        "Seed": "1",
        "Voices": str(voices),
        "Snapshot": "0",
        "ArpeggiatorBox": s,
        "ArpeggiatorBoxPublished": published,
        "ArpeggiatorBoxTab": 0,
        "expanded": old.get("expanded", True),
    }
    return new


def rename_conn_node(name):
    return NEW_FILE + name[len(OLD_FILE):] if name.startswith(OLD_FILE) else name


def migrate_folder(folder, data, keys, dry):
    mpath = os.path.join(folder, "modules.json")
    modules = json.load(open(mpath, encoding="utf-8"))
    if OLD_KEY not in modules:
        return False
    print(("would migrate " if dry else "migrating ") + folder)
    if dry:
        return True
    backup = os.path.join(data, BACKUP, os.path.relpath(folder, data))
    if not os.path.exists(backup):
        shutil.copytree(folder, backup)
    ids = modules.pop(OLD_KEY)
    target = modules.setdefault(NEW_KEY, {})
    renumber = {}
    for i, pos in ids.items():
        new_id = i
        while new_id in target:
            new_id = "%02d" % (int(new_id) + 50)
        renumber[str(int(i))] = str(int(new_id))
        target[new_id] = pos
    json.dump(modules, open(mpath, "w", encoding="utf-8"), indent=4)

    def node_name(n):
        if not n.startswith(OLD_FILE):
            return n
        return NEW_FILE + renumber.get(n[len(OLD_FILE):], n[len(OLD_FILE):])

    cpath = os.path.join(folder, "connections.json")
    if os.path.exists(cpath):
        conns = json.load(open(cpath, encoding="utf-8"))
        out = {}
        for src, params in conns.items():
            nsrc = node_name(src)
            for sp, dsts in params.items():
                nsp = PARAM_RENAMES.get(sp, sp) if src.startswith(OLD_FILE) else sp
                for dst, dps in dsts.items():
                    ndst = node_name(dst)
                    for dp, v in dps.items():
                        ndp = PARAM_RENAMES.get(dp, dp) if dst.startswith(OLD_FILE) else dp
                        out.setdefault(nsrc, {}).setdefault(nsp, {}).setdefault(ndst, {})[ndp] = v
        json.dump(out, open(cpath, "w", encoding="utf-8"), indent=4)
    for f in os.listdir(folder):
        if f.startswith(OLD_FILE) and f.endswith(".json"):
            old = json.load(open(os.path.join(folder, f), encoding="utf-8"))
            new_name = node_name(f[:-5]) + ".json"
            json.dump(convert_node(old, keys), open(os.path.join(folder, new_name), "w", encoding="utf-8"), indent=4)
            os.remove(os.path.join(folder, f))
    return True


def migrate(data, keys, dry):
    presets = os.path.join(data, "Presets")
    count = 0
    for root, dirs, files in os.walk(presets):
        if BACKUP in root or "modules.json" not in files:
            continue
        if OLD_KEY not in open(os.path.join(root, "modules.json"), encoding="utf-8").read():
            continue
        if migrate_folder(root, data, keys, dry):
            count += 1
    return count


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv
    keys = control_keys()
    if not args:
        apps = os.path.normpath(os.path.join(HERE, "..", "..", "..", "apps", "Playmodes"))
        args = [os.path.join(apps, a, "bin", "data") for a in ("OceanAudio", "OceanodeScroller")]
    total = sum(migrate(d, keys, dry) for d in args)
    print(f"{total} preset folder(s) {'to migrate' if dry else 'migrated'}")


if __name__ == "__main__":
    main()
