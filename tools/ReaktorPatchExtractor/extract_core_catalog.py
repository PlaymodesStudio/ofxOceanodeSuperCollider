#!/usr/bin/env python3
"""Build a portable Reaktor Core module-ID catalog from C++ factory symbols."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


DEFAULT_EXECUTABLE = Path(
    "/Applications/Native Instruments/Reaktor 6/Reaktor 6.app/Contents/MacOS/Reaktor 6"
)
FACTORY_RE = re.compile(
    r"ImplModuleFactory<(.+?), \(NI::SD::eModuleType\)(\d+), (\d+), "
    r"\(NI::SDI::IModule::eFlavor\)(\d+),"
)


def display_name(implementation_class: str) -> str:
    short = implementation_class.rsplit("::", 1)[-1]
    short = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", short)
    short = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", short)
    replacements = {
        "Mult": "Multiply",
        "Sub": "Subtract",
        "Div": "Divide",
        "Const": "Constant",
        "R5 X": "R5X",
        "RO Table Ref": "Read-only Table Reference",
    }
    return replacements.get(short, short).replace("R5 X", "R5X")


def extract(executable: Path) -> dict:
    nm = subprocess.Popen(
        ["nm", "-arch", "arm64", "-m", str(executable)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    demangled = subprocess.run(
        ["c++filt"],
        stdin=nm.stdout,
        stdout=subprocess.PIPE,
        check=True,
        text=True,
    )
    if nm.stdout:
        nm.stdout.close()
    if nm.wait() != 0:
        raise RuntimeError("nm could not read the Reaktor executable")
    modules: dict[str, dict] = {}
    for line in demangled.stdout.splitlines():
        match = FACTORY_RE.search(line)
        if not match:
            continue
        implementation, type_id, resource_id, flavor = match.groups()
        modules.setdefault(
            type_id,
            {
                "type_id": int(type_id),
                "name": display_name(implementation),
                "implementation_class": implementation,
                "resource_id": int(resource_id),
                "flavor": int(flavor),
            },
        )
    source = executable.read_bytes()
    return {
        "schema": "reaktor-core-module-catalog-v1",
        "source_executable": str(executable),
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "module_count": len(modules),
        "modules": dict(sorted(modules.items(), key=lambda item: int(item[0]))),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, default=DEFAULT_EXECUTABLE)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).with_name("core_module_catalog.json"),
    )
    args = parser.parse_args()
    report = extract(args.executable)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"Wrote {args.output}: {report['module_count']} Core module types")


if __name__ == "__main__":
    main()
