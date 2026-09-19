#!/usr/bin/env python3
"""Inspect the object hierarchy stored in Native Instruments NRKT files.

This is a conservative parser for the portions of the Reaktor 4/5/6 binary
serialization that have been verified against SteamPipe.  It does not modify
the source file.  Unknown fields are deliberately left uninterpreted.
"""

from __future__ import annotations

import argparse
import base64
import bisect
import hashlib
import json
import struct
import zlib
from collections import Counter
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterator


MODULE_MARKER = b"[\x07\x00\x00\x00KSModul"
PORT_CLASSES = (b"KInPort", b"KOutPort")
# Reaktor's serializer revision is stored in the second KSModul word.  Files
# saved by older Reaktor 5/6 builds use 0x228d00; current 6.5 uses 0x22f300.
MODULE_SIGNATURES = {0x00228D00, 0x0022F300}
ROOT_SENTINEL_ID = 0x75646F4D  # bytes spell "Modu"; root has a different schema
MAX_STRING_SIZE = 1 << 20
DEFAULT_CATALOG = (
    Path(__file__).resolve().parents[2]
    / "tools" / "ReaktorPatchExtractor" / "module_catalog.json"
)
DEFAULT_CORE_CATALOG = (
    Path(__file__).resolve().parents[2]
    / "tools" / "ReaktorPatchExtractor" / "core_module_catalog.json"
)
SIGNAL_TYPES = {
    0: "unspecified_or_hybrid",
    1: "audio",
    2: "event_table_or_internal",
    3: "event_or_control",
}
PANEL_CONTROL_KINDS = {20, 22, 24, 25}
CORE_CELL_KINDS = {524, 525}
CORE_DOCUMENT_MARKER = b"#NI#CS#Document##NI#Reaktor#Core#TaggedFile#"
CORE_DATA_MARKER = b"atad\x02\x00\x00\x00"

# Tags used by NI::SDIF::RXTaggedFileRead/Write.  The names below come from
# the corresponding symbolized Reaktor methods; the numeric IDs are what is
# serialized in the file.
CORE_TAG_FILE = 0x029349BA
CORE_TAG_FILE_TYPE = 0x02A26E32
CORE_TAG_FILE_TYPE_VALUE = 0x03A32928
CORE_TAG_STRUCTURE = 0x028F2D33
CORE_TAG_STRUCTURE_META = 0x03A32D33
CORE_TAG_STRUCTURE_BOUNDS = 0x02C25D33
CORE_TAG_MODULE_ARRAY = 0x02CE4BED
CORE_TAG_ARRAY_COUNT = 0x03A3392D
CORE_TAG_ARRAY_VALUES = 0x028F392D
CORE_TAG_MODULE = 0x02B24BED
CORE_TAG_MODULE_META = 0x03A2C92D
CORE_TAG_MODULE_POSITION = 0x02C2592D
CORE_TAG_POSITION_VALUE = 0x03A3096D
CORE_TAG_PROPERTIES = 0x02CF0CB0
CORE_TAG_PROPERTY_COUNT = 0x03A33CB0
CORE_TAG_PROPERTY_VALUES = 0x028F3CB0
CORE_TAG_PROPERTY = 0x02E70CB0
CORE_TAG_PROPERTY_META = 0x03A39CB0
CORE_TAG_PROPERTY_VALUE = 0x028B9CB0
CORE_TAG_STRING_VALUE = 0x03A30D33
CORE_TAG_INT_VALUE = 0x03A30BA9
CORE_TAG_CONNECTION_ARRAY = 0x02CEEBE3
CORE_TAG_CONNECTION_COUNT = 0x03A33BE3
CORE_TAG_CONNECTION_VALUES = 0x028F3BE3
CORE_TAG_CONNECTION = 0x04BAEBE3
CORE_TAG_CONNECTION_SOURCE = 0x05CEEBE3
CORE_TAG_CONNECTION_TARGET = 0x0392EBE3


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f"u32 outside file at 0x{offset:x}")
    return struct.unpack_from("<I", data, offset)[0]


def marker_offsets(data: bytes, marker: bytes) -> Iterator[int]:
    offset = 0
    while True:
        offset = data.find(marker, offset)
        if offset < 0:
            return
        yield offset
        offset += 1


def decode_nrkt_string(data: bytes, length_offset: int) -> tuple[str, int]:
    length = u32(data, length_offset)
    if length > MAX_STRING_SIZE:
        raise ValueError(f"implausible string length {length} at 0x{length_offset:x}")
    start = length_offset + 4
    end = start + length
    if end > len(data):
        raise ValueError(f"string outside file at 0x{length_offset:x}")
    return data[start:end].decode("utf-8", errors="replace"), end


@dataclass
class Module:
    offset: int
    version: int
    signature: int
    kind: int
    object_id: int
    name: str | None = None
    description: str | None = None
    child_count: int = 0
    input_count: int | None = None
    output_count: int | None = None
    x: int | None = None
    y: int | None = None
    width: int | None = None
    height: int | None = None
    type_name: str | None = None
    parent_object_id: int | None = None
    hierarchy_path: str | None = None
    instance_label: str | None = None
    instance_description: str | None = None
    control_properties: dict[str, float] | None = None
    constant_value: str | None = None
    children: list["Module"] = field(default_factory=list)

    @property
    def is_macro(self) -> bool:
        return self.kind == 4

    def label(self) -> str:
        if self.name:
            return self.name
        return f"kind-{self.kind}"

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["offset"] = f"0x{self.offset:x}"
        result["signature"] = f"0x{self.signature:08x}"
        return result


@dataclass
class Connection:
    target_local_index: int
    target_input_index: int
    target_object_id: int | None = None
    target_name: str | None = None
    target_kind: int | None = None
    target_input_name: str | None = None


@dataclass
class PortRecord:
    offset: int
    class_name: str
    source_object_id: int | None = None
    source_port_record_index: int | None = None
    signal_code: int | None = None
    declared_port_index: int | None = None
    encoding: int | None = None
    name: str | None = None
    description: str | None = None
    connections: list[Connection] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["offset"] = f"0x{self.offset:x}"
        result["signal_type"] = SIGNAL_TYPES.get(self.signal_code, "unknown")
        return result


