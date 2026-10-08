#!/usr/bin/env python3
"""Upgrade Oceanode presets from fixed-channel SynthDefs to dynamic SynthDefs.

The converter never edits source presets.  It can process one preset or every
preset immediately below a folder, writing copies to an UPGRADED_PRESETS
subfolder by default. Converted preset directory names end in ``_UPG`` so they
cannot collide with their source presets when placed in the same Oceanode bank.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import traceback
import uuid
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import asdict, dataclass, field
from io import StringIO
from pathlib import Path
from typing import Any, Callable, Iterable, Optional


SCRIPT_DIR = Path(__file__).resolve().parent

_DEFAULT_APP_DATA = Path(
    "/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/apps/Playmodes/"
    "OceanodeScroller/bin/data"
)
_DEFAULT_NEWEST = SCRIPT_DIR.parent / "NEWEST"
if not _DEFAULT_NEWEST.exists():
    _DEFAULT_NEWEST = Path("/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST")

DEFAULT_APP_DATA = Path(os.environ.get("OCEANODE_APP_DATA", str(_DEFAULT_APP_DATA)))
DEFAULT_LEGACY_REGISTRY = Path(
    os.environ.get("OCEANODE_LEGACY_REGISTRY", str(DEFAULT_APP_DATA / "Supercollider/Synthdefs.json"))
)
DEFAULT_SYNTHDEFS = Path(os.environ.get("OCEANODE_SYNTHDEFS", str(DEFAULT_APP_DATA / "Supercollider/Synthdefs")))
DEFAULT_MACROS = Path(os.environ.get("OCEANODE_MACROS", str(DEFAULT_APP_DATA / "Macros/AUDIO/EFFECTS")))
DEFAULT_NEWEST = Path(os.environ.get("OCEANODE_NEWEST", str(_DEFAULT_NEWEST)))
DEFAULT_SCLANG = Path(os.environ.get("OCEANODE_SCLANG", "/Applications/SuperCollider.app/Contents/MacOS/sclang"))

JSON_INDENT = 4
OLD_PREFIX = "SC "
NEW_SUFFIX = "*"


# Overrides are intentionally explicit.  Everything else uses conservative
# suffix removal and normalized exact-name matching.
MODEL_OVERRIDES = {
    "filter": "MultimodeFilter",
    "reverb": "RichReverb",
    "eq": "EQ5",
    "bandpass": "BandpassFilter",
    "sampler": "BasicSampler",
    "phmod": "PMod",
    "audioinput": "Input",
    "metal": "MetalPercussion",
    "woodenbar": "WoodBlock",
    "jpverb": "JPReverb",
    "solochannel": "ChannelSolo",
    "brickwallfilter": "PV_Brickwall",
    "fm": "FM2op",
    "membranestereo": "Membrane",
    "percmembrane": "Membrane",
    "membranefx": "Membrane",
    "feedbackb": "FeedbackBCompat",
    "flute2": "Flute2Compat",
    "oteypiano": "OteyPianoCompat",
    "monoto": "NtoM",
    "monoize": "NtoM",
    "fmcomplex": "FMFeedback",
    "buzz": "BuzzBass",
    "additive": "Additive320_",
    "malletin": "Mallet",
    "malletdrone": "Mallet",
    "sinefeed": "FeedbackOsc",
    "grainsamplerb": "GrainSampler",
    "aliasingsynthd": "AliasingSynth",
    "fftstretch2rate": "FFTStretchRate",
    "pulsetrainmulti": "PulseTrain",
    "fm4raw": "FMFeedback",
    "bells": "Bells",
    "dynklang": "DynKlang",
    "ringz": "Ringz",
    "simplereverb": "SimpleReverb",
    "campaneitorsignes": "Campaneitor",
    "campaneitorunna": "Campaneitor",
    "xpanaz78to13": "XPanAzLegacy",
    "forms6": "FormsLegacy",
    "forms13": "FormsLegacy",
}

# These models require structural replacement, not a one-node rename.
PLAIN_CONVERTER_RE = re.compile(r"^StereoMix(?:13|78|6|48)?$", re.IGNORECASE)
PANAZ_CONVERTER_RE = re.compile(r"^PanAz(?:13to6|78to6|78to13)$", re.IGNORECASE)
STEREO_PAN_RE = re.compile(r"^StereoPan(?:13|78|6)$", re.IGNORECASE)
PANAZ_MODEL_RE = re.compile(r"^PanAz(?:13|6)Multi(?:AR)?$", re.IGNORECASE)
LEGACY_PANNER_RE = re.compile(r"^Panner(?:13|6)$", re.IGNORECASE)
FORMS_RE = re.compile(r"^FORMS2?$", re.IGNORECASE)

IGNORED_NODE_KEYS = {"expanded"}
IGNORED_NODE_KEY_NORMALIZED = {"notdistin", "notdistout"}

# These existing models received compatibility parameters as part of the
# converter. Their numbered binaries and metadata must be refreshed when the
# edited source is newer. Additive320_ is new, so normal missing-file detection
# handles it without invalidating every other model in additive.scd.
REFRESH_WHEN_SOURCE_NEWER = {
    "analog", "fmfeedback", "sidechain", "membrane", "mallet",
    "fftstretchrate", "pulsetrain", "bells", "dynklang", "ringz",
    "simplereverb", "campaneitor", "xpanazlegacy", "formslegacy",
    "ntom", "squine", "pitchshifter", "snare808",
    "lfo", "junkperc",
}

# A few OceanAudio binaries were built from earlier parameter names.  The
# installed metadata determines which names the app actually exposes.  Keep
# both normalized lookup keys so saved values and live cables migrate to the
# parameter accepted by the installed binary.
INSTALLED_PARAMETER_ALIASES = {
    "multibandsineshaper": {"mix": "drywet"},
    "combfeed": {"send": "inputgain"},
    "metalpercussion": {"levels": "amp"},
    "cymbalpad": {"levels": "level"},
    "handpan": {"levels": "amp"},
}


SCD_EXACT_HELPER = r'''
(
~synthCreatorExact = {|name, func, description = "", category = "", variables, variableDimensions|
    var variableNames = variables ? [];
    var dimensions = variableDimensions ? [];
    var args = ~exactVariableValues ? [];
    var synthdefName;
    var metadataVariableNames, metadataDimensions, placeholderArgs;

    if(~exactNumChannels.isNil || { ~exactNumChannels.asInteger < 1 }) {
        Error("~exactNumChannels must be an integer greater than zero").throw;
    };
    if(dimensions.size != variableNames.size) {
        Error("Variable dimension count does not match variable count for " ++ name).throw;
    };
    if(args.size != variableNames.size) {
        Error("Exact variable value count does not match variable count for " ++ name).throw;
    };
    args.do { |value, index|
        if((value.asInteger < 1) || { value.asInteger > dimensions[index].asInteger }) {
            Error("Exact variable value outside declared range for " ++ name).throw;
        };
    };

    synthdefName = name ++ (~exactNumChannels.asInteger).asSymbol;
    args.do { |value| synthdefName = synthdefName ++ "_" ++ (value.asInteger).asSymbol; };
    File.mkdir(d +/+ name);

    // A newly added model needs its unnumbered SynthDef and txarcmeta once so
    // Oceanode can discover it. Existing models skip this and receive only the
    // requested numbered variant.
    if(~writeModelMetadata == true) {
        description = description.replace(" ", "_").replace(",", "|");
        metadataVariableNames = if(variableNames.size == 0) { "" } { variableNames.join(":") };
        metadataDimensions = if(dimensions.size == 0) { "" } { dimensions.collect(_.asString).join(":") };
        placeholderArgs = Array.fill(variableNames.size, 1);
        SynthDef.new(name, {
            var sig = SynthDef.wrap(func, prependArgs: [1, placeholderArgs]);
        }, metadata: (
            name: name,
            type: "source",
            description: description,
            category: category,
            variables: metadataVariableNames,
            variableDimensions: metadataDimensions
        )).writeDefFile(d +/+ name);
    };

    ("OCEANODE_CONVERTER_WRITING:" ++ synthdefName).postln;
    SynthDef.new(synthdefName, {
        var sig = SynthDef.wrap(
            func,
            prependArgs: [~exactNumChannels.asInteger, args.collect(_.asInteger)]
        );
    }).writeDefFile(d +/+ name, mdPlugin: AbstractMDPlugin);
};
~synthCreator = ~synthCreatorExact;
);
'''


def normalize(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.lower())


def endpoint_prefix(model: str) -> str:
    return model.replace(" ", "_")


def endpoint_id(model: str, instance: int) -> str:
    return f"{endpoint_prefix(model)}_{instance}"


def node_filename(model: str, instance: int) -> str:
    return f"{endpoint_id(model, instance)}.json"


def upgraded_preset_name(name: str) -> str:
    """Append _UPG to the visible preset name while preserving its number."""
    if "--" in name:
        number, preset_name = name.split("--", 1)
        if preset_name.endswith("_UPG"):
            return name
        return f"{number}--{preset_name}_UPG"
    return name if name.endswith("_UPG") else f"{name}_UPG"


def sc_quote(path: Path | str) -> str:
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, data: Any) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=JSON_INDENT, ensure_ascii=False)
        handle.write("\n")


def scalar_number(value: str) -> Any:
    value = value.strip()
    try:
        number = float(value)
    except ValueError:
        return value
    if number.is_integer():
        return int(number)
    return number


def broadcast_transform(value: Any, function: Callable[[float], float]) -> Any:
    if isinstance(value, list):
        return [broadcast_transform(item, function) for item in value]
    if isinstance(value, (int, float)):
        return function(float(value))
    try:
        return function(float(value))
    except (TypeError, ValueError):
        return value


def broadcast_zip(left: Any, right: Any, function: Callable[[float, float], float]) -> Any:
    if isinstance(left, list) or isinstance(right, list):
        left_values = left if isinstance(left, list) else [left]
        right_values = right if isinstance(right, list) else [right]
        size = max(len(left_values), len(right_values))
        return [
            broadcast_zip(
                left_values[index] if index < len(left_values) else left_values[0],
                right_values[index] if index < len(right_values) else right_values[0],
                function,
            )
            for index in range(size)
        ]
    try:
        return function(float(left), float(right))
    except (TypeError, ValueError):
        return left


def value_matches_default(value: Any, default: Any) -> bool:
    if isinstance(value, list):
        return bool(value) and all(value_matches_default(item, default) for item in value)
    if isinstance(value, (int, float)) and isinstance(default, (int, float)):
        return math.isclose(float(value), float(default), rel_tol=1e-6, abs_tol=1e-6)
    return str(value) == str(default)


def parse_legacy_defaults(params: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for item in params.split(","):
        fields = [field.strip() for field in item.strip().split(":")]
        if len(fields) >= 3 and fields[1]:
            result[normalize(fields[1])] = scalar_number(fields[2])
    return result


def strip_legacy_channel_suffix(name: str) -> list[str]:
    candidates = [name]
    mono = re.sub(r"Mono$", "", name, flags=re.IGNORECASE)
    if mono != name:
        candidates.append(mono)
    plain = re.sub(r"(?:13|78|48|32|25|24|21|20|16|6|2|1)(?:ar)?$", "", name, flags=re.IGNORECASE)
    if plain != name:
        candidates.append(plain)
    variant = re.match(r"^(.*?)(?:13|78|48|32|25|24|21|20|16|6|2|1)([BCD])$", name, re.IGNORECASE)
    if variant:
        candidates.append(variant.group(1) + variant.group(2))
    return list(dict.fromkeys(candidates))


def extract_balanced_call(text: str, start: int) -> str:
    opening = text.find("(", start)
    if opening < 0:
        return text[start:]
    depth = 0
    quote: Optional[str] = None
    escaped = False
    line_comment = False
    block_comment = False
    index = opening
    while index < len(text):
        char = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and nxt == "/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char == "/" and nxt == "/":
            line_comment = True
            index += 1
        elif char == "/" and nxt == "*":
            block_comment = True
            index += 1
        elif char in {'"', "'"}:
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
        index += 1
    return text[start:]


@dataclass
class ModelSpec:
    name: str
    source: Path
    params: dict[str, str]
    variables: list[str]
    variable_dimensions: list[int]
    installed_dir: Optional[Path] = None

    def expected_filename(self, num_channels: int, variable_values: Iterable[int]) -> str:
        suffix = "".join(f"_{int(value)}" for value in variable_values)
        return f"{self.name}{int(num_channels)}{suffix}.scsyndef"


class SourceCatalog:
    CALL_RE = re.compile(r"~synthCreator(?:WithMetadata)?\.value\(\s*\"([^\"]+)\"")
    PARAM_RE = re.compile(
        r"Oceanode(?:Parameter(?:Lag|Dropdown|FloatDropdown)?|Input|Output|Buffer|InternalBuffer)"
        r"\.(?:ar|kr)\(\s*\\([A-Za-z0-9_]+)"
    )
    VARIABLES_RE = re.compile(r"variables\s*:\s*\[([^\]]*)\]", re.DOTALL)
    DIMENSIONS_RE = re.compile(r"variableDimensions\s*:\s*\[([^\]]*)\]", re.DOTALL)

    def __init__(self, newest: Path, synthdefs_root: Path):
        self.newest = newest
        self.synthdefs_root = synthdefs_root
        self.models: dict[str, ModelSpec] = {}
        self.installed_dirs = self._installed_directory_index()
        self._scan_sources()

    def _installed_directory_index(self) -> dict[str, Path]:
        result: dict[str, Path] = {}
        if not self.synthdefs_root.exists():
            return result
        for metadata in self.synthdefs_root.rglob("*.txarcmeta"):
            if "OLD" in metadata.parts:
                continue
            result[normalize(metadata.stem)] = metadata.parent
        return result

    @staticmethod
    def _source_rank(path: Path) -> tuple[int, int, str]:
        parts = set(path.parts)
        if "SYNTHDEFS" in parts:
            priority = 0
        elif "LIGHTNET" in parts:
            priority = 1
        else:
            priority = 2
        return priority, len(path.parts), str(path)

    def _scan_sources(self) -> None:
        selected: dict[str, tuple[tuple[int, int, str], ModelSpec]] = {}
        for source in self.newest.rglob("*.scd"):
            text = source.read_text(encoding="utf-8", errors="ignore")
            for match in self.CALL_RE.finditer(text):
                name = match.group(1)
                segment = extract_balanced_call(text, match.start())
                symbols = list(dict.fromkeys(self.PARAM_RE.findall(segment)))
                params = {normalize(symbol): symbol[:1].upper() + symbol[1:] for symbol in symbols}
                params.setdefault("out", "Out")
                variables_match = self.VARIABLES_RE.search(segment)
                variables = re.findall(r'"([^\"]+)"', variables_match.group(1)) if variables_match else []
                dimensions_match = self.DIMENSIONS_RE.search(segment)
                dimensions = (
                    [int(value) for value in re.findall(r"\d+", dimensions_match.group(1))]
                    if dimensions_match
                    else []
                )
                spec = ModelSpec(
                    name=name,
                    source=source,
                    params=params,
                    variables=variables,
                    variable_dimensions=dimensions,
                    installed_dir=(
                        self.installed_dirs.get(normalize(name))
                        or self._inferred_installed_dir(source, name)
                    ),
                )
                key = normalize(name)
                rank = self._source_rank(source)
                if key not in selected or rank < selected[key][0]:
                    selected[key] = (rank, spec)
        self.models = {key: item[1] for key, item in selected.items()}
        self._reconcile_installed_parameter_names()

    def _reconcile_installed_parameter_names(self) -> None:
        for model_name, aliases in INSTALLED_PARAMETER_ALIASES.items():
            spec = self.models.get(model_name)
            if not spec or not spec.installed_dir:
                continue
            metadata = spec.installed_dir / f"{spec.name}.txarcmeta"
            if not metadata.is_file():
                continue
            text = metadata.read_text(encoding="utf-8", errors="ignore")
            for source_name, installed_name in aliases.items():
                source_found = re.search(rf"['\"]{re.escape(source_name)}['\"]", text)
                installed_found = re.search(rf"['\"]{re.escape(installed_name)}['\"]", text)
                if source_name in spec.params and installed_found and not source_found:
                    display = installed_name[:1].upper() + installed_name[1:]
                    spec.params[source_name] = display
                    spec.params[installed_name] = display

    def _inferred_installed_dir(self, source: Path, name: str) -> Optional[Path]:
        """Infer the category directory for a new model without metadata yet."""
        source_root = self.newest / "SYNTHDEFS"
        try:
            relative = source.relative_to(source_root)
        except ValueError:
            return None
        return self.synthdefs_root / relative.parent / name

    def get(self, name: str) -> Optional[ModelSpec]:
        return self.models.get(normalize(name))

    def match_legacy(self, legacy_name: str) -> Optional[ModelSpec]:
        base_candidates = strip_legacy_channel_suffix(legacy_name)
        for base in base_candidates:
            override = MODEL_OVERRIDES.get(normalize(base))
            if override:
                return self.get(override)
        for base in base_candidates:
            match = self.get(base)
            if match:
                return match
        return None


@dataclass
class SynthRequirement:
    model: str
    num_channels: int
    variable_values: list[int]
    filename: str
    installed_path: str
    source: str
    status: str


@dataclass
class NodeIssue:
    graph: str
    node: str
    legacy_model: str
    reason: str


@dataclass
class PresetReport:
    preset: str
    converted_nodes: int = 0
    remaining_legacy_nodes: int = 0
    graph_count: int = 0
    complete: bool = False
    issues: list[NodeIssue] = field(default_factory=list)


@dataclass
class RunReport:
    source: str
    destination: str
    dry_run: bool
    presets: list[PresetReport] = field(default_factory=list)
    synthdefs: list[SynthRequirement] = field(default_factory=list)


@dataclass
class EndpointRoute:
    source_id: str
    source_params: dict[str, Any]
    destination_id: str
    destination_params: dict[str, Any]


@dataclass
class PlannedNode:
    old_model: str
    old_instance: int
    old_id: str
    route: EndpointRoute
    new_modules: list[tuple[str, int, list[float], dict[str, Any]]]
    internal_edges: list[tuple[str, str, str, str, Any]]
    primary_id: str


class ExactSynthCompiler:
    def __init__(
        self,
        catalog: SourceCatalog,
        sclang: Path,
        compile_missing: bool,
        dry_run: bool,
        report: RunReport,
    ):
        self.catalog = catalog
        self.sclang = sclang
        self.compile_missing = compile_missing
        self.dry_run = dry_run
        self.report = report
        self.seen: dict[tuple[str, int, tuple[int, ...]], bool] = {}

    def ensure(self, spec: ModelSpec, num_channels: int, variable_values: list[int]) -> bool:
        key = (normalize(spec.name), int(num_channels), tuple(variable_values))
        if key in self.seen:
            return self.seen[key]
        expected = spec.expected_filename(num_channels, variable_values)
        installed_dir = spec.installed_dir
        installed_path = installed_dir / expected if installed_dir else Path(expected)
        metadata_path = installed_dir / f"{spec.name}.txarcmeta" if installed_dir else None
        check_freshness = normalize(spec.name) in REFRESH_WHEN_SOURCE_NEWER
        source_newer = bool(
            check_freshness
            and installed_dir
            and installed_path.exists()
            and spec.source.stat().st_mtime > installed_path.stat().st_mtime
        )
        metadata_stale = bool(
            check_freshness
            and metadata_path
            and metadata_path.exists()
            and spec.source.stat().st_mtime > metadata_path.stat().st_mtime
        )
        if installed_dir and installed_path.exists() and not source_newer and not metadata_stale:
            status = "present"
            success = True
        elif self.dry_run:
            status = "would_compile" if self.compile_missing else "missing"
            success = self.compile_missing
        elif not self.compile_missing:
            status = "missing"
            success = False
        elif not installed_dir:
            status = "missing_model_directory"
            success = False
        else:
            success, status = self._compile(spec, num_channels, variable_values, installed_path)
        self.report.synthdefs.append(
            SynthRequirement(
                model=spec.name,
                num_channels=num_channels,
                variable_values=list(variable_values),
                filename=expected,
                installed_path=str(installed_path),
                source=str(spec.source),
                status=status,
            )
        )
        self.seen[key] = success
        return success

    def _compile(
        self, spec: ModelSpec, num_channels: int, variable_values: list[int], installed_path: Path
    ) -> tuple[bool, str]:
        if not self.sclang.exists():
            return False, "sclang_not_found"
        with tempfile.TemporaryDirectory(prefix="oceanode_synthdef_") as temp_name:
            temp = Path(temp_name)
            output = temp / "compiled"
            output.mkdir()
            values = ", ".join(str(int(value)) for value in variable_values)
            wrapper = temp / "compile_exact.scd"
            metadata_path = installed_path.parent / f"{spec.name}.txarcmeta"
            write_metadata = (
                not metadata_path.exists()
                or spec.source.stat().st_mtime > metadata_path.stat().st_mtime
            )
            script = (
                "(\n"
                f'd = "{sc_quote(output)}";\n'
                f"~exactNumChannels = {int(num_channels)};\n"
                f"~exactVariableValues = [{values}];\n"
                f"~writeModelMetadata = {'true' if write_metadata else 'false'};\n"
                f"{SCD_EXACT_HELPER}\n"
                f'"{sc_quote(spec.source)}".load;\n'
                '"OCEANODE_CONVERTER_DONE".postln;\n'
                "0.exit;\n"
                ")\n"
            )
            wrapper.write_text(script, encoding="utf-8")
            try:
                process = subprocess.run(
                    [str(self.sclang), str(wrapper)],
                    capture_output=True,
                    text=True,
                    timeout=180,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                return False, "compile_process_failed"
            generated = output / spec.name / installed_path.name
            log = (process.stdout or "") + "\n" + (process.stderr or "")
            if process.returncode != 0 or "ERROR:" in log or not generated.exists():
                print(
                    f"SynthDef compilation failed for {spec.name} {num_channels} "
                    f"{variable_values}:\n{log[-6000:]}",
                    file=sys.stderr,
                )
                return False, "compile_failed"
            installed_path.parent.mkdir(parents=True, exist_ok=True)
            temporary_target = installed_path.with_name(installed_path.name + f".tmp-{uuid.uuid4().hex}")
            shutil.copy2(generated, temporary_target)
            os.replace(temporary_target, installed_path)
            if write_metadata:
                generated_model = output / spec.name / f"{spec.name}.scsyndef"
                generated_metadata = output / spec.name / f"{spec.name}.txarcmeta"
                if not generated_model.exists() or not generated_metadata.exists():
                    return False, "metadata_compile_failed"
                for generated_file in (generated_model, generated_metadata):
                    target = installed_path.parent / generated_file.name
                    temporary = target.with_name(target.name + f".tmp-{uuid.uuid4().hex}")
                    shutil.copy2(generated_file, temporary)
                    os.replace(temporary, target)
                return True, "compiled_with_metadata"
            return True, "compiled"


class MacroTemplates:
    def __init__(self, root: Path):
        self.root = root
        self.panner_dir = root / "CHAN_CONVERTER"
        self.panaz_dir = root / "CHAN_CONVERTER_PANAZ"
        self.indexer = self._load(self.panner_dir / "Indexer_1.json", self._default_indexer())
        self.panner = self._load(self.panner_dir / "SC_Panner*_1.json", self._default_panner())
        self.panaz = self._load(self.panaz_dir / "SC_PanAz*_1.json", self._default_panaz())

    @staticmethod
    def _load(path: Path, fallback: dict[str, Any]) -> dict[str, Any]:
        try:
            data = read_json(path)
            return data if isinstance(data, dict) else fallback
        except (OSError, json.JSONDecodeError):
            return fallback

    @staticmethod
    def _default_indexer() -> dict[str, Any]:
        return {
            "Comb": "0", "Discrete": "0", "Invert": "1", "Modulo": "13",
            "NWaves": "1", "Norm": "1", "Offset": "0", "Output": [0.0],
            "Quant": "13", "Random": "0", "Shuffle": "0", "Size": "13",
            "Sym": "0", "expanded": True,
        }

    @staticmethod
    def _default_panner() -> dict[str, Any]:
        return {"Level": 1.0, "N_Chan": "13", "NumSpeakers": "2", "Position": [0.0], "Width": 2.0, "expanded": True}

    @staticmethod
    def _default_panaz() -> dict[str, Any]:
        return {"Level": 1.0, "N_Chan": "13", "NumSpeakers": "2", "Orientation": "0.5", "Position": 0.0, "Width": 2.0, "expanded": True}


def flatten_connections(data: Any) -> list[tuple[str, str, str, str, Any]]:
    edges: list[tuple[str, str, str, str, Any]] = []
    if not isinstance(data, dict):
        return edges
    for source_id, outputs in data.items():
        if not isinstance(outputs, dict):
            continue
        for source_param, targets in outputs.items():
            if not isinstance(targets, dict):
                continue
            for destination_id, inputs in targets.items():
                if not isinstance(inputs, dict):
                    continue
                for destination_param, payload in inputs.items():
                    edges.append((source_id, source_param, destination_id, destination_param, payload))
    return edges


def build_connections(edges: Iterable[tuple[str, str, str, str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for source_id, source_param, destination_id, destination_param, payload in edges:
        result.setdefault(source_id, {}).setdefault(source_param, {}).setdefault(destination_id, {})[
            destination_param
        ] = payload
    return result


def recursively_replace_strings(data: Any, replacements: dict[str, str]) -> Any:
    if isinstance(data, dict):
        return {
            replacements.get(str(key), str(key)): recursively_replace_strings(value, replacements)
            for key, value in data.items()
        }
    if isinstance(data, list):
        return [recursively_replace_strings(item, replacements) for item in data]
    if isinstance(data, str):
        updated = data
        for old, new in replacements.items():
            updated = updated.replace(old, new)
        return updated
    return data


class GraphConverter:
    def __init__(
        self,
        registry: dict[str, Any],
        catalog: SourceCatalog,
        compiler: ExactSynthCompiler,
        templates: MacroTemplates,
        preset_root: Path,
        preset_report: PresetReport,
        dry_run: bool,
    ):
        self.registry = registry
        self.catalog = catalog
        self.compiler = compiler
        self.templates = templates
        self.preset_root = preset_root
        self.report = preset_report
        self.dry_run = dry_run

    def convert(self, graph: Path) -> None:
        modules_path = graph / "modules.json"
        connections_path = graph / "connections.json"
        try:
            modules = read_json(modules_path)
        except (OSError, json.JSONDecodeError) as error:
            self._issue(graph, "modules.json", "", f"invalid modules.json: {error}")
            return
        if not isinstance(modules, dict):
            return
        try:
            connection_data = read_json(connections_path) if connections_path.exists() else {}
        except json.JSONDecodeError as error:
            self._issue(graph, "connections.json", "", f"invalid connections.json: {error}")
            return
        edges = flatten_connections(connection_data)
        edges, modern_changed = self._repair_modern_amp_aliases(graph, modules, edges)
        edges, router_replacements = self._repair_sc_buffer_routers(graph, modules, edges)
        planned: list[PlannedNode] = []
        reserved = self._reserved_instances(modules)

        for model, instances in list(modules.items()):
            if not model.startswith(OLD_PREFIX) or not isinstance(instances, dict):
                continue
            legacy_name = model[len(OLD_PREFIX) :]
            if legacy_name not in self.registry:
                continue
            for instance_key, position in list(instances.items()):
                instance = int(instance_key)
                old_id = endpoint_id(model, instance)
                node_path = graph / node_filename(model, instance)
                try:
                    node_data = read_json(node_path)
                except (OSError, json.JSONDecodeError) as error:
                    self._issue(graph, old_id, legacy_name, f"cannot read node JSON: {error}")
                    continue
                plan, reason = self._plan_node(
                    graph, model, legacy_name, instance, list(position), node_data, edges, reserved
                )
                if plan:
                    planned.append(plan)
                else:
                    self._issue(graph, old_id, legacy_name, reason or "no safe mapping")

        if not planned:
            if not self.dry_run:
                if router_replacements:
                    write_json(modules_path, modules)
                if modern_changed or router_replacements:
                    write_json(connections_path, build_connections(edges))
                if router_replacements:
                    self._rewrite_ancillary_json(graph, router_replacements)
            self.report.remaining_legacy_nodes += self._count_legacy(modules)
            return

        route_by_id = {plan.old_id: plan.route for plan in planned}
        rebuilt_edges: list[tuple[str, str, str, str, Any]] = []
        for source_id, source_param, destination_id, destination_param, payload in edges:
            if source_id in route_by_id:
                route = route_by_id[source_id]
                mapped = route.source_params.get(normalize(source_param))
                if mapped is None:
                    continue
                if isinstance(mapped, tuple):
                    source_id, source_param = mapped
                else:
                    source_id, source_param = route.source_id, mapped
            if destination_id in route_by_id:
                route = route_by_id[destination_id]
                mapped = route.destination_params.get(normalize(destination_param))
                if mapped is None:
                    continue
                if isinstance(mapped, list):
                    for mapped_item in mapped:
                        if isinstance(mapped_item, tuple):
                            rebuilt_edges.append((source_id, source_param, mapped_item[0], mapped_item[1], payload))
                        else:
                            rebuilt_edges.append((source_id, source_param, route.destination_id, mapped_item, payload))
                    continue
                if isinstance(mapped, tuple):
                    destination_id, destination_param = mapped
                else:
                    destination_id, destination_param = route.destination_id, mapped
            rebuilt_edges.append((source_id, source_param, destination_id, destination_param, payload))
        for plan in planned:
            rebuilt_edges.extend(plan.internal_edges)

        id_replacements: dict[str, str] = {}
        for plan in planned:
            old_instances = modules.get(plan.old_model, {})
            old_instances.pop(f"{plan.old_instance:02d}", None)
            old_instances.pop(str(plan.old_instance), None)
            if not old_instances:
                modules.pop(plan.old_model, None)
            for new_model, new_instance, position, node_data in plan.new_modules:
                modules.setdefault(new_model, {})[f"{new_instance:02d}"] = position
                if not self.dry_run:
                    write_json(graph / node_filename(new_model, new_instance), node_data)
            id_replacements[plan.old_id] = plan.primary_id
            old_file = graph / node_filename(plan.old_model, plan.old_instance)
            if not self.dry_run and old_file.exists():
                old_file.unlink()
            self.report.converted_nodes += 1

        if not self.dry_run:
            write_json(modules_path, modules)
            write_json(connections_path, build_connections(rebuilt_edges))
            self._rewrite_ancillary_json(graph, {**router_replacements, **id_replacements})
        self.report.remaining_legacy_nodes += self._count_legacy(modules)

    def _repair_sc_buffer_routers(
        self,
        graph: Path,
        modules: dict[str, Any],
        edges: list[tuple[str, str, str, str, Any]],
    ) -> tuple[list[tuple[str, str, str, str, Any]], dict[str, str]]:
        """Use the registered vector<int> router for saved SC buffer numbers."""
        obsolete = modules.get("Router ScBuffer")
        if not isinstance(obsolete, dict):
            return edges, {}
        replacements: dict[str, str] = {}
        current = modules.get("Router v_i", {})
        if not isinstance(current, dict):
            return edges, {}
        for instance_key, position in list(obsolete.items()):
            try:
                instance = int(instance_key)
            except (TypeError, ValueError):
                self._issue(graph, str(instance_key), "Router ScBuffer", "invalid router instance")
                continue
            old_id = endpoint_id("Router ScBuffer", instance)
            new_id = endpoint_id("Router v_i", instance)
            old_path = graph / f"{old_id}.json"
            new_path = graph / f"{new_id}.json"
            collision = any(int(key) == instance for key in current if str(key).isdigit())
            if collision or new_path.exists() or not old_path.is_file():
                self._issue(graph, old_id, "Router ScBuffer", "router destination collision or missing node JSON")
                continue
            try:
                node_data = read_json(old_path)
            except (OSError, json.JSONDecodeError) as error:
                self._issue(graph, old_id, "Router ScBuffer", f"cannot read node JSON: {error}")
                continue
            if not isinstance(node_data, dict):
                self._issue(graph, old_id, "Router ScBuffer", "router node JSON is not an object")
                continue
            current[instance_key] = position
            del obsolete[instance_key]
            replacements[old_id] = new_id
            if not self.dry_run:
                write_json(new_path, node_data)
                old_path.unlink()
        if not replacements:
            return edges, {}
        modules["Router v_i"] = current
        if not obsolete:
            modules.pop("Router ScBuffer", None)
        rewritten = [
            (replacements.get(source_id, source_id), source_param,
             replacements.get(destination_id, destination_id), destination_param, payload)
            for source_id, source_param, destination_id, destination_param, payload in edges
        ]
        return rewritten, replacements

    def _repair_modern_amp_aliases(
        self,
        graph: Path,
        modules: dict[str, Any],
        edges: list[tuple[str, str, str, str, Any]],
    ) -> tuple[list[tuple[str, str, str, str, Any]], bool]:
        """Repair Amp only when the installed model metadata exposes Levels."""
        target_ids: set[str] = set()
        changed = False
        for model, instances in modules.items():
            if not model.startswith(OLD_PREFIX) or not model.endswith(NEW_SUFFIX):
                continue
            spec = self.catalog.get(model[len(OLD_PREFIX):-len(NEW_SUFFIX)])
            if not spec or "amp" in spec.params or "levels" not in spec.params:
                continue
            # Presets run against the installed metadata and numbered binary,
            # which may lag the .scd source. SquareOS is one such case: its
            # source says Levels, but the installed model still exposes Amp.
            metadata = spec.installed_dir / f"{spec.name}.txarcmeta" if spec.installed_dir else None
            if not metadata or not metadata.is_file():
                continue
            metadata_text = metadata.read_text(encoding="utf-8", errors="ignore")
            if not re.search(r"['\"]levels['\"]", metadata_text) or re.search(r"['\"]amp['\"]", metadata_text):
                continue
            for instance_key in instances:
                instance = int(instance_key)
                node_id = endpoint_id(model, instance)
                target_ids.add(node_id)
                node_path = graph / node_filename(model, instance)
                if not node_path.exists():
                    continue
                node_data = read_json(node_path)
                old_keys = [key for key in node_data if normalize(key) == "amp"]
                if not old_keys:
                    continue
                levels_display = spec.params["levels"].replace(" ", "_")
                if not any(normalize(key) == "levels" for key in node_data):
                    node_data[levels_display] = node_data[old_keys[0]]
                for key in old_keys:
                    del node_data[key]
                if not self.dry_run:
                    write_json(node_path, node_data)
                changed = True
        repaired_edges = []
        for source_id, source_param, dest_id, dest_param, payload in edges:
            if dest_id in target_ids and normalize(dest_param) == "amp":
                dest_param = "Levels"
                changed = True
            repaired_edges.append((source_id, source_param, dest_id, dest_param, payload))
        return repaired_edges, changed

    def _plan_node(
        self,
        graph: Path,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        position: list[float],
        node_data: dict[str, Any],
        edges: list[tuple[str, str, str, str, Any]],
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        definition = self.registry[legacy_name]
        input_count = int(definition.get("In_Size", 0) or 0)
        output_count = int(definition.get("Out_Size", 0) or 0)
        num_channels = max(input_count, output_count, 1)
        old_id = endpoint_id(old_model, old_instance)

        if PLAIN_CONVERTER_RE.match(legacy_name):
            return self._plan_plain_converter(
                old_model, legacy_name, old_instance, old_id, position, input_count, output_count, reserved
            )
        if PANAZ_CONVERTER_RE.match(legacy_name) or STEREO_PAN_RE.match(legacy_name):
            return self._plan_panaz_converter(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                input_count, output_count, edges, reserved,
            )
        if PANAZ_MODEL_RE.match(legacy_name) or LEGACY_PANNER_RE.match(legacy_name):
            return self._plan_panaz_converter(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                input_count, output_count, edges, reserved,
            )
        if FORMS_RE.match(legacy_name):
            return self._plan_forms(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                output_count, edges, reserved,
            )
        if normalize(legacy_name) == "multar":
            return self._plan_multar(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                input_count, reserved,
            )
        if normalize(legacy_name) == "fm4raw":
            return self._plan_fm4raw(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                output_count, edges, reserved,
            )
        if normalize(legacy_name) in {"bubblea", "bubbleb"}:
            return self._plan_stereo_bubble(
                old_model, legacy_name, old_instance, old_id, position, node_data,
                definition, edges, reserved,
            )

        spec = self.catalog.match_legacy(legacy_name)
        if not spec:
            return None, "no unambiguous new SynthDef model"
        if normalize(spec.name) == "ntom":
            num_channels = max(input_count, 1)
        variable_values, variable_reason = self._variable_values(spec, legacy_name, input_count, output_count)
        if variable_reason:
            return None, variable_reason
        new_data, destination_params, reason = self._migrate_parameters(
            legacy_name, spec, node_data, definition, edges, old_id
        )
        if reason:
            return None, reason
        if not self.compiler.ensure(spec, num_channels, variable_values):
            return None, f"required SynthDef {spec.expected_filename(num_channels, variable_values)} is unavailable"
        new_model = f"SC {spec.name}{NEW_SUFFIX}"
        new_instance = self._allocate(reserved, new_model)
        new_id = endpoint_id(new_model, new_instance)
        new_data["N_Chan"] = str(num_channels)
        for variable, value in zip(spec.variables, variable_values):
            new_data[variable.replace(" ", "_")] = str(value)
        new_data.setdefault("expanded", node_data.get("expanded", True))
        self._apply_legacy_mode(legacy_name, spec, new_data)
        source_params = {normalize("Out"): "Out"}
        for norm_name, display in spec.params.items():
            source_params[norm_name] = display
        new_modules = [(new_model, new_instance, position, new_data)]
        internal_edges: list[tuple[str, str, str, str, Any]] = []
        if self._requires_q_inversion(legacy_name):
            connected_q = any(
                destination_id == old_id and normalize(destination_param) == "q"
                for _, _, destination_id, destination_param, _ in edges
            )
            if connected_q:
                transform_model = "Transformations"
                transform_instance = self._allocate(reserved, transform_model)
                transform_id = endpoint_id(transform_model, transform_instance)
                transform_data = {
                    "Input": 0.0,
                    "Output": 1.0,
                    "Trans": "1",
                    "expanded": False,
                }
                x, y = position[:2]
                new_modules.append(
                    (transform_model, transform_instance, [x - 190.0, y + 115.0], transform_data)
                )
                destination_params["q"] = (transform_id, "Input")
                internal_edges.append((transform_id, "Output", new_id, "Q", None))
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(new_id, source_params, new_id, destination_params),
            new_modules=new_modules,
            internal_edges=internal_edges,
            primary_id=new_id,
        ), None

    def _plan_stereo_bubble(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        node_data: dict[str, Any],
        definition: dict[str, Any],
        edges: list[tuple[str, str, str, str, Any]],
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        """Replace legacy stereo bubbles with a mono bubble and Pan2 stage."""
        bubble_spec = self.catalog.match_legacy(legacy_name)
        pan_spec = self.catalog.get("StereoPan2")
        if not bubble_spec or not pan_spec:
            return None, "new bubble or StereoPan2 model/source is unavailable"
        # BubbleA is one voice panned to stereo. BubbleB is registered as two
        # channels but its compiled file actually makes two overlapping
        # eight-channel Out writes. The new BubbleB deliberately mixes those
        # filter bands into one voice, then this stage pans it to stereo.
        saved_without_pan = {
            key: value for key, value in node_data.items() if normalize(key) != "pan"
        }
        edges_without_pan = [
            edge for edge in edges
            if not (edge[2] == old_id and normalize(edge[3]) == "pan")
        ]
        bubble_data, destinations, reason = self._migrate_parameters(
            legacy_name, bubble_spec, saved_without_pan, definition,
            edges_without_pan, old_id,
        )
        if reason:
            return None, reason
        if not self.compiler.ensure(bubble_spec, 1, []):
            return None, f"required SynthDef {bubble_spec.expected_filename(1, [])} is unavailable"
        if not self.compiler.ensure(pan_spec, 1, []):
            return None, f"required SynthDef {pan_spec.expected_filename(1, [])} is unavailable"

        bubble_model = f"SC {bubble_spec.name}{NEW_SUFFIX}"
        pan_model = f"SC {pan_spec.name}{NEW_SUFFIX}"
        bubble_instance = self._allocate(reserved, bubble_model)
        pan_instance = self._allocate(reserved, pan_model)
        bubble_id = endpoint_id(bubble_model, bubble_instance)
        pan_id = endpoint_id(pan_model, pan_instance)
        bubble_data["N_Chan"] = "1"
        bubble_data.setdefault("expanded", node_data.get("expanded", True))
        pan_data = {
            "N_Chan": "1",
            "Pan": node_data.get("pan", 0.0),
            "expanded": node_data.get("expanded", True),
        }
        destinations["pan"] = (pan_id, "Pan")
        x, y = position[:2]
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(pan_id, {"out": "Out"}, bubble_id, destinations),
            new_modules=[
                (bubble_model, bubble_instance, [x, y], bubble_data),
                (pan_model, pan_instance, [x + 190.0, y], pan_data),
            ],
            internal_edges=[(bubble_id, "Out", pan_id, "In", None)],
            primary_id=pan_id,
        ), None

    def _plan_multar(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        node_data: dict[str, Any],
        input_count: int,
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        spec = self.catalog.get("Arithmetic")
        if not spec:
            return None, "new Arithmetic model/source is unavailable"
        if not self.compiler.ensure(spec, input_count, []):
            return None, f"required SynthDef {spec.expected_filename(input_count, [])} is unavailable"
        new_model = "SC Arithmetic*"
        new_instance = self._allocate(reserved, new_model)
        new_id = endpoint_id(new_model, new_instance)
        new_data = {
            "N_Chan": str(input_count),
            "Operation": "2",
            "expanded": node_data.get("expanded", True),
        }
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(
                new_id,
                {"out": "Out"},
                new_id,
                {"in": "In1", "in1": "In1", "in2": "In2"},
            ),
            new_modules=[(new_model, new_instance, position, new_data)],
            internal_edges=[],
            primary_id=new_id,
        ), None

    @staticmethod
    def _apply_legacy_mode(legacy_name: str, spec: ModelSpec, node_data: dict[str, Any]) -> None:
        legacy = normalize(legacy_name)
        model = normalize(spec.name)

        def set_param(normalized_name: str, value: Any) -> None:
            display = spec.params.get(normalized_name)
            if display:
                node_data[display.replace(" ", "_")] = value

        if model == "analog" and legacy.startswith("analog"):
            set_param("fmmode", "1")
            if legacy.endswith("ar"):
                set_param("inputmode", "1")
        elif model == "sidechain" and legacy.startswith("sidechain"):
            set_param("mode", "1")
        elif model == "membrane" and legacy.startswith("percmembrane"):
            set_param("behavior", "1")
        elif model == "mallet" and legacy.startswith("malletin"):
            set_param("mode", "3")
        elif model == "mallet" and legacy.startswith("malletdrone"):
            set_param("mode", "1")
        elif model == "feedbackosc" and legacy.startswith("sinefeed"):
            set_param("wavetype", "0")

    def _plan_forms(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        node_data: dict[str, Any],
        output_count: int,
        edges: list[tuple[str, str, str, str, Any]],
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        """Replace FORMS with Additive1080_ and an Hz-to-pitch conversion."""
        spec = self.catalog.get("Additive1080_")
        if not spec:
            return None, "new Additive1080_ model/source is unavailable"
        connected = self._connected_destination_params(edges, old_id)
        connected_norm = {normalize(parameter) for parameter in connected}
        supported = {"freqarray", "pitcharray", "amparray", "panarray", "stereoamp"}
        unsupported = [parameter for parameter in connected if normalize(parameter) not in supported]
        if unsupported:
            return None, "unmapped connected FORMS parameters: " + ", ".join(sorted(unsupported))
        if not self.compiler.ensure(spec, output_count, []):
            return None, f"required SynthDef {spec.expected_filename(output_count, [])} is unavailable"

        additive_model = "SC Additive1080_*"
        conversion_model = "Conversions"
        vector_sum_model = "Vector Item Operations"
        additive_instance = self._allocate(reserved, additive_model)
        additive_id = endpoint_id(additive_model, additive_instance)

        old_freq = self._node_value(node_data, "freqarray", 60.0)
        old_pitch = self._node_value(node_data, "pitcharray", 36.0)
        old_amp = self._node_value(node_data, "amparray", 0.0)
        old_pan = self._node_value(node_data, "panarray", 0.0)
        old_levels = self._node_value(node_data, "stereoamp", 1.0)
        pitch_hz = broadcast_transform(old_pitch, self._pitch_to_hz)
        summed_hz = broadcast_zip(old_freq, pitch_hz, lambda left, right: left + right)
        pitch = broadcast_transform(summed_hz if "pitcharray" in connected_norm else old_freq, self._hz_to_pitch)
        expanded = node_data.get("expanded", True)
        additive_data = {
            "Amparray": old_amp,
            "Levels": old_levels,
            "N_Chan": str(output_count),
            "Panarray": old_pan,
            "Pitcharray": pitch,
            "Width": "2",
            "expanded": expanded,
        }
        x, y = position[:2]
        new_modules: list[tuple[str, int, list[float], dict[str, Any]]] = [
            (additive_model, additive_instance, [x, y], additive_data)
        ]
        internal_edges: list[tuple[str, str, str, str, Any]] = []
        freq_destination: str | tuple[str, str] = "Pitcharray"
        pitch_destination: str | tuple[str, str] = "Pitcharray"
        if "pitcharray" in connected_norm:
            pitch_hz_instance = self._allocate(reserved, conversion_model)
            vector_sum_instance = self._allocate(reserved, vector_sum_model)
            final_pitch_instance = self._allocate(reserved, conversion_model)
            pitch_hz_id = endpoint_id(conversion_model, pitch_hz_instance)
            vector_sum_id = endpoint_id(vector_sum_model, vector_sum_instance)
            final_pitch_id = endpoint_id(conversion_model, final_pitch_instance)
            pitch_hz_data = {
                "Input": old_pitch,
                "Op": "8",
                "Output": pitch_hz,
                "expanded": expanded,
            }
            vector_sum_data = {
                "Input_1": old_freq,
                "Input_2": pitch_hz,
                "Op_": "1",
                "Output": summed_hz,
                "Trig_In": "2",
                "expanded": expanded,
            }
            final_pitch_data = {
                "Input": summed_hz,
                "Op": "9",
                "Output": pitch,
                "expanded": expanded,
            }
            new_modules.insert(0, (conversion_model, final_pitch_instance, [x - 205.0, y - 55.0], final_pitch_data))
            new_modules.insert(0, (vector_sum_model, vector_sum_instance, [x - 410.0, y - 55.0], vector_sum_data))
            new_modules.insert(0, (conversion_model, pitch_hz_instance, [x - 615.0, y + 15.0], pitch_hz_data))
            internal_edges.append((pitch_hz_id, "Output", vector_sum_id, "Input.2", None))
            internal_edges.append((vector_sum_id, "Output", final_pitch_id, "Input", None))
            internal_edges.append((final_pitch_id, "Output", additive_id, "Pitcharray", None))
            freq_destination = (vector_sum_id, "Input.1")
            pitch_destination = (pitch_hz_id, "Input")
        else:
            conversion_instance = self._allocate(reserved, conversion_model)
            conversion_id = endpoint_id(conversion_model, conversion_instance)
            conversion_data = {
                "Input": old_freq,
                "Op": "9",
                "Output": pitch,
                "expanded": expanded,
            }
            new_modules.insert(0, (conversion_model, conversion_instance, [x - 205.0, y - 115.0], conversion_data))
            internal_edges.append((conversion_id, "Output", additive_id, "Pitcharray", None))
            freq_destination = (conversion_id, "Input")
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(
                additive_id,
                {"out": "Out"},
                additive_id,
                {
                    "freqarray": freq_destination,
                    "pitcharray": pitch_destination,
                    "amparray": "Amparray",
                    "panarray": "Panarray",
                    "stereoamp": "Levels",
                },
            ),
            new_modules=new_modules,
            internal_edges=internal_edges,
            primary_id=additive_id,
        ), None

    @staticmethod
    def _node_value(node_data: dict[str, Any], normalized_name: str, default: Any) -> Any:
        for key, value in node_data.items():
            if normalize(key) == normalized_name:
                return value
        return default

    @staticmethod
    def _hz_to_pitch(value: float) -> float:
        # Matches the Conversions node's hz-pitch operation. Values at or
        # below zero are clamped to MIDI pitch 0, the Additive input minimum.
        if value <= 0:
            return 0.0
        return 69.0 + 12.0 * math.log2(value / 440.0)

    @staticmethod
    def _pitch_to_hz(value: float) -> float:
        return 440.0 * math.pow(2.0, (value - 69.0) / 12.0)

    def _plan_plain_converter(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        input_count: int,
        output_count: int,
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        spec = self.catalog.get("Panner")
        if not spec:
            return None, "new Panner model/source is unavailable"
        variables = [output_count]
        if not self.compiler.ensure(spec, input_count, variables):
            return None, f"required SynthDef {spec.expected_filename(input_count, variables)} is unavailable"
        indexer_model = "Indexer"
        panner_model = "SC Panner*"
        indexer_instance = self._allocate(reserved, indexer_model)
        panner_instance = self._allocate(reserved, panner_model)
        indexer_id = endpoint_id(indexer_model, indexer_instance)
        panner_id = endpoint_id(panner_model, panner_instance)
        indexer_data = copy.deepcopy(self.templates.indexer)
        indexer_data["Size"] = str(input_count)
        indexer_data["Modulo"] = str(input_count)
        indexer_data["Quant"] = str(input_count)
        if input_count <= 1:
            positions = [0.0]
        else:
            positions = [index / (input_count - 1) for index in range(input_count)]
        indexer_data["Output"] = positions
        panner_data = copy.deepcopy(self.templates.panner)
        panner_data["N_Chan"] = str(input_count)
        panner_data["NumSpeakers"] = str(output_count)
        panner_data["Position"] = positions
        # Legacy StereoMix uses Splay.ar with levelComp enabled by default.
        panner_data["Level"] = 1.0 / math.sqrt(input_count)
        x, y = position[:2]
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(
                panner_id, {"out": "Out"}, panner_id, {"in": "In"}
            ),
            new_modules=[
                (indexer_model, indexer_instance, [x - 210.0, y + 145.0], indexer_data),
                (panner_model, panner_instance, [x, y], panner_data),
            ],
            internal_edges=[(indexer_id, "Output", panner_id, "Position", None)],
            primary_id=panner_id,
        ), None

    def _plan_panaz_converter(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        node_data: dict[str, Any],
        input_count: int,
        output_count: int,
        edges: list[tuple[str, str, str, str, Any]],
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        spec = self.catalog.get("PanAz")
        if not spec:
            return None, "new PanAz model/source is unavailable"
        variables = [output_count]
        if not self.compiler.ensure(spec, input_count, variables):
            return None, f"required SynthDef {spec.expected_filename(input_count, variables)} is unavailable"
        new_model = "SC PanAz*"
        new_instance = self._allocate(reserved, new_model)
        new_id = endpoint_id(new_model, new_instance)
        new_data = copy.deepcopy(self.templates.panaz)
        new_data["N_Chan"] = str(input_count)
        new_data["NumSpeakers"] = str(output_count)
        aliases = {
            "pan": "Position",
            "position": "Position",
            "in2": "Position",
            "width": "Width",
            "orientation": "Orientation",
            "level": "Level",
            "wet": "Level",
            "levelsout": "Level",
        }
        for key, value in node_data.items():
            destination = aliases.get(normalize(key))
            if destination:
                new_data[destination] = value
        destination_params = {"in": "In"}
        for alias, destination in aliases.items():
            destination_params[alias] = destination
        connected = self._connected_destination_params(edges, old_id)
        unsupported = [param for param in connected if normalize(param) not in destination_params]
        if unsupported:
            return None, f"unmapped connected PanAz parameters: {', '.join(sorted(unsupported))}"
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(new_id, {"out": "Out"}, new_id, destination_params),
            new_modules=[(new_model, new_instance, position, new_data)],
            internal_edges=[],
            primary_id=new_id,
        ), None

    def _plan_fm4raw(
        self,
        old_model: str,
        legacy_name: str,
        old_instance: int,
        old_id: str,
        position: list[float],
        node_data: dict[str, Any],
        output_count: int,
        edges: list[tuple[str, str, str, str, Any]],
        reserved: dict[str, set[int]],
    ) -> tuple[Optional[PlannedNode], Optional[str]]:
        spec = self.catalog.get("FMFeedback")
        if not spec:
            return None, "new FMFeedback model/source is unavailable"
        num_channels = max(output_count, 1)
        if not self.compiler.ensure(spec, num_channels, []):
            return None, f"required SynthDef {spec.expected_filename(num_channels, [])} is unavailable"

        display = spec.params
        new_data: dict[str, Any] = {
            "Feed1": 0,
            "Feed2": 0,
            "Feed3": 0,
            "Feed4": 0,
            "expanded": node_data.get("expanded", True),
        }
        direct = {
            "pitch": display.get("pitch", "Pitch"),
            "r1": display.get("r1", "R1"),
            "r2": display.get("r2", "R2"),
            "r3": display.get("r3", "R3"),
            "r4": display.get("r4", "R4"),
            "levels": display.get("levels", "Levels"),
        }
        defaults = parse_legacy_defaults(str(self.registry[legacy_name].get("Params", "")))
        unmapped_values: list[str] = []
        for key, value in node_data.items():
            key_norm = normalize(key)
            if key in IGNORED_NODE_KEYS or key_norm in IGNORED_NODE_KEY_NORMALIZED or key_norm == "nchan":
                continue
            if key_norm == "fm":
                for target in ("fm1", "fm2", "fm3", "fm4"):
                    new_data[display.get(target, target.title()).replace(" ", "_")] = value
            elif key_norm in direct:
                new_data[direct[key_norm].replace(" ", "_")] = value
            elif key_norm in defaults and value_matches_default(value, defaults[key_norm]):
                continue
            else:
                unmapped_values.append(key)
        if unmapped_values:
            return None, "non-default saved parameters have no safe mapping: " + ", ".join(sorted(unmapped_values))

        new_model = "SC FMFeedback*"
        new_instance = self._allocate(reserved, new_model)
        new_id = endpoint_id(new_model, new_instance)
        new_data["N_Chan"] = str(num_channels)
        destination_params: dict[str, Any] = {"in": "In"}
        for source_norm, destination in direct.items():
            destination_params[source_norm] = destination
        destination_params["fm"] = [
            display.get("fm1", "Fm1"),
            display.get("fm2", "Fm2"),
            display.get("fm3", "Fm3"),
            display.get("fm4", "Fm4"),
        ]
        connected = self._connected_destination_params(edges, old_id)
        unsupported = [parameter for parameter in connected if normalize(parameter) not in destination_params]
        if unsupported:
            return None, "unmapped live parameters: " + ", ".join(sorted(unsupported))
        return PlannedNode(
            old_model=old_model,
            old_instance=old_instance,
            old_id=old_id,
            route=EndpointRoute(new_id, {"out": "Out"}, new_id, destination_params),
            new_modules=[(new_model, new_instance, position, new_data)],
            internal_edges=[],
            primary_id=new_id,
        ), None

    def _variable_values(
        self, spec: ModelSpec, legacy_name: str, input_count: int, output_count: int
    ) -> tuple[list[int], Optional[str]]:
        if not spec.variables:
            return [], None
        names = [normalize(value) for value in spec.variables]
        if names in (["numout"], ["numspeakers"]):
            return [output_count], None
        if normalize(spec.name) == "input" and len(names) == 1:
            return [output_count], None
        return [], f"new model {spec.name} requires unresolved compile variables {spec.variables}"

    def _migrate_parameters(
        self,
        legacy_name: str,
        spec: ModelSpec,
        node_data: dict[str, Any],
        definition: dict[str, Any],
        edges: list[tuple[str, str, str, str, Any]],
        old_id: str,
    ) -> tuple[dict[str, Any], dict[str, Any], Optional[str]]:
        defaults = parse_legacy_defaults(str(definition.get("Params", "")))
        param_overrides = self._parameter_overrides(legacy_name, spec.name)
        destination_params = dict(spec.params)
        # A legacy input cable can only survive when the new model exposes an
        # audio input. Two-input models commonly call their first bus In1;
        # generators such as Noise have no input and must not receive In.
        if "in" not in destination_params and int(definition.get("In", 0)) > 0:
            if "in1" in destination_params:
                destination_params["in"] = destination_params["in1"]
        migrated: dict[str, Any] = {}
        unmapped_values: list[str] = []

        # LinearGain's legacy Pow is always one in this bank. GainMult therefore
        # maps exactly to the new Gain parameter, including live connections.
        linear_gain = normalize(legacy_name).startswith("lineargain") and normalize(spec.name) == "lineargain"
        if linear_gain:
            power = node_data.get("Pow", 1)
            if not value_matches_default(power, 1):
                return {}, {}, "LinearGain Pow is not 1 and cannot be folded into a live Gain connection"
        grain_sampler_b = normalize(strip_legacy_channel_suffix(legacy_name)[-1]) == "grainsamplerb"

        for key, value in node_data.items():
            key_norm = normalize(key)
            if key in IGNORED_NODE_KEYS or key_norm in IGNORED_NODE_KEY_NORMALIZED or key_norm in {"nchan"}:
                continue
            if linear_gain and key_norm == "pow":
                continue
            if grain_sampler_b and key_norm == "envtype":
                continue
            # Analog13 presets may retain a Phase field from an older node
            # description. The legacy registry and new Analog expose no Phase;
            # discard only its neutral zero value. A live Phase cable remains
            # an error below, and a nonzero saved value is still reported.
            if normalize(legacy_name).startswith("analog") and key_norm == "phase" and value_matches_default(value, 0):
                continue
            target_norm = self._resolve_parameter(key_norm, destination_params, param_overrides)
            target_display = destination_params.get(target_norm)
            if target_display:
                if self._requires_q_inversion(legacy_name) and target_norm == "q":
                    value = broadcast_transform(value, lambda item: 1.0 - item)
                migrated[target_display.replace(" ", "_")] = value
            elif key_norm in defaults and value_matches_default(value, defaults[key_norm]):
                continue
            else:
                unmapped_values.append(key)

        connected = self._connected_destination_params(edges, old_id)
        unmapped_connections: list[str] = []
        for parameter in connected:
            norm_parameter = normalize(parameter)
            if norm_parameter == "in":
                continue
            target_norm = self._resolve_parameter(norm_parameter, destination_params, param_overrides)
            target_display = destination_params.get(target_norm)
            if target_display:
                destination_params[norm_parameter] = target_display
            else:
                unmapped_connections.append(parameter)
        if unmapped_connections:
            return {}, {}, "unmapped live parameters: " + ", ".join(sorted(unmapped_connections))
        if unmapped_values:
            return {}, {}, "non-default saved parameters have no safe mapping: " + ", ".join(sorted(unmapped_values))
        migrated["expanded"] = node_data.get("expanded", True)
        return migrated, destination_params, None

    @staticmethod
    def _requires_q_inversion(legacy_name: str) -> bool:
        normalized = normalize(legacy_name)
        return normalized.startswith("filter") or normalized.startswith("bandpass")

    @staticmethod
    def _resolve_parameter(
        source_norm: str, destination_params: dict[str, str], explicit: dict[str, str]
    ) -> str:
        if source_norm in destination_params:
            return source_norm
        explicit_target = explicit.get(source_norm)
        if explicit_target in destination_params:
            return explicit_target
        fallbacks = {
            "amp": ["levels", "level"],
            "levels": ["level", "amp"],
            "level": ["levels", "amp"],
            "masterlevel": ["levels", "level"],
            "inputgain": ["send", "gain"],
            "buf": ["bufnum"],
            "buffer": ["bufnum"],
            "trig": ["gate", "trigger", "trigg"],
            "trigger": ["gate", "trigg"],
            "onoff": ["on"],
            "offsetms": ["offset"],
            "in2": ["control"],
            "morph": ["morphamt"],
            "x": ["positionx"],
            "y": ["positiony"],
            "pos": ["position"],
            "hz": ["freq", "frequency"],
        }
        for candidate in fallbacks.get(source_norm, []):
            if candidate in destination_params:
                return candidate
        return explicit_target or source_norm

    @staticmethod
    def _parameter_overrides(legacy_name: str, new_name: str) -> dict[str, str]:
        mapping: dict[str, str] = {
            "wet": "mix", "drywet": "mix", "feedback": "feed", "feed": "feedback",
        }
        old_base = normalize(strip_legacy_channel_suffix(legacy_name)[-1])
        if old_base == "reverb" and normalize(new_name) == "richreverb":
            mapping.update({"inputlevel": "send"})
        if old_base == "eq" and normalize(new_name) == "eq5":
            mapping.update({
                "lowf": "lowshelf", "lowdb": "lowshelfdb",
                "midlowf": "peak1", "midlowdb": "peak1db",
                "midf": "peak2", "middb": "peak2db",
                "midhighf": "peak3", "midhighdb": "peak3db",
                "highf": "hishelf", "highdb": "hishelfdb",
            })
        if old_base == "lineargain" and normalize(new_name) == "lineargain":
            mapping.update({"gainmult": "gain"})
        if old_base == "audioinput" and normalize(new_name) == "input":
            mapping.update({"inchan": "chan"})
        if old_base in {"grainsamplerb", "samplerstretch"}:
            mapping.update({"bufnum2": "envbuf"})
        if old_base == "samplerstretch":
            mapping.update({
                "interp": "interpolation",
                "pos": "position",
                "winrand": "windowrand",
                "winsize": "windowsize",
            })
        if old_base == "shiftchannels" and normalize(new_name) == "shiftchannels":
            mapping.update({"in2": "shift"})
        if old_base == "xpanaz" and normalize(new_name) == "xpanaz":
            mapping.update({"x": "position"})
        if old_base == "wtsynth" and normalize(new_name) == "wtsynth":
            mapping.update({"wtpos": "wtpos8"})
        if old_base == "polymixer" and normalize(new_name) == "polymixer":
            mapping.update({"mute": "gainvec"})
        return mapping

    @staticmethod
    def _connected_destination_params(
        edges: list[tuple[str, str, str, str, Any]], old_id: str
    ) -> list[str]:
        return [destination_param for _, _, destination_id, destination_param, _ in edges if destination_id == old_id]

    @staticmethod
    def _reserved_instances(modules: dict[str, Any]) -> dict[str, set[int]]:
        result: dict[str, set[int]] = {}
        for model, instances in modules.items():
            if isinstance(instances, dict):
                result[model] = {int(instance) for instance in instances}
        return result

    @staticmethod
    def _allocate(reserved: dict[str, set[int]], model: str) -> int:
        used = reserved.setdefault(model, set())
        candidate = 1
        while candidate in used:
            candidate += 1
        used.add(candidate)
        return candidate

    def _rewrite_ancillary_json(self, graph: Path, replacements: dict[str, str]) -> None:
        excluded = {"modules.json", "connections.json"}
        for path in graph.glob("*.json"):
            if path.name in excluded or re.search(r"_\d+\.json$", path.name):
                continue
            try:
                data = read_json(path)
            except (OSError, json.JSONDecodeError):
                continue
            updated = recursively_replace_strings(data, replacements)
            if updated != data:
                write_json(path, updated)

    def _count_legacy(self, modules: dict[str, Any]) -> int:
        count = 0
        for model, instances in modules.items():
            if model.startswith(OLD_PREFIX) and model[len(OLD_PREFIX) :] in self.registry and isinstance(instances, dict):
                count += len(instances)
        return count

    def _issue(self, graph: Path, node: str, legacy_model: str, reason: str) -> None:
        try:
            relative = str(graph.relative_to(self.preset_root)) or "."
        except ValueError:
            relative = str(graph)
        self.report.issues.append(NodeIssue(relative, node, legacy_model, reason))


def find_oceanode_node_loader(*paths: Path) -> Optional[Path]:
    """Find the addon source when converting data from an openFrameworks app."""
    for path in paths:
        for ancestor in (path, *path.parents):
            if ancestor.name == "openFrameworks":
                loader = ancestor / "addons/ofxOceanode/src/Nodes/ofxOceanodeNode.cpp"
                if loader.is_file():
                    return loader
    return None


def has_unsafe_wrapped_parameter_load(loader: Path) -> bool:
    """Detect ofDeserialize's invalid ofParameter<T> cast on Oceanode wrappers."""
    source = loader.read_text(encoding="utf-8", errors="replace")
    function = re.search(
        r"void\s+ofxOceanodeNode::deserializeParameter\s*\(.*?(?=\nvoid\s+ofxOceanodeNode::|\Z)",
        source,
        re.DOTALL,
    )
    return bool(function and re.search(r"\bofDeserialize\s*\(\s*json\s*,\s*p\s*\)", function.group()))


