"""LLDB helper: dump fully initialized Reaktor TModData strings.

Usage from an LLDB session stopped at main:

    command script import lldb_dump_catalog.py
    dump-reaktor-catalog /tmp/reaktor-runtime-catalog.json

This is read-only. Addresses are for Reaktor 6.5.0 arm64 and the resulting
catalog is validated separately against the executable SHA-256.
"""

from __future__ import annotations

import json
import struct

import lldb


TMODDATA_TABLE = 0x1031526D0
KIND_BASE = 4
KIND_COUNT = 552
TMODDATA_SIZE = 0x60


def _memory(process, address: int, size: int) -> bytes:
    error = lldb.SBError()
    value = process.ReadMemory(address, size, error)
    if not error.Success():
        raise RuntimeError(f"cannot read 0x{address:x}: {error}")
    return value


def _libcpp_string(process, raw: bytes) -> str | None:
    """Decode the 24-byte libc++ string layout used by this Reaktor build."""
    if len(raw) != 24:
        return None
    if raw[23] & 0x80:
        pointer, length = struct.unpack_from("<QQ", raw)
        if not pointer or length > 1_000_000:
            return None
        encoded = _memory(process, pointer, length)
    else:
        length = raw[23]
        if length > 23:
            return None
        encoded = raw[:length]
    try:
        return encoded.decode("utf-8")
    except UnicodeDecodeError:
        return encoded.decode("utf-8", errors="replace")


def _string_at(process, address: int) -> str | None:
    return _libcpp_string(process, _memory(process, address, 24))


def _ports(process, address: int, stride: int, help_offset: int) -> list[dict]:
    count = struct.unpack("<I", _memory(process, address, 4))[0]
    if count > 64:
        return []
    result = []
    for index in range(count):
        descriptor = address + 8 + index * stride
        result.append(
            {
                "index": index,
                "label": _string_at(process, descriptor),
                "help": _string_at(process, descriptor + help_offset),
            }
        )
    return result


def dump_reaktor_catalog(debugger, command, result, _internal_dict):
    output = command.strip()
    if not output:
        result.SetError("usage: dump-reaktor-catalog OUTPUT.json")
        return
    process = debugger.GetSelectedTarget().GetProcess()
    if not process.IsValid() or process.GetState() != lldb.eStateStopped:
        result.SetError("Reaktor must be stopped after its static initializers (break at main)")
        return

    pointers = _memory(process, TMODDATA_TABLE, KIND_COUNT * 8)
    modules = {}
    for index in range(KIND_COUNT):
        pointer = struct.unpack_from("<Q", pointers, index * 8)[0]
        if not pointer:
            continue
        raw = _memory(process, pointer, TMODDATA_SIZE)
        input_data, output_data = struct.unpack_from("<QQ", raw, 0x20)
        item = {
            "kind": KIND_BASE + index,
            "tmoddata_address": f"0x{pointer:x}",
            "runtime_name": _libcpp_string(process, raw[:24]),
            "runtime_description": _libcpp_string(process, raw[0x48:0x60]),
            "runtime_inputs": _ports(process, input_data, 0x60, 0x48) if input_data else [],
            "runtime_outputs": _ports(process, output_data, 0x40, 0x28) if output_data else [],
        }
        modules[str(KIND_BASE + index)] = item

    with open(output, "w", encoding="utf-8") as stream:
        json.dump({"schema": "reaktor-runtime-tmoddata-v1", "modules": modules}, stream,
                  ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")
    result.AppendMessage(f"wrote {len(modules)} initialized TModData records to {output}")


def __lldb_init_module(debugger, _internal_dict):
    debugger.HandleCommand(
        "command script add -f lldb_dump_catalog.dump_reaktor_catalog dump-reaktor-catalog"
    )