def printable_strings(data: bytes, start: int, end: int) -> list[tuple[int, str]]:
    """Find conservative length-prefixed UTF-8 strings in one module record."""
    result: list[tuple[int, str]] = []
    for offset in range(start, max(start, end - 4)):
        length = struct.unpack_from("<I", data, offset)[0]
        if not (0 < length <= 16_384) or offset + 4 + length > end:
            continue
        try:
            value = data[offset + 4 : offset + 4 + length].decode("utf-8")
        except UnicodeDecodeError:
            continue
        if value and all(character.isprintable() or character in "\r\n\t" for character in value):
            result.append((offset, value))
    return result


def parse_instance_metadata(data: bytes, modules: list[Module]) -> None:
    """Recover instance labels/help, ranges and displayed constants."""
    for index, module in enumerate(modules):
        end = modules[index + 1].offset if index + 1 < len(modules) else len(data)
        strings = printable_strings(data, module.offset, min(end, module.offset + 0x1000))
        strings = [item for item in strings if item[1] not in {"KSModul", "KInPort", "KOutPort"}]
        if module.kind in PANEL_CONTROL_KINDS:
            for pair_index in range(len(strings) - 1):
                first_offset, label = strings[pair_index]
                second_offset, description = strings[pair_index + 1]
                if (
                    first_offset >= module.offset + 0x100
                    and second_offset == first_offset + 4 + len(label.encode("utf-8"))
                    and len(label) <= 80
                    and len(description) >= 8
                ):
                    module.instance_label = label
                    module.instance_description = description
                    break
            if module.offset + 0x64 <= end:
                step = struct.unpack_from("<d", data, module.offset + 0x48)[0]
                lower, upper = struct.unpack_from("<ff", data, module.offset + 0x5C)
                if (
                    all(map(lambda value: abs(value) < 1e12, (step, lower, upper)))
                    and 1e-12 <= abs(step) < 1e9
                ):
                    module.control_properties = {
                        "step_or_resolution": step,
                        "range_endpoint_1": lower,
                        "range_endpoint_2": upper,
                    }
        elif module.kind == 100:
            candidates = [value for offset, value in strings if offset >= module.offset + 0x80]
            for value in candidates:
                try:
                    float(value)
                except ValueError:
                    continue
                module.constant_value = value
                break


def parse_macro_metadata(data: bytes, module: Module) -> None:
    """Parse the verified common header used by kind-4 macro modules."""
    name_offset = module.offset + (0xF0 if module.signature == 0x00228D00 else 0x104)
    trailer_version = 2 if module.signature == 0x00228D00 else 4
    try:
        name, after_name = decode_nrkt_string(data, name_offset)
        description, after_description = decode_nrkt_string(data, after_name)
    except ValueError:
        return

    close = data.find(b"]", after_description, min(len(data), after_description + 32))
    if close < 0 or close + 9 > len(data):
        return

    # The dword immediately following ']' is consistently 4 for macros.  The
    # next dword is the number of immediate children serialized after it.
    if u32(data, close + 1) != trailer_version:
        return

    module.name = name
    module.description = description or None
    module.child_count = u32(data, close + 5)
    module.input_count = u32(data, module.offset + 0x20)
    module.output_count = u32(data, module.offset + 0x24)
    module.x = signed_u32(u32(data, module.offset + 0x30))
    module.y = signed_u32(u32(data, module.offset + 0x34))
    module.width = u32(data, module.offset + 0x38)
    module.height = u32(data, module.offset + 0x3C)


def parse_modules(data: bytes) -> tuple[list[Module], list[Module]]:
    modules: list[Module] = []
    special_roots: list[Module] = []

    for offset in marker_offsets(data, MODULE_MARKER):
        if offset + 28 > len(data):
            continue
        version, signature, kind, object_id = struct.unpack_from("<IIII", data, offset + 12)
        if signature not in MODULE_SIGNATURES:
            continue
        module = Module(offset, version, signature, kind, object_id)
        if kind == 4:
            parse_macro_metadata(data, module)
        if object_id == ROOT_SENTINEL_ID:
            special_roots.append(module)
        else:
            modules.append(module)

    # An Ensemble embeds an instrument as kind 9.  Its variable-sized header
    # uses the same `]`, dword 4, immediate-child-count trailer as a macro.
    # Parsing this boundary keeps the instrument's local connection indices in
    # a separate scope from the Ensemble's own modules.
    for index, module in enumerate(modules):
        if module.kind != 9:
            continue
        end = modules[index + 1].offset if index + 1 < len(modules) else len(data)
        cursor = module.offset
        while True:
            close = data.find(b"]", cursor, end)
            if close < 0:
                break
            if close + 9 <= end and u32(data, close + 1) in (2, 4):
                candidate = u32(data, close + 5)
                if 0 < candidate < 10000:
                    module.child_count = candidate
            cursor = close + 1

    return modules, special_roots


def signed_u32(value: int) -> int:
    return value - (1 << 32) if value & (1 << 31) else value


def parse_output_port(data: bytes, offset: int) -> PortRecord:
    marker_size = 1 + 4 + len(b"KOutPort")
    cursor = offset + marker_size
    version, signal_code, declared_port_index, encoding = struct.unpack_from(
        "<IIII", data, cursor
    )
    cursor += 16
    if version != 1:
        raise ValueError(f"unsupported KOutPort version {version} at 0x{offset:x}")

    name: str | None = None
    description: str | None = None
    # The low three bits select the serialized text layout.  Higher bits are
    # independent port flags (for example 0x20 in older ensembles), just as
    # they are for KInPort.
    variant = encoding & 0x7
    if variant == 0:
        name, cursor = decode_nrkt_string(data, cursor)
        description, cursor = decode_nrkt_string(data, cursor)
    elif variant == 2:
        description, cursor = decode_nrkt_string(data, cursor)
    elif variant == 4:
        name, cursor = decode_nrkt_string(data, cursor)
    elif variant != 6:
        raise ValueError(f"unknown KOutPort encoding {encoding} at 0x{offset:x}")

    connection_count = u32(data, cursor)
    cursor += 4
    if connection_count > 10000:
        raise ValueError(f"implausible connection count at 0x{offset:x}")
    connections: list[Connection] = []
    for _ in range(connection_count):
        target_local_index = u32(data, cursor)
        target_input_index = u32(data, cursor + 4)
        cursor += 8
        connections.append(Connection(target_local_index, target_input_index))

    if cursor >= len(data) or data[cursor] != ord("]"):
        raise ValueError(f"KOutPort did not end at predicted offset 0x{cursor:x}")

    return PortRecord(
        offset=offset,
        class_name="KOutPort",
        signal_code=signal_code,
        declared_port_index=signed_u32(declared_port_index),
        encoding=encoding,
        name=name,
        description=description,
        connections=connections,
    )