class PresetConverter:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        loader = find_oceanode_node_loader(args.source, args.synthdefs)
        if loader and has_unsafe_wrapped_parameter_load(loader):
            message = (
                f"Unsafe Oceanode preset loader: {loader} calls ofDeserialize(json, p) "
                "on a wrapped parameter. Valid numeric float values can crash it. "
                "Fix that loader and rebuild the app before installing converted presets."
            )
            if args.dry_run:
                print("WARNING: " + message, file=sys.stderr)
            else:
                raise ValueError(message)
        self.registry = read_json(args.legacy_registry)
        if not isinstance(self.registry, dict):
            raise ValueError("Legacy Synthdefs registry must be a JSON object")
        self.catalog = SourceCatalog(args.newest, args.synthdefs)
        self.templates = MacroTemplates(args.macros)
        self.run_report = RunReport(str(args.source), str(args.output), args.dry_run)
        self.compiler = ExactSynthCompiler(
            self.catalog, args.sclang, not args.no_compile, args.dry_run, self.run_report
        )

    def run(self) -> int:
        presets = discover_presets(self.args.source, self.args.preset, self.args.all)
        if not presets:
            raise ValueError("No Oceanode presets found")
        if not self.args.dry_run:
            self.args.output.mkdir(parents=True, exist_ok=True)
        for preset in presets:
            report = PresetReport(preset=preset.name)
            self.run_report.presets.append(report)
            if self.args.dry_run:
                work_root = preset
                self._convert_preset(work_root, report)
            else:
                self._copy_and_convert(preset, report)
            report.complete = report.remaining_legacy_nodes == 0 and not report.issues
        if not self.args.dry_run:
            write_json(self.args.output / "conversion_report.json", report_to_json(self.run_report))
        print_summary(self.run_report)
        incomplete = any(not preset.complete for preset in self.run_report.presets)
        return 2 if incomplete and self.args.require_complete else 0

    def _copy_and_convert(self, source: Path, report: PresetReport) -> None:
        destination_name = upgraded_preset_name(source.name)
        destination = self.args.output / destination_name
        if destination.exists() and not self.args.overwrite:
            report.issues.append(NodeIssue(".", "", "", f"destination already exists: {destination}"))
            report.remaining_legacy_nodes = count_preset_legacy(source, self.registry)
            return
        temporary = self.args.output / f".{destination_name}.tmp-{uuid.uuid4().hex}"
        baseline_invalid = invalid_connection_endpoints(source)
        try:
            shutil.copytree(source, temporary)
            self._convert_preset(temporary, report, baseline_invalid)
            if destination.exists():
                shutil.rmtree(destination)
            os.replace(temporary, destination)
        except Exception as error:
            if temporary.exists():
                shutil.rmtree(temporary)
            report.issues.append(NodeIssue(".", "", "", f"preset conversion failed: {error}"))
            report.converted_nodes = 0
            report.remaining_legacy_nodes = count_preset_legacy(source, self.registry)

    def _convert_preset(
        self,
        preset: Path,
        report: PresetReport,
        allowed_invalid: Optional[set[str]] = None,
    ) -> None:
        graph_dirs = sorted({path.parent for path in preset.rglob("modules.json")})
        report.graph_count = len(graph_dirs)
        converter = GraphConverter(
            self.registry, self.catalog, self.compiler, self.templates, preset, report, self.args.dry_run
        )
        for graph in graph_dirs:
            converter.convert(graph)
        if not self.args.dry_run:
            validate_preset(preset, allowed_invalid or set())


