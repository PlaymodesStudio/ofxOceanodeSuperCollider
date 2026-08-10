#!/usr/bin/env python3
"""Build a Reaktor Primary module catalog from the installed executable.

The Reaktor 6 macOS executable still contains names for its C++ symbols.  This
tool reads two internal dispatch tables without launching or modifying Reaktor:

* the quick-search module list (visible module name -> ``eModType``);
* ``mono::GetAudioFktTypeDependent`` (``eModType`` -> DSP function).

The generated JSON is deliberately versioned with the executable SHA-256.  It
can therefore be regenerated when Reaktor is updated instead of treating the
numeric module identifiers as timeless constants.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import tempfile
from pathlib import Path


DEFAULT_REAKTOR = Path(
    "/Applications/Native Instruments/Reaktor 6/Reaktor 6.app/Contents/MacOS/Reaktor 6"
)

# Short names are frequently materialised directly in ARM instructions, so
# objdump cannot annotate them as literal strings.  These stable Primary names
# fill those holes; every DSP mapping still comes independently from the binary.
KNOWN_NAMES = {
    4: "Macro", 5: "In Terminal", 6: "Out Terminal", 9: "Instrument",
    20: "Knob", 21: "Numeric Input", 22: "Fader", 23: "XY",
    24: "Button", 25: "XY Controller", 26: "Scope", 27: "Multi Picture",
    29: "Lamp", 30: "Meter", 31: "Level Meter", 32: "Picture",
    35: "Text", 36: "Switch", 37: "List", 100: "Constant",
    110: "Add", 111: "Multiply", 112: "Invert", 113: "Reciprocal",
    114: "Divide", 115: "Subtract", 116: "Multiply/Add",
    117: "Exponential (F)", 118: "Exponential (A)", 119: "Logarithm (A)",
    120: "Logarithm (F)", 121: "Modulo", 122: "Rectify",
    123: "Rectify/Sign", 124: "Compare", 125: "Compare/Equal",
    126: "Power", 127: "Square Root", 128: "Inverse Square Root",
    129: "Sine (math)", 130: "Sine/Cosine", 131: "ArcSin",
    132: "ArcCos", 133: "ArcTan", 134: "Quantize",
    159: "Merge", 208: "Audio to Event", 361: "Saturator 2",
    362: "Clipper", 363: "Level Crossing", 367: "Shaper 1 BP",
    401: "Counter", 404: "Event Value", 409: "Logic OR",
    412: "Separator", 414: "Merge", 415: "Value", 417: "Event Delay",
    419: "Event Order", 420: "Event Table Index", 446: "A to E",
    450: "To Voice", 454: "Voice Info", 457: "Sample Rate Info",
    459: "MIDI Note", 461: "Snap Isolate", 524: "Core Cell / R5X",
}

KNOWN_CLASSES = {
    4: "ModMacro", 5: "ModInTerminal", 6: "ModOutTerminal",
    9: "ModInstrument", 340: "ModDelayBase", 342: "ModDelayBase",
    458: "SystemInfo", 524: "ModR5XBase",
}


def run(*args: str, input_text: str | None = None) -> str:
    return subprocess.run(
        args, input=input_text, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True,
    ).stdout


def symbol_table(binary: Path) -> tuple[dict[str, int], dict[int, str]]:
    demangled = run("c++filt", input_text=run("nm", "-nm", str(binary)))
    by_name: dict[str, int] = {}
    by_address: dict[int, str] = {}
    pattern = re.compile(
        r"^([0-9a-f]+)\s+\([^)]*\)\s+"
        r"(?:non-external\s+(?:\(was a private external\)\s+)?)?(.+)$"
    )
    for line in demangled.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        name = match.group(2)
        by_name.setdefault(name, address)
        by_address.setdefault(address, name)
    return by_name, by_address


def next_symbol(address: int, by_address: dict[int, str]) -> int:
    return min(item for item in by_address if item > address)


@functools.lru_cache(maxsize=2)
def whole_disassembly(binary_name: str) -> str:
    return run("objdump", "--arch-name=arm64", "--macho", "--disassemble", binary_name)


def disassemble(binary: Path, start: int, stop: int) -> str:
    # Apple's Mach-O objdump currently accepts but does not consistently honour
    # --start/--stop-address.  Filter instruction addresses as a second guard;
    # otherwise regexes below could accidentally read an earlier jump table.
    output = whole_disassembly(str(binary))
    selected: list[str] = []
    for line in output.splitlines():
        match = re.match(r"^([0-9a-f]+):", line)
        if match and start <= int(match.group(1), 16) < stop:
            selected.append(line)
    return "\n".join(selected)


def vm_to_file_sections(binary: Path) -> list[tuple[int, int, int]]:
    """Return (VM start, VM end, file offset) for non-zerofill sections."""
    output = run("otool", "-l", str(binary))
    sections: list[tuple[int, int, int]] = []
    current: dict[str, int] | None = None
    for line in output.splitlines():
        stripped = line.strip()
        if stripped == "Section":
            if current and {"addr", "size", "offset"} <= current.keys():
                sections.append((current["addr"], current["addr"] + current["size"], current["offset"]))
            current = {}
        elif current is not None:
            match = re.match(r"(addr|size|offset)\s+(0x[0-9a-f]+|\d+)$", stripped)
            if match:
                current[match.group(1)] = int(match.group(2), 0)
    if current and {"addr", "size", "offset"} <= current.keys():
        sections.append((current["addr"], current["addr"] + current["size"], current["offset"]))
    return sections


def vm_to_file(address: int, sections: list[tuple[int, int, int]]) -> int:
    for start, end, offset in sections:
        if start <= address < end:
            return offset + address - start
    raise ValueError(f"VM address 0x{address:x} does not belong to a file-backed section")


def extract_quicksearch_names(binary: Path, symbols: dict[str, int], by_address: dict[int, str]) -> dict[int, str]:
    symbol = "reaktor::model::quicksearch::createModuleSearchItems(bool)"
    start = symbols[symbol]
    text = disassemble(binary, start, next_symbol(start, by_address))
    current_name: str | None = None
    current_kind: int | None = None
    result: dict[int, str] = {}
    for line in text.splitlines():
        literal = re.search(r'literal pool for: "(.*)"', line)
        if literal:
            current_name = literal.group(1)
        immediate = re.search(r"\bmov\s+w2,\s+#0x([0-9a-f]+)", line)
        if immediate:
            current_kind = int(immediate.group(1), 16)
        if "createModuleSearchItemsEbENK3$_9cl" in line and "\tbl\t" in line:
            if current_kind is not None and current_name:
                result.setdefault(current_kind, current_name)
            current_name = None
            current_kind = None
    return result


def extract_audio_functions(
    binary: Path, symbols: dict[str, int], by_address: dict[int, str], raw: bytes,
    sections: list[tuple[int, int, int]],
) -> dict[int, str]:
    symbol = "mono::GetAudioFktTypeDependent(eModType)"
    start = symbols[symbol]
    text = disassemble(binary, start, next_symbol(start, by_address))
    kind_base_match = re.search(r"\bsub\s+w8,\s+w0,\s+#0x([0-9a-f]+)", text)
    count_match = re.search(r"\bcmp\s+w8,\s+#0x([0-9a-f]+)", text)
    page_match = re.search(r"\badrp\s+x9,.*;\s+(0x[0-9a-f]+)", text)
    add_match = re.search(r"\badd\s+x9,\s+x9,\s+#0x([0-9a-f]+)", text)
    base_match = re.search(r"^([0-9a-f]+):.*\badr\s+x10,\s+#(-?\d+)", text, re.M)
    if not all((kind_base_match, count_match, page_match, add_match, base_match)):
        raise ValueError("GetAudioFktTypeDependent does not match the known ARM64 jump-table form")
    kind_base = int(kind_base_match.group(1), 16)
    count = int(count_match.group(1), 16) + 1
    table_vm = int(page_match.group(1), 16) + int(add_match.group(1), 16)
    handler_base = int(base_match.group(1), 16) + int(base_match.group(2))
    table_file = vm_to_file(table_vm, sections)

    handler_returns: dict[int, int] = {}
    for match in re.finditer(r"^([0-9a-f]+):.*\badr\s+x0,\s+#(-?\d+)", text, re.M):
        handler = int(match.group(1), 16)
        handler_returns[handler] = handler + int(match.group(2))

    result: dict[int, str] = {}
    for index in range(count):
        jump = struct.unpack_from("<H", raw, table_file + index * 2)[0]
        handler = handler_base + jump * 4
        function = handler_returns.get(handler)
        name = by_address.get(function or -1)
        if name and name.startswith("mono::"):
            result[kind_base + index] = name
    return result


def module_data_targets(
    binary: Path, symbols: dict[str, int], by_address: dict[int, str], raw: bytes,
    sections: list[tuple[int, int, int]],
) -> dict[int, int]:
    """Return runtime TModData address -> eModType from GetModData's table."""
    symbol = "TSModul::GetModData(eModType)"
    start = symbols[symbol]
    text = disassemble(binary, start, next_symbol(start, by_address))
    kind_match = re.search(r"\bsub\s+w8,\s+w0,\s+#0x([0-9a-f]+)", text)
    count_match = re.search(r"\bcmp\s+w8,\s+#0x([0-9a-f]+)", text)
    page_match = re.search(r"\badrp\s+x9,.*;\s+(0x[0-9a-f]+)", text)
    add_match = re.search(r"\badd\s+x9,\s+x9,\s+#0x([0-9a-f]+)", text)
    if not all((kind_match, count_match, page_match, add_match)):
        raise ValueError("TSModul::GetModData does not match the known table form")
    kind_base = int(kind_match.group(1), 16)
    count = int(count_match.group(1), 16) + 1
    table_vm = int(page_match.group(1), 16) + int(add_match.group(1), 16)
    table_file = vm_to_file(table_vm, sections)
    result: dict[int, int] = {}
    for index in range(count):
        pointer = struct.unpack_from("<Q", raw, table_file + index * 8)[0]
        if pointer:
            result[pointer] = kind_base + index
    return result