def parse_input_port(data: bytes, offset: int) -> PortRecord:
    marker_size = 1 + 4 + len(b"KInPort")
    cursor = offset + marker_size
    (
        version,
        signal_code,
        declared_port_index,
        _source_module,
        _source_port,
        encoding,
    ) = struct.unpack_from("<IIIIII", data, cursor)
    cursor += 24
    if version != 1:
        raise ValueError(f"unsupported KInPort version {version} at 0x{offset:x}")

    name: str | None = None
    description: str | None = None
    variant = encoding & 0x7
    if variant == 0:
        name, cursor = decode_nrkt_string(data, cursor)
        description, cursor = decode_nrkt_string(data, cursor)
    elif variant == 2:
        description, cursor = decode_nrkt_string(data, cursor)
    elif variant == 4:
        name, cursor = decode_nrkt_string(data, cursor)
    elif variant != 6:
        raise ValueError(f"unknown KInPort encoding {encoding} at 0x{offset:x}")

    if cursor >= len(data) or data[cursor] != ord("]"):
        raise ValueError(f"KInPort did not end at predicted offset 0x{cursor:x}")

    return PortRecord(
        offset=offset,
        class_name="KInPort",
        signal_code=signal_code,
        declared_port_index=signed_u32(declared_port_index),
        encoding=encoding,
        name=name,
        description=description,
    )


def parse_ports(data: bytes, modules: list[Module]) -> tuple[list[PortRecord], list[str]]:
    ports: list[PortRecord] = []
    warnings: list[str] = []
    module_offsets = [module.offset for module in modules]
    input_indices: Counter[int] = Counter()
    output_indices: Counter[int] = Counter()

    for class_bytes in PORT_CLASSES:
        marker = b"[" + struct.pack("<I", len(class_bytes)) + class_bytes
        for offset in marker_offsets(data, marker):
            if class_bytes == b"KOutPort":
                try:
                    port = parse_output_port(data, offset)
                except ValueError as error:
                    warnings.append(str(error))
                    port = PortRecord(offset, "KOutPort")
            else:
                try:
                    port = parse_input_port(data, offset)
                except ValueError as error:
                    warnings.append(str(error))
                    port = PortRecord(offset, "KInPort")

            source_index = bisect.bisect_right(module_offsets, offset) - 1
            if source_index >= 0:
                source_id = modules[source_index].object_id
                port.source_object_id = source_id
                if port.class_name == "KInPort":
                    port.source_port_record_index = input_indices[source_id]
                    input_indices[source_id] += 1
                else:
                    port.source_port_record_index = output_indices[source_id]
                    output_indices[source_id] += 1
            ports.append(port)
    return sorted(ports, key=lambda port: port.offset), warnings


def build_forest(modules: list[Module]) -> tuple[list[Module], list[str]]:
    """Use macro child counts to consume the pre-order module stream."""
    warnings: list[str] = []

    def consume(index: int) -> tuple[Module, int]:
        node = modules[index]
        next_index = index + 1
        node.children = []
        for child_number in range(node.child_count):
            if next_index >= len(modules):
                warnings.append(
                    f"{node.label()} id={node.object_id} expects {node.child_count} children; "
                    f"stream ended at child {child_number}"
                )
                break
            child, next_index = consume(next_index)
            node.children.append(child)
        return node, next_index

    forest: list[Module] = []
    index = 0
    while index < len(modules):
        node, index = consume(index)
        forest.append(node)
    return forest, warnings


def load_catalog(path: Path | None) -> tuple[dict[str, Any], str | None]:
    if path is None or not path.exists():
        return {}, f"module catalog not found: {path}"
    try:
        return json.loads(path.read_text()), None
    except (OSError, json.JSONDecodeError) as error:
        return {}, f"could not read module catalog {path}: {error}"


def enrich_hierarchy(forest: list[Module], catalog: dict[str, Any]) -> None:
    catalog_modules = catalog.get("modules", {})

    def visit(scope: list[Module], parent: Module | None, prefix: str) -> None:
        for local_index, node in enumerate(scope):
            entry = catalog_modules.get(str(node.kind), {})
            node.type_name = entry.get("name")
            node.parent_object_id = parent.object_id if parent else None
            segment = node.name or node.instance_label or node.type_name or f"kind-{node.kind}"
            # Object IDs disambiguate repeated labels and remain stable inside a file.
            node.hierarchy_path = f"{prefix}/{segment}#{node.object_id}" if prefix else f"/{segment}#{node.object_id}"
            visit(node.children, node, node.hierarchy_path)

    visit(forest, None, "")


def resolve_connections(forest: list[Module], ports: list[PortRecord]) -> list[dict[str, Any]]:
    modules_by_id: dict[int, Module] = {}
    sibling_scopes: dict[int, list[Module]] = {}

    def index_scope(scope: list[Module]) -> None:
        for node in scope:
            modules_by_id[node.object_id] = node
            sibling_scopes[node.object_id] = scope
            index_scope(node.children)

    index_scope(forest)
    input_ports = {
        (port.source_object_id, port.source_port_record_index): port
        for port in ports
        if port.class_name == "KInPort" and port.source_object_id is not None
    }
    wires: list[dict[str, Any]] = []
    for port in ports:
        if port.class_name != "KOutPort" or port.source_object_id is None:
            continue
        source = modules_by_id.get(port.source_object_id)
        scope = sibling_scopes.get(port.source_object_id, [])
        for connection in port.connections:
            if connection.target_local_index < len(scope):
                target = scope[connection.target_local_index]
                connection.target_object_id = target.object_id
                connection.target_name = target.name
                connection.target_kind = target.kind
                target_port = input_ports.get((target.object_id, connection.target_input_index))
                if target_port:
                    connection.target_input_name = target_port.name
            wires.append(
                {
                    "source_object_id": port.source_object_id,
                    "source_name": source.name if source else None,
                    "source_kind": source.kind if source else None,
                    "source_output_index": port.source_port_record_index,
                    "source_output_name": port.name,
                    "source_output_description": port.description,
                    "source_signal_code": port.signal_code,
                    "source_signal_type": SIGNAL_TYPES.get(port.signal_code, "unknown"),
                    "declared_port_index": port.declared_port_index,
                    "target_input_description": target_port.description if target_port else None,
                    "target_signal_code": target_port.signal_code if target_port else None,
                    "target_signal_type": (
                        SIGNAL_TYPES.get(target_port.signal_code, "unknown") if target_port else None
                    ),
                    **asdict(connection),
                }
            )
    return wires