def discover_presets(source: Path, requested: Optional[str], all_presets: bool) -> list[Path]:
    source = source.resolve()
    if requested:
        candidate = Path(requested).expanduser()
        if not candidate.is_absolute():
            candidate = source / candidate
        candidate = candidate.resolve()
        return [candidate] if is_preset(candidate) else []
    if is_preset(source) and not all_presets:
        return [source]
    return sorted(path for path in source.iterdir() if path.is_dir() and is_preset(path))


def is_preset(path: Path) -> bool:
    return path.is_dir() and (path / "modules.json").exists()


def count_preset_legacy(preset: Path, registry: dict[str, Any]) -> int:
    total = 0
    for modules_path in preset.rglob("modules.json"):
        try:
            modules = read_json(modules_path)
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(modules, dict):
            continue
        for model, instances in modules.items():
            if model.startswith(OLD_PREFIX) and model[len(OLD_PREFIX) :] in registry and isinstance(instances, dict):
                total += len(instances)
    return total


def invalid_connection_endpoints(preset: Path) -> set[str]:
    invalid: set[str] = set()
    for modules_path in preset.rglob("modules.json"):
        graph = modules_path.parent
        try:
            modules = read_json(modules_path)
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(modules, dict):
            continue
        declared = {
            endpoint_id(model, int(instance))
            for model, instances in modules.items()
            if isinstance(instances, dict)
            for instance in instances
        }
        connections_path = graph / "connections.json"
        if not connections_path.exists():
            continue
        try:
            edges = flatten_connections(read_json(connections_path))
        except (OSError, json.JSONDecodeError):
            continue
        relative = str(graph.relative_to(preset)) or "."
        for source_id, _, destination_id, _, _ in edges:
            if source_id not in declared:
                invalid.add(f"{relative}:source:{source_id}")
            if destination_id not in declared:
                invalid.add(f"{relative}:destination:{destination_id}")
    return invalid


