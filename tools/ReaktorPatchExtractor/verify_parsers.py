#!/usr/bin/env python3
"""Cross-check the Python and native Swift NRKT parsers on real files."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PYTHON_PARSER = REPO / "docs/steampipe_extraction/parse_nrkt.py"
NATIVE_PARSER = HERE / "dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor"

EXPECTED_COUNTS = {
    "SteamPipe.ism": {"modules": 947, "connections": 989, "snapshots": 65},
    "SteamPipe_santi_2.ens": {"modules": 950, "connections": 991, "snapshots": 67},
    "CRAX 2.ens": {"modules": 277, "connections": 349, "snapshots": 21},
    "comets.ens": {"modules": 830, "connections": 950, "snapshots": 40},
}


def load_python_parser():
    spec = importlib.util.spec_from_file_location("parse_nrkt", PYTHON_PARSER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot import {PYTHON_PARSER}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def native_report(path: Path) -> dict:
    completed = subprocess.run(
        [NATIVE_PARSER, path], check=True, stdout=subprocess.PIPE, text=True
    )
    return json.loads(completed.stdout)


def wire_signature(report: dict) -> set[tuple]:
    return {
        (
            wire["source_object_id"],
            wire.get("source_output_index"),
            wire.get("target_object_id"),
            wire["target_input_index"],
        )
        for wire in report["connections"]
    }


def verify_core_structure(structure: dict) -> None:
    assert structure["declared_module_count"] == structure["module_count"]
    assert structure["declared_connection_count"] == structure["connection_count"]
    for connection in structure["connections"]:
        for endpoint_name in ("source", "target"):
            endpoint = connection[endpoint_name]
            if endpoint["kind"] == "module_pin":
                assert "module_path" in endpoint, endpoint
    for module in structure["modules"]:
        assert not module["type_name"].startswith("core_type_"), module["type_id"]
        if "inner_structure" in module:
            verify_core_structure(module["inner_structure"])


def verify(path: Path, parser) -> None:
    python_report = parser.make_report(path)
    swift_report = native_report(path)
    keys = (
        "modules",
        "special_root_records",
        "input_ports",
        "output_ports",
        "connections",
        "resolved_connections",
        "snapshots",
        "snapshot_parameter_records",
        "core_cells",
        "decoded_core_cells",
        "core_structures",
        "core_modules",
        "core_connections",
    )
    for key in keys:
        assert python_report["counts"][key] == swift_report["counts"][key], key
    assert python_report["warnings"] == [], python_report["warnings"]
    assert swift_report["warnings"] == [], swift_report["warnings"]
    assert wire_signature(python_report) == wire_signature(swift_report)
    assert python_report["counts"]["connections"] == python_report["counts"]["resolved_connections"]
    assert [item["name"] for item in python_report["snapshots"]] == [
        item["name"] for item in swift_report["snapshots"]
    ]
    assert python_report["catalog"]["source_sha256"] == swift_report["catalog"]["source_sha256"]
    assert [item["sha256"] for item in python_report["core_cells"]] == [
        item["sha256"] for item in swift_report["core_cells"]
    ]
    assert [item["core_document"]["decoded_sha256"] for item in python_report["core_cells"]] == [
        item["core_document"]["decoded_sha256"] for item in swift_report["core_cells"]
    ]
    for python_cell, swift_cell in zip(python_report["core_cells"], swift_report["core_cells"]):
        assert python_cell["core_document"]["parse_status"] == "decoded"
        assert swift_cell["core_document"]["parse_status"] == "decoded"
        python_structure = python_cell["core_document"]["structure"]
        swift_structure = swift_cell["core_document"]["structure"]
        assert python_structure["module_count"] == swift_structure["module_count"]
        assert python_structure["connection_count"] == swift_structure["connection_count"]
        assert python_structure["properties"].get("name") == swift_structure["properties"].get("name")
        verify_core_structure(python_structure)
        verify_core_structure(swift_structure)
    for key, expected in EXPECTED_COUNTS.get(path.name, {}).items():
        assert python_report["counts"][key] == expected, (key, expected)
    print(f"OK {path.name}: {python_report['counts']['modules']} modules, "
          f"{python_report['counts']['connections']} connections")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    if not NATIVE_PARSER.exists():
        raise SystemExit(f"Build the app first: {HERE / 'build.sh'}")
    implementation = load_python_parser()
    for path in args.files:
        verify(path, implementation)


if __name__ == "__main__":
    main()