def read_cstring(raw: bytes, address: int, sections: list[tuple[int, int, int]]) -> str | None:
    try:
        offset = vm_to_file(address, sections)
    except ValueError:
        return None
    end = raw.find(b"\0", offset, min(len(raw), offset + 16_384))
    if end < 0:
        return None
    try:
        value = raw[offset:end].decode("utf-8")
    except UnicodeDecodeError:
        return None
    if not value or not all(character.isprintable() or character in "\r\n\t" for character in value):
        return None
    return value


def extract_documentation(
    binary: Path, symbols: dict[str, int], by_address: dict[int, str], raw: bytes,
    sections: list[tuple[int, int, int]],
) -> dict[int, dict]:
    """Recover per-eModType help text and contextual port tips from TModData."""
    targets = module_data_targets(binary, symbols, by_address, raw, sections)
    start = symbols["__GLOBAL__sub_I_moddat.cpp"]
    text = disassemble(binary, start, next_symbol(start, by_address))
    registers: dict[str, int] = {}
    blocks: list[dict] = []
    for line in text.splitlines():
        if "\tbl\t" in line:
            if blocks and "___cxa_atexit" in line:
                blocks[-1]["in_header"] = False
            # AArch64 x0...x18 are caller-saved. Retaining their inferred
            # addresses across a call creates convincing but false TModData
            # boundaries later in this very large initializer.
            for number in range(19):
                registers.pop(f"x{number}", None)
            continue
        page = re.search(r"\badrp\s+(x\d+),.*;\s+(0x[0-9a-f]+)", line)
        if page:
            registers[page.group(1)] = int(page.group(2), 16)
            continue
        direct = re.search(r"^([0-9a-f]+):.*\badr\s+(x\d+),\s+#(-?\d+)", line)
        if direct:
            address = int(direct.group(1), 16) + int(direct.group(3))
            registers[direct.group(2)] = address
            if address in targets:
                blocks.append({"kind": targets[address], "header": [], "following": [], "in_header": True})
            continue
        move = re.search(r"\bmov\s+(x\d+),\s+(x\d+)", line)
        if move and move.group(2) in registers:
            registers[move.group(1)] = registers[move.group(2)]
            continue
        add = re.search(r"\badd\s+(x\d+),\s+(x\d+),\s+#0x([0-9a-f]+)", line)
        if not add or add.group(2) not in registers:
            continue
        address = registers[add.group(2)] + int(add.group(3), 16)
        registers[add.group(1)] = address
        if address in targets:
            blocks.append({"kind": targets[address], "header": [], "following": [], "in_header": True})
            continue
        if not blocks or "literal pool for:" not in line:
            continue
        value = read_cstring(raw, address, sections)
        destination = blocks[-1]["header"] if blocks[-1]["in_header"] else blocks[-1]["following"]
        if value and value not in destination:
            destination.append(value)

    # Each TModData object is emitted after the inlet/outlet descriptor arrays
    # that it references.  Consequently a block's header strings describe that
    # block, while the descriptor strings following its __cxa_atexit belong to
    # the next TModData block.  Keeping this boundary prevents neighbouring
    # modules (for example Reciprocal, Divide and X-Fade) being conflated.
    strings_by_kind: dict[int, list[str]] = {}
    header_by_kind: dict[int, list[str]] = {}
    for index, block in enumerate(blocks):
        kind = block["kind"]
        inherited = blocks[index - 1]["following"] if index else []
        combined = block["header"] + inherited
        strings_by_kind[kind] = list(dict.fromkeys(combined))
        header_by_kind[kind] = block["header"]

    result: dict[int, dict] = {}
    input_prefixes = ("input", "control input", "audio input", "event input", "gate input")
    output_prefixes = ("output", "audio output", "event output", "gate output")
    for kind, strings in strings_by_kind.items():
        inputs: list[str] = []
        outputs: list[str] = []
        general: list[str] = []
        port_documentation: list[dict[str, str | None]] = []
        for index, value in enumerate(strings):
            lowered = value.strip().lower()
            if lowered.startswith(input_prefixes):
                inputs.append(value)
                label = strings[index - 1] if index and len(strings[index - 1]) <= 48 else None
                port_documentation.append({"direction": "input", "label": label, "help": value})
            elif lowered.startswith(output_prefixes):
                outputs.append(value)
                label = strings[index - 1] if index and len(strings[index - 1]) <= 48 else None
                port_documentation.append({"direction": "output", "label": label, "help": value})
            elif len(value) >= 24:
                general.append(value)
        item: dict = {
            "documentation_strings": strings,
            "documentation_source": "Reaktor TModData static initializer",
        }
        if inputs:
            item["input_help"] = inputs
        if outputs:
            item["output_help"] = outputs
        if port_documentation:
            item["port_documentation"] = port_documentation
        header_general = [value for value in header_by_kind[kind] if len(value) >= 24]
        if header_general:
            item["description"] = max(header_general, key=len)
        elif general:
            item["description"] = max(general, key=len)
        result[kind] = item
    return result