def validate_preset(preset: Path, allowed_invalid: set[str]) -> None:
    for path in preset.rglob("*.json"):
        read_json(path)
    unexpected = invalid_connection_endpoints(preset) - allowed_invalid
    if unexpected:
        raise ValueError("new undeclared connection endpoint(s): " + ", ".join(sorted(unexpected)))


def report_to_json(report: RunReport) -> dict[str, Any]:
    return {
        "source": report.source,
        "destination": report.destination,
        "dry_run": report.dry_run,
        "presets": [
            {
                **{key: value for key, value in asdict(preset).items() if key != "issues"},
                "issues": [asdict(issue) for issue in preset.issues],
            }
            for preset in report.presets
        ],
        "synthdefs": [asdict(requirement) for requirement in report.synthdefs],
    }


def print_summary(report: RunReport) -> None:
    converted = sum(preset.converted_nodes for preset in report.presets)
    remaining = sum(preset.remaining_legacy_nodes for preset in report.presets)
    complete = sum(1 for preset in report.presets if preset.complete)
    print(
        f"Presets: {len(report.presets)} ({complete} complete); "
        f"converted nodes: {converted}; remaining legacy nodes: {remaining}"
    )
    statuses: dict[str, int] = {}
    for requirement in report.synthdefs:
        statuses[requirement.status] = statuses.get(requirement.status, 0) + 1
    if statuses:
        print("SynthDefs: " + ", ".join(f"{key}={value}" for key, value in sorted(statuses.items())))
    for preset in report.presets:
        if preset.issues:
            print(f"\n{preset.preset}: {len(preset.issues)} issue(s)")
            for issue in preset.issues[:20]:
                location = f"{issue.graph}/{issue.node}".strip("/")
                print(f"  - {location}: {issue.legacy_model}: {issue.reason}")
            if len(preset.issues) > 20:
                print(f"  - ... {len(preset.issues) - 20} more; see conversion_report.json")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert fixed-channel Oceanode SynthDef presets to dynamic SynthDefs."
    )
    parser.add_argument("--gui", action="store_true", help="Open a graphical folder/preset picker")
    parser.add_argument(
        "source", nargs="?", type=Path, default=Path.cwd(),
        help="Preset directory or folder containing presets (default: current directory)",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--preset", help="Convert one named preset below SOURCE, or an explicit path")
    mode.add_argument("--all", action="store_true", help="Convert all immediate preset children of SOURCE")
    parser.add_argument("--output", type=Path, help="Output directory (default: SOURCE/UPGRADED_PRESETS)")
    parser.add_argument("--dry-run", action="store_true", help="Analyze without copying or compiling")
    parser.add_argument("--overwrite", action="store_true", help="Replace an existing upgraded copy")
    parser.add_argument("--no-compile", action="store_true", help="Do not compile missing SynthDef variants")
    parser.add_argument(
        "--require-complete", action="store_true",
        help="Return exit status 2 if any legacy nodes remain or any issues are reported",
    )
    parser.add_argument("--legacy-registry", type=Path, default=DEFAULT_LEGACY_REGISTRY)
    parser.add_argument("--synthdefs", type=Path, default=DEFAULT_SYNTHDEFS)
    parser.add_argument("--newest", type=Path, default=DEFAULT_NEWEST)
    parser.add_argument("--macros", type=Path, default=DEFAULT_MACROS)
    parser.add_argument("--sclang", type=Path, default=DEFAULT_SCLANG)
    return parser


def normalize_args(args: argparse.Namespace) -> argparse.Namespace:
    args.source = args.source.expanduser().resolve()
    if args.output:
        args.output = args.output.expanduser().resolve()
    elif is_preset(args.source) and not args.all and not args.preset:
        args.output = args.source.parent / "UPGRADED_PRESETS"
    else:
        args.output = args.source / "UPGRADED_PRESETS"
    for name in ("legacy_registry", "synthdefs", "newest", "macros", "sclang"):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    return args


class _QueueWriter:
    def __init__(self, output_queue: "queue.SimpleQueue[str]"):
        self.output_queue = output_queue

    def write(self, text: str) -> int:
        if text:
            self.output_queue.put(text)
        return len(text)

    def flush(self) -> None:
        pass


def _args_from_gui(values: dict[str, Any]) -> argparse.Namespace:
    source = Path(values["source"]).expanduser()
    output = values.get("output", "").strip()
    return argparse.Namespace(
        gui=False,
        source=source,
        preset=None,
        all=values["mode"] == "folder",
        output=Path(output).expanduser() if output else None,
        dry_run=values["dry_run"],
        overwrite=values["overwrite"],
        no_compile=values["no_compile"],
        require_complete=values["require_complete"],
        legacy_registry=Path(values["legacy_registry"]).expanduser(),
        synthdefs=Path(values["synthdefs"]).expanduser(),
        newest=Path(values["newest"]).expanduser(),
        macros=Path(values["macros"]).expanduser(),
        sclang=Path(values["sclang"]).expanduser(),
    )


def run_gui(initial_args: argparse.Namespace) -> int:
    # On current macOS installs, Apple/CommandLineTools Tk can abort in native
    # code before Python can catch it (for example: "macOS 15 (1507) or later
    # required" or Tcl_Panic crashes). Avoid importing Tk by default on macOS.
    # Power users can still force the Tk window with OCEANODE_USE_TK=1.
    if sys.platform == "darwin" and os.environ.get("OCEANODE_USE_TK") != "1":
        return run_macos_dialog_gui(initial_args, "Using macOS native dialogs")

    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, scrolledtext
    except Exception as error:
        return run_macos_dialog_gui(initial_args, f"tkinter GUI is unavailable: {error}")

    root = tk.Tk()
    root.title("Oceanode Preset SynthDef Upgrader")
    root.geometry("980x720")

    mode_var = tk.StringVar(value="preset")
    source_var = tk.StringVar(value=str(initial_args.source.expanduser()))
    output_var = tk.StringVar(value=str(initial_args.output.expanduser()) if initial_args.output else "")
    legacy_var = tk.StringVar(value=str(initial_args.legacy_registry))
    synthdefs_var = tk.StringVar(value=str(initial_args.synthdefs))
    newest_var = tk.StringVar(value=str(initial_args.newest))
    macros_var = tk.StringVar(value=str(initial_args.macros))
    sclang_var = tk.StringVar(value=str(initial_args.sclang))
    dry_run_var = tk.BooleanVar(value=bool(initial_args.dry_run))
    overwrite_var = tk.BooleanVar(value=bool(initial_args.overwrite))
    no_compile_var = tk.BooleanVar(value=bool(initial_args.no_compile))
    require_complete_var = tk.BooleanVar(value=bool(initial_args.require_complete))
    running = {"value": False}
    messages: "queue.SimpleQueue[str]" = queue.SimpleQueue()

    def choose_directory(var: tk.StringVar, title: str) -> None:
        selected = filedialog.askdirectory(title=title, initialdir=str(Path(var.get() or ".").expanduser().parent))
        if selected:
            var.set(selected)

    def choose_file(var: tk.StringVar, title: str, filetypes: list[tuple[str, str]]) -> None:
        selected = filedialog.askopenfilename(
            title=title,
            initialdir=str(Path(var.get() or ".").expanduser().parent),
            filetypes=filetypes,
        )
        if selected:
            var.set(selected)

    outer = tk.Frame(root, padx=14, pady=14)
    outer.pack(fill="both", expand=True)

    mode_frame = tk.LabelFrame(outer, text="Conversion mode", padx=10, pady=8)
    mode_frame.pack(fill="x")
    tk.Radiobutton(mode_frame, text="Single preset folder", variable=mode_var, value="preset").pack(side="left")
    tk.Radiobutton(mode_frame, text="All presets inside source folder", variable=mode_var, value="folder").pack(side="left", padx=(20, 0))

    form = tk.Frame(outer)
    form.pack(fill="x", pady=(10, 0))

    def add_path_row(row: int, label: str, var: tk.StringVar, browse: Callable[[], None]) -> None:
        tk.Label(form, text=label, anchor="w").grid(row=row, column=0, sticky="w", pady=3)
        tk.Entry(form, textvariable=var).grid(row=row, column=1, sticky="ew", padx=8, pady=3)
        tk.Button(form, text="Browse…", command=browse).grid(row=row, column=2, sticky="ew", pady=3)

    form.columnconfigure(1, weight=1)
    add_path_row(0, "Source preset/folder", source_var, lambda: choose_directory(source_var, "Choose preset or presets folder"))
    add_path_row(1, "Output folder (optional)", output_var, lambda: choose_directory(output_var, "Choose output folder"))
    add_path_row(2, "Synthdefs install folder", synthdefs_var, lambda: choose_directory(synthdefs_var, "Choose Supercollider/Synthdefs folder"))
    add_path_row(3, "New .scd catalog folder", newest_var, lambda: choose_directory(newest_var, "Choose NEWEST folder"))
    add_path_row(4, "Audio macro templates folder", macros_var, lambda: choose_directory(macros_var, "Choose macro templates folder"))
    add_path_row(
        5,
        "Legacy Synthdefs.json",
        legacy_var,
        lambda: choose_file(legacy_var, "Choose legacy Synthdefs.json", [("JSON files", "*.json"), ("All files", "*")]),
    )
    add_path_row(
        6,
        "sclang executable",
        sclang_var,
        lambda: choose_file(sclang_var, "Choose sclang executable", [("Executables", "*"), ("All files", "*")]),
    )

    options = tk.LabelFrame(outer, text="Options", padx=10, pady=8)
    options.pack(fill="x", pady=(10, 0))
    tk.Checkbutton(options, text="Dry run", variable=dry_run_var).pack(side="left")
    tk.Checkbutton(options, text="Overwrite existing upgraded copies", variable=overwrite_var).pack(side="left", padx=(16, 0))
    tk.Checkbutton(options, text="Do not compile missing SynthDefs", variable=no_compile_var).pack(side="left", padx=(16, 0))
    tk.Checkbutton(options, text="Require complete conversion", variable=require_complete_var).pack(side="left", padx=(16, 0))

    buttons = tk.Frame(outer)
    buttons.pack(fill="x", pady=(10, 0))
    status_var = tk.StringVar(value="Ready")
    tk.Label(buttons, textvariable=status_var, anchor="w").pack(side="left", fill="x", expand=True)
    run_button = tk.Button(buttons, text="Convert")
    run_button.pack(side="right")

    log = scrolledtext.ScrolledText(outer, height=22, wrap="word")
    log.pack(fill="both", expand=True, pady=(10, 0))

    def append_log(text: str) -> None:
        log.configure(state="normal")
        log.insert("end", text)
        log.see("end")
        log.configure(state="disabled")

    def poll_messages() -> None:
        while True:
            try:
                text = messages.get_nowait()
            except queue.Empty:
                break
            append_log(text)
        if running["value"]:
            root.after(100, poll_messages)

    def collect_values() -> dict[str, Any]:
        return {
            "mode": mode_var.get(),
            "source": source_var.get().strip(),
            "output": output_var.get().strip(),
            "legacy_registry": legacy_var.get().strip(),
            "synthdefs": synthdefs_var.get().strip(),
            "newest": newest_var.get().strip(),
            "macros": macros_var.get().strip(),
            "sclang": sclang_var.get().strip(),
            "dry_run": dry_run_var.get(),
            "overwrite": overwrite_var.get(),
            "no_compile": no_compile_var.get(),
            "require_complete": require_complete_var.get(),
        }

    def worker(values: dict[str, Any]) -> None:
        exit_code = 1
        writer = _QueueWriter(messages)
        try:
            with redirect_stdout(writer), redirect_stderr(writer):
                args = normalize_args(_args_from_gui(values))
                print("Running Oceanode preset upgrade")
                print(f"Source: {args.source}")
                print(f"Output: {args.output}")
                print(f"SynthDefs: {args.synthdefs}")
                print()
                exit_code = PresetConverter(args).run()
        except Exception:
            writer.write(traceback.format_exc())
            exit_code = 1
        finally:
            messages.put(f"\nFinished with exit code {exit_code}.\n")
            def done() -> None:
                running["value"] = False
                run_button.configure(state="normal")
                status_var.set("Complete" if exit_code == 0 else f"Finished with issues (exit {exit_code})")
                poll_messages()
            root.after(0, done)

    def start_conversion() -> None:
        if running["value"]:
            return
        values = collect_values()
        required = ["source", "legacy_registry", "synthdefs", "newest", "macros", "sclang"]
        missing = [label for label in required if not values[label]]
        if missing:
            messagebox.showerror("Missing path", "Please fill: " + ", ".join(missing))
            return
        log.configure(state="normal")
        log.delete("1.0", "end")
        log.configure(state="disabled")
        running["value"] = True
        run_button.configure(state="disabled")
        status_var.set("Running…")
        threading.Thread(target=worker, args=(values,), daemon=True).start()
        poll_messages()

    run_button.configure(command=start_conversion)
    root.mainloop()
    return 0


class GuiCancelled(Exception):
    pass


def applescript_quote(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def run_osascript(script: str) -> str:
    process = subprocess.run(
        ["osascript", "-e", script],
        capture_output=True,
        text=True,
        check=False,
    )
    if process.returncode != 0:
        message = (process.stderr or process.stdout or "").strip()
        if str(process.returncode) == "1" and ("-128" in message or "User canceled" in message):
            raise GuiCancelled
        raise RuntimeError(message or f"osascript exited with {process.returncode}")
    return process.stdout.strip()


def display_macos_message(title: str, message: str) -> None:
    try:
        run_osascript(
            "display dialog "
            + applescript_quote(message)
            + " with title "
            + applescript_quote(title)
            + ' buttons {"OK"} default button "OK"'
        )
    except Exception:
        pass


def choose_macos_folder(prompt: str, default_path: Path) -> str:
    default_dir = default_path if default_path.exists() and default_path.is_dir() else Path.home()
    script = (
        "set defaultLocation to POSIX file "
        + applescript_quote(str(default_dir))
        + "\nreturn POSIX path of (choose folder with prompt "
        + applescript_quote(prompt)
        + " default location defaultLocation)"
    )
    return run_osascript(script)


def choose_macos_file(prompt: str, default_path: Path) -> str:
    default_dir = default_path.parent if default_path.parent.exists() else Path.home()
    script = (
        "set defaultLocation to POSIX file "
        + applescript_quote(str(default_dir))
        + "\nreturn POSIX path of (choose file with prompt "
        + applescript_quote(prompt)
        + " default location defaultLocation)"
    )
    return run_osascript(script)


def choose_macos_button(prompt: str, buttons: list[str], default: str, title: str = "Oceanode Preset Upgrader") -> str:
    button_list = "{" + ", ".join(applescript_quote(button) for button in buttons) + "}"
    script = (
        "button returned of (display dialog "
        + applescript_quote(prompt)
        + " with title "
        + applescript_quote(title)
        + " buttons "
        + button_list
        + " default button "
        + applescript_quote(default)
        + ")"
    )
    return run_osascript(script)


def run_macos_dialog_gui(initial_args: argparse.Namespace, fallback_reason: str = "") -> int:
    """Small native macOS dialog flow used when tkinter is unavailable.

    It intentionally streams the real converter log to the launching terminal.
    The graphical part is for selecting paths/options; the CLI engine remains
    the one source of truth.
    """
    if sys.platform != "darwin":
        raise RuntimeError(fallback_reason)
    if fallback_reason:
        print(f"{fallback_reason}; using macOS native dialogs instead.")
    try:
        mode_button = choose_macos_button(
            "What do you want to convert?",
            ["Cancel", "Single preset", "Preset folder"],
            "Single preset",
        )
        if mode_button == "Cancel":
            return 0
        mode = "folder" if mode_button == "Preset folder" else "preset"
        source_prompt = (
            "Choose the folder containing presets"
            if mode == "folder"
            else "Choose one Oceanode preset folder"
        )
        source = choose_macos_folder(source_prompt, initial_args.source.expanduser())
        output_button = choose_macos_button(
            "Where should upgraded preset copies be written?",
            ["Default", "Choose output folder"],
            "Default",
        )
        output = ""
        if output_button == "Choose output folder":
            output = choose_macos_folder("Choose output folder", initial_args.source.expanduser())

        synthdefs = choose_macos_folder("Choose Supercollider/Synthdefs install folder", initial_args.synthdefs)
        newest = choose_macos_folder("Choose NEWEST catalog folder", initial_args.newest)
        legacy_registry = choose_macos_file("Choose legacy Synthdefs.json", initial_args.legacy_registry)
        macros = choose_macos_folder("Choose AUDIO/EFFECTS macro templates folder", initial_args.macros)
        sclang = choose_macos_file("Choose sclang executable", initial_args.sclang)

        option_mode = choose_macos_button(
            "Use safe default options?\n\nDefaults: real conversion, compile missing SynthDefs, no overwrite, do not require complete.",
            ["Safe defaults", "Customize"],
            "Safe defaults",
        )
        dry_run = False
        overwrite = False
        no_compile = False
        require_complete = False
        if option_mode == "Customize":
            dry_run = choose_macos_button("Dry run only?", ["No", "Yes"], "No") == "Yes"
            overwrite = choose_macos_button("Overwrite existing upgraded copies?", ["No", "Yes"], "No") == "Yes"
            no_compile = choose_macos_button("Skip compiling missing SynthDefs?", ["No", "Yes"], "No") == "Yes"
            require_complete = choose_macos_button("Fail if any legacy nodes remain?", ["No", "Yes"], "No") == "Yes"

        values = {
            "mode": mode,
            "source": source,
            "output": output,
            "legacy_registry": legacy_registry,
            "synthdefs": synthdefs,
            "newest": newest,
            "macros": macros,
            "sclang": sclang,
            "dry_run": dry_run,
            "overwrite": overwrite,
            "no_compile": no_compile,
            "require_complete": require_complete,
        }
        args = normalize_args(_args_from_gui(values))
        print("Running Oceanode preset upgrade")
        print(f"Source: {args.source}")
        print(f"Output: {args.output}")
        print(f"SynthDefs: {args.synthdefs}")
        print()
        exit_code = PresetConverter(args).run()
        display_macos_message(
            "Oceanode Preset Upgrader",
            "Conversion finished successfully." if exit_code == 0 else f"Conversion finished with exit code {exit_code}. See Terminal for details.",
        )
        return exit_code
    except GuiCancelled:
        print("GUI conversion canceled.")
        return 0
    except Exception as error:
        display_macos_message("Oceanode Preset Upgrader Error", str(error))
        raise


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.gui:
        try:
            return run_gui(args)
        except RuntimeError as error:
            parser.error(str(error))
    args = normalize_args(args)
    try:
        return PresetConverter(args).run()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
