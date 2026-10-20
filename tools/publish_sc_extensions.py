#!/usr/bin/env python3
"""Publish an app's third-party SuperCollider UGens for "Sync with SynthDefs Git".

    tools/publish_sc_extensions.py <app>/bin/data/Supercollider/Extensions [--publish]

Each plugin folder becomes one zip asset of the repository's "sc-extensions"
GitHub release, listed with its content hash in the release's extensions.json.
Oceanode's sync downloads a folder only when the hash of the local copy
differs (scSynthdefSync::folderHash computes the same hash).

Our own UGens (ugens/<Plugin>/ with a committed .scx) are skipped: the sync
takes those from the repository itself. So are Extensions/synthdefs and
Extensions/Oceanode, which hold no UGens.

Without --publish it only lists what would be uploaded. With it, it needs the
GitHub CLI (gh) logged in with write access to the repository, and uploads only
the folders whose hash changed since the last publish.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile

REPOSITORY = "PlaymodesStudio/ofxOceanodeSuperCollider"
RELEASE = "sc-extensions"
ADDON = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Not UGens: stray compiled SynthDefs, and the Oceanode classes (they come with
# the SynthDef sources)
EXCLUDED = {"synthdefs", "Oceanode"}


def folder_hash(folder):
    """sha256 of "<relative path>\\0<sha256 of the file>\\n" for every file, sorted by path."""
    entries = []
    for root, dirs, files in os.walk(folder):
        for name in files:
            if name == ".DS_Store" or name.startswith("._"):
                continue
            path = os.path.join(root, name)
            if not os.path.isfile(path):
                continue
            entries.append((os.path.relpath(path, folder).replace(os.sep, "/"), path))
    listing = b""
    for rel, path in sorted(entries, key=lambda e: e[0].encode()):
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        listing += rel.encode() + b"\0" + digest.encode() + b"\n"
    return hashlib.sha256(listing).hexdigest()


def own_plugins():
    ugens = os.path.join(ADDON, "ugens")
    return {name for name in os.listdir(ugens)
            if os.path.isdir(os.path.join(ugens, name))
            and any(f.endswith(".scx") for f in os.listdir(os.path.join(ugens, name)))}


def gh(*args, check=True):
    return subprocess.run(["gh", *args, "--repo", REPOSITORY], check=check, capture_output=True, text=True)


def published_manifest():
    with tempfile.TemporaryDirectory() as tmp:
        if gh("release", "view", RELEASE, check=False).returncode != 0:
            return None
        if gh("release", "download", RELEASE, "--pattern", "extensions.json", "--dir", tmp, check=False).returncode != 0:
            return {"folders": {}}
        with open(os.path.join(tmp, "extensions.json")) as f:
            return json.load(f)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    extensions = os.path.abspath(sys.argv[1])
    publish = "--publish" in sys.argv[2:]
    skip = own_plugins()

    folders = {}
    for name in sorted(os.listdir(extensions)):
        path = os.path.join(extensions, name)
        if os.path.isdir(path) and name not in skip and name not in EXCLUDED and not name.startswith("."):
            folders[name] = {"asset": name + ".zip", "hash": folder_hash(path)}

    previous = published_manifest() if publish else None
    old = (previous or {}).get("folders", {})
    changed = [name for name, entry in folders.items() if old.get(name, {}).get("hash") != entry["hash"]]
    size = lambda name: sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(os.path.join(extensions, name)) for f in fs)
    for name in folders:
        mark = "*" if name in changed else " "
        print(f"{mark} {name:32} {size(name) / 1e6:8.1f} MB  {folders[name]['hash'][:12]}")
    print(f"{len(folders)} folders, {len(changed)} to upload; skipped (from ugens/): {', '.join(sorted(skip))}")
    if not publish:
        print("Dry run: add --publish to upload.")
        return

    with tempfile.TemporaryDirectory() as tmp:
        zips = []
        for name in changed:
            out = os.path.join(tmp, name + ".zip")
            subprocess.run(["ditto", "-c", "-k", "--keepParent", os.path.join(extensions, name), out], check=True)
            zips.append(out)
        manifest = os.path.join(tmp, "extensions.json")
        with open(manifest, "w") as f:
            json.dump({"folders": folders}, f, indent=1, sort_keys=True)
        if previous is None:
            gh("release", "create", RELEASE, "--title", "SuperCollider UGens for Oceanode",
               "--notes", "Third-party UGen folders installed by Oceanode's Sync with SynthDefs Git. "
                          "Published with tools/publish_sc_extensions.py.")
        for path in zips + [manifest]:
            print("Uploading", os.path.basename(path))
            gh("release", "upload", RELEASE, path, "--clobber")
    print("Published.")


if __name__ == "__main__":
    main()