def parse_snapshots(
    data: bytes, module_ids: set[int], preferred_object_id_offsets: list[int] | None = None,
) -> list[dict[str, Any]]:
    """Extract verified Reaktor snapshot parameter records.

    A snapshot name is followed by three 36-byte metadata records. Parameter
    records then contain a typed NRKT payload and the target module object ID.
    Candidates are retained only when their IDs resolve into this patch.
    """
    snapshots: list[dict[str, Any]] = []
    seen: set[tuple[int, str]] = set()
    for offset in range(0, max(0, len(data) - 16)):
        length = struct.unpack_from("<I", data, offset)[0]
        if not (1 <= length <= 512) or offset + 4 + length + 12 > len(data):
            continue
        name_end = offset + 4 + length
        if struct.unpack_from("<III", data, name_end) != (9, 2, 9):
            continue
        try:
            name = data[offset + 4 : name_end].decode("utf-8")
        except UnicodeDecodeError:
            continue
        if not name or not all(character.isprintable() for character in name):
            continue
        cursor = name_end + 108
        metadata_extension_bytes = 0
        # Serializer revision 0x228d00 stores two extra snapshot/bank words
        # between the three common metadata records and the parameter stream.
        def parameter_record_starts(at: int) -> bool:
            return (
                at + 12 <= len(data)
                and u32(data, at) in (32, 40, 60)
                and (u32(data, at + 4), u32(data, at + 8)) == (9, 2)
            )

        if not parameter_record_starts(cursor):
            if not parameter_record_starts(cursor + 8):
                continue
            cursor += 8
            metadata_extension_bytes = 8
        parameters: list[dict[str, Any]] = []
        while cursor + 8 <= len(data):
            size = struct.unpack_from("<I", data, cursor)[0]
            if size not in (32, 40, 60) or cursor + size + 8 > len(data):
                break
            payload = data[cursor + 4 : cursor + 4 + size]
            words = list(struct.unpack(f"<{size // 4}I", payload))
            parameter_id = struct.unpack_from("<I", data, cursor + 4 + size)[0]
            if len(words) < 3 or words[:2] != [9, 2] or not parameter_id:
                break
            record: dict[str, Any] = {
                "module_object_id": parameter_id,
                "record_size": size,
                "value_type_code": words[2],
            }
            if size == 40 and len(words) >= 9:
                raw_value = words[8]
                record["raw_value_u32"] = raw_value
                if words[2] == 1:
                    record["value"] = signed_u32(raw_value)
                    record["value_type"] = "integer"
                elif words[2] == 2:
                    record["value"] = struct.unpack("<f", struct.pack("<I", raw_value))[0]
                    record["value_type"] = "normalized_float"
                else:
                    record["value_type"] = "unknown"
            elif size == 32:
                record["value_type"] = "empty_or_null_state"
            else:
                record["value_type"] = "compound_state"
                record["payload_words"] = words
            parameters.append(record)
            cursor += size + 8

        local_ids = [parameter["module_object_id"] for parameter in parameters]
        if not local_ids:
            continue
        preferred = preferred_object_id_offsets or [0]
        object_id_offset = preferred[0]
        resolved = sum(parameter_id + object_id_offset in module_ids for parameter_id in local_ids)
        candidates = {module_id - local_ids[0] for module_id in module_ids}
        ordered_candidates = list(dict.fromkeys(preferred + sorted(candidates, key=abs)))
        for candidate in ordered_candidates:
            score = sum(parameter_id + candidate in module_ids for parameter_id in local_ids)
            if score > resolved:
                object_id_offset, resolved = candidate, score
        mapping_confident = resolved / len(parameters) >= 0.9
        if not mapping_confident and (metadata_extension_bytes != 8 or len(parameters) < 3):
            continue
        for parameter in parameters:
            local_id = parameter["module_object_id"]
            parameter["local_module_object_id"] = local_id
            parameter["module_object_id"] = (
                local_id + object_id_offset if mapping_confident else None
            )
        key = (offset, name)
        if key in seen:
            continue
        seen.add(key)
        snapshots.append(
            {
                "offset": f"0x{offset:x}",
                "name": name,
                "parameter_count": len(parameters),
                "resolved_parameter_count": resolved,
                "object_id_offset": object_id_offset if mapping_confident else None,
                "candidate_object_id_offset": object_id_offset,
                "object_id_mapping_status": "resolved" if mapping_confident else "unresolved",
                "metadata_extension_bytes": metadata_extension_bytes,
                "parameters": parameters,
            }
        )
    return snapshots


@dataclass(frozen=True)
class CoreTagRecord:
    offset: int
    tag: int
    size: int
    payload_offset: int
    end: int


def parse_core_tag_records(
    payload: bytes, start: int = 0, end: int | None = None,
) -> list[CoreTagRecord] | None:
    """Parse one exactly tiled sequence of <tag, size, payload> records."""
    limit = len(payload) if end is None else end
    cursor = start
    records: list[CoreTagRecord] = []
    while cursor + 8 <= limit:
        tag, size = struct.unpack_from("<II", payload, cursor)
        payload_offset = cursor + 8
        record_end = payload_offset + size
        if record_end > limit:
            return None
        records.append(CoreTagRecord(cursor, tag, size, payload_offset, record_end))
        cursor = record_end
    return records if cursor == limit else None


def core_children(payload: bytes, record: CoreTagRecord) -> list[CoreTagRecord]:
    return parse_core_tag_records(payload, record.payload_offset, record.end) or []


def core_child(
    payload: bytes, record: CoreTagRecord, tag: int,
) -> CoreTagRecord | None:
    return next((item for item in core_children(payload, record) if item.tag == tag), None)


def find_core_tags(
    payload: bytes, records: list[CoreTagRecord], tag: int,
) -> Iterator[CoreTagRecord]:
    for record in records:
        if record.tag == tag:
            yield record
        children = core_children(payload, record)
        if children:
            yield from find_core_tags(payload, children, tag)


def decode_core_string(raw: bytes) -> str | None:
    if len(raw) < 5:
        return None
    length = u32(raw, 0)
    # RX strings store a one-byte encoding/version flag between length and text.
    if 5 + length > len(raw):
        return None
    try:
        return raw[5 : 5 + length].decode("utf-8")
    except UnicodeDecodeError:
        return raw[5 : 5 + length].decode("utf-8", errors="replace")