def build_catalog(executable: Path, runtime_catalog_path: Path | None = None) -> dict:
    original = executable.read_bytes()
    product_version: str | None = None
    info_plist = executable.parents[1] / "Info.plist"
    if info_plist.exists():
        with info_plist.open("rb") as stream:
            info = plistlib.load(stream)
        product_version = info.get("CFBundleShortVersionString")
    with tempfile.TemporaryDirectory(prefix="reaktor-catalog-") as temp:
        thin = Path(temp) / "Reaktor6-arm64"
        subprocess.run(["lipo", str(executable), "-thin", "arm64", "-output", str(thin)], check=True)
        raw = thin.read_bytes()
        symbols, by_address = symbol_table(thin)
        sections = vm_to_file_sections(thin)
        names = extract_quicksearch_names(thin, symbols, by_address)
        audio = extract_audio_functions(thin, symbols, by_address, raw, sections)
        documentation = extract_documentation(thin, symbols, by_address, raw, sections)

    runtime_modules: dict[str, dict] = {}
    if runtime_catalog_path:
        runtime_catalog = json.loads(runtime_catalog_path.read_text())
        if runtime_catalog.get("schema") != "reaktor-runtime-tmoddata-v1":
            raise ValueError(f"unsupported runtime catalog schema: {runtime_catalog.get('schema')}")
        runtime_modules = runtime_catalog.get("modules", {})
    runtime_descriptions = {
        int(kind) for kind, item in runtime_modules.items() if item.get("runtime_description")
    }
    runtime_names = {
        int(kind): item["runtime_name"]
        for kind, item in runtime_modules.items()
        if item.get("runtime_name")
    }
    available_names = set(names) | set(runtime_names) | set(KNOWN_NAMES)

    # GetModData accepts the complete inclusive range 4...555.  Keep every ID
    # in the exported dictionary: undocumented slots are valuable evidence too
    # (usually reserved, obsolete, or unavailable in this Reaktor build).
    kinds = list(range(4, 556))
    modules = {}
    for kind in kinds:
        item = {"kind": kind}
        if kind in names:
            item["name"] = names[kind]
            item["name_source"] = "Reaktor Quick Search"
        elif kind in runtime_names:
            item["name"] = runtime_names[kind]
            item["name_source"] = "initialized Reaktor TModData"
        elif kind in KNOWN_NAMES:
            item["name"] = KNOWN_NAMES[kind]
            item["name_source"] = "verified fallback"
        if kind in audio:
            item["mono_audio_function"] = audio[kind]
        if kind in KNOWN_CLASSES:
            item["implementation_class"] = KNOWN_CLASSES[kind]
        if kind in documentation:
            item.update(documentation[kind])
        runtime_item = runtime_modules.get(str(kind))
        if runtime_item:
            item.pop("documentation_strings", None)
            item.pop("documentation_source", None)
            item["tmoddata_address"] = runtime_item.get("tmoddata_address")
            item["runtime_name"] = runtime_item.get("runtime_name")
            if runtime_item.get("runtime_description"):
                item["description"] = runtime_item["runtime_description"]
                item["description_source"] = "initialized Reaktor TModData"
            runtime_inputs = runtime_item.get("runtime_inputs", [])
            runtime_outputs = runtime_item.get("runtime_outputs", [])
            item["inputs"] = runtime_inputs
            item["outputs"] = runtime_outputs
            exact_ports = [
                {"direction": "input", **port} for port in runtime_inputs
            ] + [
                {"direction": "output", **port} for port in runtime_outputs
            ]
            if exact_ports:
                item["port_documentation"] = exact_ports
                item["input_help"] = [
                    port["help"] for port in runtime_inputs if port.get("help")
                ]
                item["output_help"] = [
                    port["help"] for port in runtime_outputs if port.get("help")
                ]
                item["port_documentation_source"] = "initialized Reaktor TModData"
        item["documented"] = kind in documentation or kind in runtime_descriptions
        item["named"] = kind in available_names
        item["has_audio_function"] = kind in audio
        if kind not in available_names and kind not in documentation and kind not in runtime_descriptions and kind not in audio:
            item["status"] = "reserved_or_unavailable"
        modules[str(kind)] = item
    return {
        "schema": "reaktor-module-catalog-v2",
        "source_executable": str(executable.resolve()),
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "source_product_version": product_version,
        "architecture": "arm64",
        "module_count": len(modules),
        "named_module_count": len(available_names),
        "documented_module_count": len(set(documentation) | runtime_descriptions),
        "audio_function_count": len(audio),
        "initialized_tmoddata_count": len(runtime_modules),
        "documented_port_count": sum(
            bool(port.get("help"))
            for item in runtime_modules.values()
            for port in item.get("runtime_inputs", []) + item.get("runtime_outputs", [])
        ),
        "modules": modules,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reaktor", type=Path, default=DEFAULT_REAKTOR)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--runtime-catalog", type=Path,
        help="optional JSON dumped at Reaktor main with lldb_dump_catalog.py",
    )
    args = parser.parse_args()
    catalog = build_catalog(args.reaktor, args.runtime_catalog)
    encoded = json.dumps(catalog, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