def decode_core_property_value(payload: bytes, record: CoreTagRecord) -> dict[str, Any]:
    cursor = record
    children = core_children(payload, cursor)
    while len(children) == 1:
        cursor = children[0]
        children = core_children(payload, cursor)
    raw = payload[cursor.payload_offset : cursor.end]
    result: dict[str, Any] = {
        "value_tag": f"0x{cursor.tag:08x}",
        "raw_size": len(raw),
    }
    if cursor.tag == CORE_TAG_STRING_VALUE:
        value = decode_core_string(raw)
        if value is not None:
            result.update(value_type="string", value=value)
            return result
    if cursor.tag == CORE_TAG_INT_VALUE and len(raw) == 4:
        result.update(value_type="integer", value=signed_u32(u32(raw, 0)))
        return result
    if len(raw) == 1:
        result.update(value_type="byte", value=raw[0])
    elif len(raw) == 4:
        result.update(value_type="u32", value=u32(raw, 0))
    elif len(raw) == 8:
        result.update(
            value_type="raw_8_bytes",
            raw_words=list(struct.unpack("<II", raw)),
        )
    else:
        result.update(
            value_type="opaque",
            encoding="base64",
            raw_value=base64.b64encode(raw).decode("ascii"),
        )
    return result


def parse_core_properties(payload: bytes, record: CoreTagRecord | None) -> dict[str, Any]:
    if record is None:
        return {"declared_count": 0, "properties": []}
    children = core_children(payload, record)
    count_record = next((item for item in children if item.tag == CORE_TAG_PROPERTY_COUNT), None)
    values_record = next((item for item in children if item.tag == CORE_TAG_PROPERTY_VALUES), None)
    declared = u32(payload, count_record.payload_offset) if count_record and count_record.size >= 4 else 0
    properties: list[dict[str, Any]] = []
    for item in core_children(payload, values_record) if values_record else []:
        if item.tag != CORE_TAG_PROPERTY:
            continue
        item_children = core_children(payload, item)
        meta = next((child for child in item_children if child.tag == CORE_TAG_PROPERTY_META), None)
        value_record = next((child for child in item_children if child.tag == CORE_TAG_PROPERTY_VALUE), None)
        if meta is None or meta.size < 8 or value_record is None:
            continue
        raw_property_id, property_type = struct.unpack_from("<II", payload, meta.payload_offset)
        decoded = decode_core_property_value(payload, value_record)
        decoded.update(
            raw_property_id=raw_property_id,
            property_id=raw_property_id & 0x7FFFFFFF,
            property_flags=raw_property_id >> 31,
            property_type_code=property_type,
        )
        properties.append(decoded)
    result: dict[str, Any] = {
        "declared_count": declared,
        "decoded_count": len(properties),
        "properties": properties,
    }
    for item in properties:
        if item.get("value_type") != "string":
            continue
        if item.get("value") and item["property_id"] in (1, 3):
            result["name"] = item["value"]
        elif item.get("value") and item["property_id"] == 10:
            result["description"] = item["value"]
    return result


def parse_core_endpoint(
    payload: bytes,
    record: CoreTagRecord,
    modules: list[dict[str, Any]],
    is_source: bool,
) -> dict[str, Any]:
    raw = payload[record.payload_offset : record.end]
    words = list(struct.unpack(f"<{len(raw) // 4}I", raw[: len(raw) // 4 * 4]))
    result: dict[str, Any] = {"raw_words": words}
    if not words:
        result["kind"] = "invalid"
        return result
    mode = words[0]
    if mode == 0 and len(words) >= 3:
        module_index, pin_index = words[1], words[2]
        result.update(kind="module_pin", module_index=module_index, pin_index=pin_index)
        if module_index < len(modules):
            module = modules[module_index]
            result.update(
                module_path=module["path"],
                module_type_id=module["type_id"],
                module_type_name=module.get("type_name"),
                module_name=module.get("name"),
            )
        else:
            result["resolution_status"] = "module_index_out_of_range"
    elif is_source and mode == 3 and len(words) >= 2:
        result.update(kind="quick_constant", signal_type_code=words[1])
        if words[1] == 1 and len(raw) >= 12:
            result["value_type"] = "float64"
            result["value"] = struct.unpack_from("<d", raw, len(raw) - 8)[0]
        elif words[1] == 2 and len(raw) >= 8:
            result["value_type"] = "integer"
            result["value"] = signed_u32(words[-1])
    else:
        mode_names = {
            1: "legacy_or_builtin_bus",
            2: "scoped_bus",
            3: "quick_constant" if is_source else "scoped_bus",
        }
        result.update(kind=mode_names.get(mode, "unknown"), mode=mode)
    return result


def parse_core_structure(
    payload: bytes,
    record: CoreTagRecord,
    core_catalog: dict[str, Any],
    path: str = "root",
) -> dict[str, Any]:
    children = core_children(payload, record)
    meta = next((item for item in children if item.tag == CORE_TAG_STRUCTURE_META), None)
    metadata: dict[str, Any] = {}
    if meta and meta.size >= 17:
        metadata = {
            "declared_module_count": u32(payload, meta.payload_offset),
            "has_owner_properties": bool(payload[meta.payload_offset + 4]),
            "quick_bus_count": u32(payload, meta.payload_offset + 5),
            "structure_id": u32(payload, meta.payload_offset + 9),
            "contents_checksum": u32(payload, meta.payload_offset + 13),
        }
    bounds_record = next((item for item in children if item.tag == CORE_TAG_STRUCTURE_BOUNDS), None)
    if bounds_record:
        bounds_children = core_children(payload, bounds_record)
        if bounds_children and bounds_children[0].size >= 16:
            values = struct.unpack_from("<iiii", payload, bounds_children[0].payload_offset)
            metadata["bounds"] = dict(x=values[0], y=values[1], width=values[2], height=values[3])

    property_record = next((item for item in children if item.tag == CORE_TAG_PROPERTIES), None)
    properties = parse_core_properties(payload, property_record)
    catalog_modules = core_catalog.get("modules", {})
    modules: list[dict[str, Any]] = []
    flavors = ("input", "output", "normal")
    arrays = [item for item in children if item.tag == CORE_TAG_MODULE_ARRAY]
    for flavor_index, array in enumerate(arrays[:3]):
        array_children = core_children(payload, array)
        count_record = next((item for item in array_children if item.tag == CORE_TAG_ARRAY_COUNT), None)
        values_record = next((item for item in array_children if item.tag == CORE_TAG_ARRAY_VALUES), None)
        declared_count = u32(payload, count_record.payload_offset) if count_record and count_record.size >= 4 else 0
        module_records = [
            item for item in (core_children(payload, values_record) if values_record else [])
            if item.tag == CORE_TAG_MODULE
        ]
        for flavor_local_index, module_record in enumerate(module_records):
            module_children = core_children(payload, module_record)
            module_meta = next((item for item in module_children if item.tag == CORE_TAG_MODULE_META), None)
            if module_meta is None or module_meta.size < 5:
                continue
            type_id = u32(payload, module_meta.payload_offset)
            has_inner = bool(payload[module_meta.payload_offset + 4])
            position_record = next((item for item in module_children if item.tag == CORE_TAG_MODULE_POSITION), None)
            position: dict[str, int] | None = None
            if position_record:
                values = core_children(payload, position_record)
                if values and values[0].tag == CORE_TAG_POSITION_VALUE and values[0].size >= 8:
                    x, y = struct.unpack_from("<ii", payload, values[0].payload_offset)
                    position = {"x": x, "y": y}
            module_properties = parse_core_properties(
                payload,
                next((item for item in module_children if item.tag == CORE_TAG_PROPERTIES), None),
            )
            module_index = len(modules)
            module_path = f"{path}/m{module_index}"
            catalog_entry = catalog_modules.get(str(type_id), {})
            module: dict[str, Any] = {
                "index": module_index,
                "flavor_local_index": flavor_local_index,
                "flavor_declared_count": declared_count,
                "flavor": flavors[flavor_index] if flavor_index < len(flavors) else f"unknown_{flavor_index}",
                "path": module_path,
                "type_id": type_id,
                "type_name": catalog_entry.get("name", f"core_type_{type_id}"),
                "implementation_class": catalog_entry.get("implementation_class"),
                "has_inner_structure": has_inner,
                "properties": module_properties,
                "record_offset": f"0x{module_record.offset:x}",
                "record_size": module_record.size + 8,
            }
            if position is not None:
                module["position"] = position
            if module_properties.get("name"):
                module["name"] = module_properties["name"]
            if module_properties.get("description"):
                module["description"] = module_properties["description"]
            inner = next((item for item in module_children if item.tag == CORE_TAG_STRUCTURE), None)
            if inner is not None:
                module["inner_structure"] = parse_core_structure(
                    payload, inner, core_catalog, module_path
                )
            modules.append(module)

    connections: list[dict[str, Any]] = []
    connection_array = next((item for item in children if item.tag == CORE_TAG_CONNECTION_ARRAY), None)
    declared_connections = 0
    if connection_array:
        array_children = core_children(payload, connection_array)
        count_record = next((item for item in array_children if item.tag == CORE_TAG_CONNECTION_COUNT), None)
        values_record = next((item for item in array_children if item.tag == CORE_TAG_CONNECTION_VALUES), None)
        declared_connections = u32(payload, count_record.payload_offset) if count_record and count_record.size >= 4 else 0
        for index, item in enumerate(core_children(payload, values_record) if values_record else []):
            if item.tag != CORE_TAG_CONNECTION:
                continue
            item_children = core_children(payload, item)
            source = next((child for child in item_children if child.tag == CORE_TAG_CONNECTION_SOURCE), None)
            target = next((child for child in item_children if child.tag == CORE_TAG_CONNECTION_TARGET), None)
            if source is None or target is None:
                continue
            connections.append(
                {
                    "index": index,
                    "source": parse_core_endpoint(payload, source, modules, True),
                    "target": parse_core_endpoint(payload, target, modules, False),
                }
            )

    result: dict[str, Any] = {
        "path": path,
        **metadata,
        "module_count": len(modules),
        "declared_connection_count": declared_connections,
        "connection_count": len(connections),
        "properties": properties,
        "modules": modules,
        "connections": connections,
    }
    return result


def extract_core_documents(data: bytes, core_catalog: dict[str, Any]) -> list[dict[str, Any]]:
    marker_positions = list(marker_offsets(data, CORE_DOCUMENT_MARKER))
    documents: list[dict[str, Any]] = []
    for document_index, marker_offset in enumerate(marker_positions):
        end = marker_positions[document_index + 1] if document_index + 1 < len(marker_positions) else len(data)
        cursor = marker_offset + len(CORE_DOCUMENT_MARKER)
        segments: list[dict[str, Any]] = []
        decoded_parts: list[bytes] = []
        while True:
            segment_offset = data.find(CORE_DATA_MARKER, cursor, end)
            if segment_offset < 0 or segment_offset + 24 > end:
                break
            algorithm = data[segment_offset + 8 : segment_offset + 16]
            stored_size, decoded_size = struct.unpack_from("<II", data, segment_offset + 16)
            payload_offset = segment_offset + 24
            payload_end = payload_offset + stored_size
            if payload_end > end:
                break
            stored = data[payload_offset:payload_end]
            try:
                if algorithm == b"crngbilz":
                    decoded = zlib.decompress(stored)
                    compression = "zlib"
                elif algorithm == b"crngenon":
                    decoded = stored
                    compression = "none"
                else:
                    cursor = segment_offset + 1
                    continue
            except zlib.error:
                cursor = payload_end
                continue
            if len(decoded) != decoded_size:
                cursor = payload_end
                continue
            decoded_parts.append(decoded)
            segments.append(
                {
                    "offset": f"0x{segment_offset:x}",
                    "compression": compression,
                    "stored_size": stored_size,
                    "decoded_size": decoded_size,
                    "sha256": hashlib.sha256(decoded).hexdigest(),
                }
            )
            cursor = payload_end
        decoded = b"".join(decoded_parts)
        top_records = parse_core_tag_records(decoded) or []
        structure_record = next(find_core_tags(decoded, top_records, CORE_TAG_STRUCTURE), None)
        file_type: str | None = None
        type_container = next(find_core_tags(decoded, top_records, CORE_TAG_FILE_TYPE), None)
        if type_container:
            value_record = core_child(decoded, type_container, CORE_TAG_FILE_TYPE_VALUE)
            if value_record:
                raw = decoded[value_record.payload_offset:value_record.end]
                if len(raw) >= 4:
                    length = u32(raw, 0)
                    if 4 + length <= len(raw):
                        file_type = raw[4 : 4 + length].decode("utf-8", errors="replace")
        document: dict[str, Any] = {
            "document_index": document_index,
            "marker_offset": f"0x{marker_offset:x}",
            "segment_count": len(segments),
            "segments": segments,
            "decoded_size": len(decoded),
            "decoded_sha256": hashlib.sha256(decoded).hexdigest(),
            "tagged_stream_valid": bool(top_records),
        }
        if file_type:
            document["file_type"] = file_type
        if structure_record:
            document["parse_status"] = "decoded"
            structure = parse_core_structure(
                decoded, structure_record, core_catalog
            )
            structures, modules, connections = core_graph_counts(structure)
            document["structure"] = structure
            document["graph_counts"] = {
                "structures": structures,
                "modules": modules,
                "connections": connections,
            }
        else:
            document["parse_status"] = "no_structure_found"
        documents.append(document)
    return documents


def core_graph_counts(structure: dict[str, Any]) -> tuple[int, int, int]:
    structures = 1
    modules = len(structure.get("modules", []))
    connections = len(structure.get("connections", []))
    for module in structure.get("modules", []):
        inner = module.get("inner_structure")
        if inner:
            child_structures, child_modules, child_connections = core_graph_counts(inner)
            structures += child_structures
            modules += child_modules
            connections += child_connections
    return structures, modules, connections


def extract_core_cells(
    data: bytes,
    modules: list[Module],
    core_catalog: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[str]]:
    """Decode embedded Core TaggedFiles and retain the primary record losslessly."""
    core_modules = [module for module in modules if module.kind in CORE_CELL_KINDS]
    documents = extract_core_documents(data, core_catalog)
    warnings: list[str] = []
    if len(core_modules) != len(documents):
        warnings.append(
            f"Core document count ({len(documents)}) does not match Core module count "
            f"({len(core_modules)})"
        )
    module_indices = {id(module): index for index, module in enumerate(modules)}
    result: list[dict[str, Any]] = []
    for core_index, module in enumerate(core_modules):
        index = module_indices[id(module)]
        end = modules[index + 1].offset if index + 1 < len(modules) else len(data)
        primary_record = data[module.offset:end]
        item: dict[str, Any] = {
            "object_id": module.object_id,
            "module_kind": module.kind,
            "module_type_name": module.type_name,
            "hierarchy_path": module.hierarchy_path,
            "offset": f"0x{module.offset:x}",
            "record_size": len(primary_record),
            "encoding": "base64",
            "scope": "serialized primary KSModul record slice, including its ports",
            "sha256": hashlib.sha256(primary_record).hexdigest(),
            "serialized_record": base64.b64encode(primary_record).decode("ascii"),
        }
        if core_index < len(documents):
            item["core_document"] = documents[core_index]
        else:
            item["core_document"] = {"parse_status": "missing"}
        result.append(item)
    return result, warnings


def format_tree(nodes: list[Module], max_depth: int | None, macros_only: bool) -> str:
    lines: list[str] = []

    def visit(node: Module, depth: int) -> None:
        if not macros_only or node.is_macro:
            indent = "  " * depth
            suffix = f" children={node.child_count}" if node.is_macro else ""
            lines.append(
                f"{indent}{node.label()} [id={node.object_id}, kind={node.kind}, "
                f"offset=0x{node.offset:x}{suffix}]"
            )
            output_depth = depth + 1
        else:
            # Do not add a visual indentation level for hidden primitive nodes.
            output_depth = depth

        if max_depth is None or output_depth <= max_depth:
            for child in node.children:
                visit(child, output_depth)

    for root in nodes:
        visit(root, 0)
    return "\n".join(lines)


def format_dot(nodes: list[Module], wires: list[dict[str, Any]]) -> str:
    def escape(value: str) -> str:
        return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

    flattened: list[Module] = []

    def collect(scope: list[Module]) -> None:
        for node in scope:
            flattened.append(node)
            collect(node.children)

    collect(nodes)
    lines = [
        "digraph ReaktorPatch {",
        "  graph [rankdir=LR, overlap=false, splines=true];",
        '  node [shape=box, fontname="Helvetica", fontsize=10];',
        '  edge [fontname="Helvetica", fontsize=8];',
    ]
    for node in flattened:
        title = node.name or f"kind {node.kind}"
        label = escape(f"{title}\\nid {node.object_id} · kind {node.kind}")
        lines.append(f'  n{node.object_id} [label="{label}"];')
    for wire in wires:
        target = wire.get("target_object_id")
        if target is None:
            continue
        output = f"out {wire.get('source_output_index', '?')}"
        input_label = wire.get("target_input_name") or f"in {wire['target_input_index']}"
        edge_label = escape(f"{output} → {input_label}")
        lines.append(
            f'  n{wire["source_object_id"]} -> n{target} [label="{edge_label}"];'
        )
    lines.append("}")
    return "\n".join(lines) + "\n"


def make_report(
    path: Path,
    catalog_path: Path | None = DEFAULT_CATALOG,
    core_catalog_path: Path | None = DEFAULT_CORE_CATALOG,
) -> dict[str, Any]:
    data = path.read_bytes()
    modules, special_roots = parse_modules(data)
    parse_instance_metadata(data, modules)
    forest, warnings = build_forest(modules)
    catalog, catalog_warning = load_catalog(catalog_path)
    core_catalog, core_catalog_warning = load_catalog(core_catalog_path)
    enrich_hierarchy(forest, catalog)
    ports, port_warnings = parse_ports(data, modules)
    wires = resolve_connections(forest, ports)
    kinds = Counter(module.kind for module in modules)
    snapshots = parse_snapshots(
        data,
        {module.object_id for module in modules},
        [module.object_id - 1 for module in modules if module.kind == 9] or [0],
    )
    modules_by_id = {module.object_id: module for module in modules}
    for snapshot in snapshots:
        for parameter in snapshot["parameters"]:
            module = modules_by_id.get(parameter["module_object_id"])
            if module:
                parameter["module_kind"] = module.kind
                parameter["module_type_name"] = module.type_name
                parameter["instance_label"] = module.instance_label
                parameter["hierarchy_path"] = module.hierarchy_path
    core_cells, core_warnings = extract_core_cells(data, modules, core_catalog)
    core_structures = core_modules = core_connections = 0
    for cell in core_cells:
        structure = cell.get("core_document", {}).get("structure")
        if structure:
            counts = core_graph_counts(structure)
            core_structures += counts[0]
            core_modules += counts[1]
            core_connections += counts[2]
    used_types = {
        str(kind): catalog.get("modules", {}).get(str(kind), {"kind": kind, "status": "unknown"})
        for kind in sorted(kinds)
    }
    all_warnings = warnings + port_warnings + core_warnings
    if catalog_warning:
        all_warnings.append(catalog_warning)
    if core_catalog_warning:
        all_warnings.append(core_catalog_warning)

    return {
        "schema": "reaktor-patch-report-v3",
        "source": str(path.resolve()),
        "size_bytes": len(data),
        "nrkt_headers": [f"0x{offset:x}" for offset in marker_offsets(data, b"NRKT")],
        "counts": {
            "modules": len(modules),
            "special_root_records": len(special_roots),
            "input_ports": sum(port.class_name == "KInPort" for port in ports),
            "output_ports": sum(port.class_name == "KOutPort" for port in ports),
            "connections": len(wires),
            "resolved_connections": sum(wire["target_object_id"] is not None for wire in wires),
            "snapshots": len(snapshots),
            "snapshot_parameter_records": sum(item["parameter_count"] for item in snapshots),
            "core_cells": len(core_cells),
            "decoded_core_cells": sum(
                item.get("core_document", {}).get("parse_status") == "decoded"
                for item in core_cells
            ),
            "core_structures": core_structures,
            "core_modules": core_modules,
            "core_connections": core_connections,
            "module_kinds": {str(kind): count for kind, count in sorted(kinds.items())},
        },
        "catalog": {
            "path": str(catalog_path.resolve()) if catalog_path and catalog_path.exists() else None,
            "schema": catalog.get("schema"),
            "source_sha256": catalog.get("source_sha256"),
            "module_count": catalog.get("module_count"),
            "named_module_count": catalog.get("named_module_count"),
            "documented_module_count": catalog.get("documented_module_count"),
            "used_module_types": used_types,
        },
        "core_catalog": {
            "path": str(core_catalog_path.resolve())
            if core_catalog_path and core_catalog_path.exists() else None,
            "schema": core_catalog.get("schema"),
            "source_sha256": core_catalog.get("source_sha256"),
            "module_count": core_catalog.get("module_count"),
        },
        "special_roots": [root.to_dict() for root in special_roots],
        "top_level": [node.to_dict() for node in forest],
        "ports": [port.to_dict() for port in ports],
        "connections": wires,
        "snapshots": snapshots,
        "core_cells": core_cells,
        "warnings": all_warnings,
    }


def format_markdown(report: dict[str, Any]) -> str:
    counts = report["counts"]
    lines = [
        "# Reaktor patch extraction report",
        "",
        f"- Source: `{report['source']}`",
        f"- Modules: {counts['modules']}",
        f"- Connections: {counts['resolved_connections']} / {counts['connections']} resolved",
        f"- Snapshots: {counts['snapshots']} ({counts['snapshot_parameter_records']} parameter records)",
        f"- Core Cells: {counts['decoded_core_cells']} / {counts['core_cells']} decoded "
        f"({counts['core_modules']} modules, {counts['core_connections']} connections)",
        "",
        "## Module types used",
        "",
        "| Kind | Name | Instances | DSP implementation | Description |",
        "|---:|---|---:|---|---|",
    ]
    used = report["catalog"]["used_module_types"]
    for kind, count in report["counts"]["module_kinds"].items():
        item = used.get(kind, {})
        name = str(item.get("name", "unknown")).replace("|", "\\|")
        dsp = str(item.get("mono_audio_function", "")).replace("|", "\\|")
        description = str(item.get("description", "")).replace("\r", " ").replace("\n", " ").replace("|", "\\|")
        lines.append(f"| {kind} | {name} | {count} | {dsp} | {description} |")
    lines.extend(["", "## Snapshots", ""])
    for snapshot in report["snapshots"]:
        lines.append(f"- **{snapshot['name']}** — {snapshot['parameter_count']} parameters")
    if not report["snapshots"]:
        lines.append("No verified snapshot records found.")
    lines.extend(["", "## Warnings", ""])
    lines.extend(f"- {warning}" for warning in report["warnings"])
    if not report["warnings"]:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract the verified object hierarchy from a Reaktor NRKT binary."
    )
    parser.add_argument("file", type=Path, help="Reaktor .ism or .ens file")
    parser.add_argument(
        "--catalog", type=Path, default=DEFAULT_CATALOG,
        help="versioned Reaktor module catalog JSON",
    )
    parser.add_argument(
        "--core-catalog", type=Path, default=DEFAULT_CORE_CATALOG,
        help="versioned Reaktor Core module catalog JSON",
    )
    parser.add_argument("--json", action="store_true", help="emit the complete JSON report")
    parser.add_argument(
        "--wires", action="store_true", help="emit only the resolved connection table as JSON"
    )
    parser.add_argument("--dot", action="store_true", help="emit a Graphviz DOT graph")
    parser.add_argument("--markdown", action="store_true", help="emit a human-readable Markdown report")
    parser.add_argument(
        "--macros-only", action="store_true", help="hide primitive modules in tree output"
    )
    parser.add_argument(
        "--max-depth", type=int, default=None, help="limit printed tree depth"
    )
    args = parser.parse_args()

    report = make_report(args.file, args.catalog, args.core_catalog)
    if args.dot:
        data = args.file.read_bytes()
        modules, _ = parse_modules(data)
        forest, _ = build_forest(modules)
        print(format_dot(forest, report["connections"]), end="")
        return
    if args.wires:
        print(json.dumps(report["connections"], ensure_ascii=False, indent=2))
        return
    if args.markdown:
        print(format_markdown(report), end="")
        return
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return

    print(f"Source: {report['source']}")
    print(f"Size: {report['size_bytes']} bytes")
    print(json.dumps(report["counts"], ensure_ascii=False, indent=2))
    print("\nHierarchy:")

    # Reparse rather than rebuilding dataclasses from the JSON representation.
    data = args.file.read_bytes()
    modules, _ = parse_modules(data)
    forest, warnings = build_forest(modules)
    print(format_tree(forest, args.max_depth, args.macros_only))
    if warnings:
        print("\nWarnings:")
        for warning in warnings:
            print(f"- {warning}")


if __name__ == "__main__":
    main()
