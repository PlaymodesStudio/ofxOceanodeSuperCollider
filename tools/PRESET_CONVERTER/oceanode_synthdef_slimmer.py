#!/usr/bin/env python3
"""Analyze Oceanode presets and quarantine unused compiled SynthDefs.

This tool scans Oceanode preset folders, determines the exact SC SynthDef
binaries referenced by their `SC ...` nodes, and optionally moves non-used
`.scsyndef` files (and fully-unused metadata files) out of the live Synthdefs
folder into a quarantine folder.

The default mode is a dry analysis. Use `--quarantine` to actually move files.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

try:
    from oceanode_preset_converter import (
        DEFAULT_APP_DATA,
        DEFAULT_NEWEST,
        DEFAULT_SYNTHDEFS,
        SourceCatalog,
        endpoint_id,
        node_filename,
        normalize,
        read_json,
    )
except Exception as import_error:  # pragma: no cover - user-facing fallback
    raise SystemExit(f"Could not import oceanode_preset_converter.py from this folder: {import_error}")

SC_PREFIX = "SC "
SCSYNDEF_SUFFIX = ".scsyndef"
META_SUFFIX = ".txarcmeta"
IGNORED_DIR_NAMES = {".git", "__pycache__"}


# Some Oceanode nodes are saved as "SC Foo" but create lower-level SynthDefs
# with names chosen in C++ rather than by the visible preset model name.
SPECIAL_FIXED_EXACT_BY_CHANNEL = {
    normalize("VelvetReverb"): ("scvelvetreverb", "N_Chan"),
    normalize("VUMeter"): ("vumeter", "Num_Channels"),
    normalize("Pink Trombone"): ("RadioStation", "N_Chan"),
    normalize("Radio Station VLC"): ("RadioStation", "N_Chan"),
    normalize("Spectrogram"): ("fftanalyzerHD", "N_Chan"),
    normalize("PolyMixerTrack"): ("polyMixerTrack", "Num_Channels"),
    normalize("Perlin"): ("Perlin", "N_Chan"),
    normalize("FFT HD"): ("fftanalyzerHD", "N_Chan"),
    normalize("Fast Envelope Follower"): ("vumeter", "Num_Channels"),
    normalize("GrainSamplerGui"): ("GrainSamplerGui", "N_Chan"),
    normalize("MultiTrack Rec"): ("multirecbuf", "N_Chan"),
}

SPECIAL_FIXED_KEEP_ALL = {
    normalize("Info"): "Info",
    normalize("RAVE Loader"): "rave",
}

SPECIAL_FIXED_EXTRA_EXACT = {
    normalize("Rhythm Box"): ["RhythmBoxTrack", "BufferBrowserPreview", "SlicePreview"],
    normalize("Buffer Allocator"): ["bufalloc"],
    normalize("Buffer Browser"): ["BufferBrowserPreview"],
    normalize("VSTI"): ["vstMono", "vstStereo", "vst"],
}

# These are Oceanode-side helper/control nodes whose saved model name begins
# with SC but which do not correspond to a compiled .scsyndef file by the same
# name. They should not produce missing-synth warnings.
NON_SYNTH_SC_MODELS = {
    normalize("Buffer"),
    normalize("Custom Buffer"),
    normalize("Double Buffer"),
    normalize("Wavetable Buffer"),
    normalize("Chord"),
    normalize("Pitch"),
}


@dataclass
class NeededNode:
    preset: str
    graph: str
    model: str
    instance: str
    reason: str
    exact_stem: Optional[str] = None
    model_base: Optional[str] = None
    keep_all_for_model: bool = False


@dataclass
class InstalledFile:
    path: Path
    relative: str
    kind: str
    stem: str
    normalized_stem: str
    parent_model: str
    normalized_parent_model: str
    size: int


@dataclass
class SlimReport:
    presets_root: str
    synthdefs_root: str
    newest_root: str
    quarantine_dir: str
    dry_run: bool
    preset_count: int = 0
    graph_count: int = 0
    sc_node_count: int = 0
    needed_exact_stems: list[str] = field(default_factory=list)
    keep_all_models: list[str] = field(default_factory=list)
    missing_needed_stems: list[str] = field(default_factory=list)
    unresolved_nodes: list[dict[str, Any]] = field(default_factory=list)
    kept_files: list[str] = field(default_factory=list)
    quarantine_candidates: list[str] = field(default_factory=list)
    moved_files: list[str] = field(default_factory=list)
    skipped_files: list[str] = field(default_factory=list)
    bytes_to_quarantine: int = 0
    bytes_moved: int = 0
    manifest_path: Optional[str] = None


def parse_int(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        value = value.strip()
        if not value:
            return None
        try:
            return int(float(value))
        except ValueError:
            return None
    return None


def lookup_node_value(node_data: dict[str, Any], key: str) -> Any:
    if key in node_data:
        return node_data[key]
    wanted = normalize(key)
    for existing_key, value in node_data.items():
        if normalize(str(existing_key)) == wanted:
            return value
    return None


def display_name(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root)) or "."
    except ValueError:
        return str(path)


def is_inside(path: Path, possible_parent: Path) -> bool:
    try:
        path.relative_to(possible_parent)
        return True
    except ValueError:
        return False


def discover_preset_roots(source: Path) -> list[Path]:
    source = source.expanduser().resolve()
    if not source.exists():
        return []
    module_paths = sorted(source.rglob("modules.json"), key=lambda p: (len(p.parts), str(p)))
    roots: list[Path] = []
    for modules_path in module_paths:
        candidate = modules_path.parent
        if any(is_inside(candidate, root) for root in roots):
            continue
        roots.append(candidate)
    return roots


def iter_graph_dirs(preset: Path) -> list[Path]:
    return sorted(path.parent for path in preset.rglob("modules.json"))


def read_node_json(graph: Path, model: str, instance: str) -> dict[str, Any]:
    try:
        numeric_instance = int(instance)
    except ValueError:
        numeric_instance = int(str(instance).lstrip("0") or "0")
    path = graph / node_filename(model, numeric_instance)
    try:
        data = read_json(path)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def sc_model_name(model: str) -> Optional[tuple[str, bool]]:
    if not model.startswith(SC_PREFIX):
        return None
    name = model[len(SC_PREFIX):]
    dynamic = name.endswith("*")
    if dynamic:
        name = name[:-1]
    return name, dynamic


def needed_for_node(
    preset: Path,
    graph: Path,
    model: str,
    instance: str,
    catalog: SourceCatalog,
) -> list[NeededNode]:
    parsed = sc_model_name(model)
    assert parsed is not None
    name, dynamic = parsed
    preset_label = preset.name
    graph_label = display_name(graph, preset)
    node_data = read_node_json(graph, model, instance)
    normalized_name = normalize(name)

    def node(**kwargs: Any) -> NeededNode:
        return NeededNode(preset=preset_label, graph=graph_label, model=model, instance=instance, **kwargs)

    if not dynamic:
        if normalized_name in NON_SYNTH_SC_MODELS:
            return [node(model_base=name, reason="Oceanode helper/control node; no direct SynthDef required")]

        result: list[NeededNode] = []
        if normalized_name in SPECIAL_FIXED_KEEP_ALL:
            keep_model = SPECIAL_FIXED_KEEP_ALL[normalized_name]
            result.append(node(model_base=keep_model, keep_all_for_model=True, reason=f"special node keeps all installed {keep_model} variants"))

        if normalized_name in SPECIAL_FIXED_EXACT_BY_CHANNEL:
            prefix, channel_key = SPECIAL_FIXED_EXACT_BY_CHANNEL[normalized_name]
            channel_count = parse_int(lookup_node_value(node_data, channel_key))
            if channel_count is None:
                result.append(node(model_base=prefix, keep_all_for_model=True, reason=f"special node has no readable {channel_key}; keeping all {prefix} variants"))
            else:
                result.append(node(exact_stem=f"{prefix}{channel_count}", model_base=prefix, reason=f"special node uses {prefix}{channel_count}.scsyndef"))

        for exact in SPECIAL_FIXED_EXTRA_EXACT.get(normalized_name, []):
            result.append(node(exact_stem=exact, model_base=exact, reason=f"special node helper SynthDef {exact}.scsyndef"))

        if result:
            return result

        return [node(
            exact_stem=name,
            model_base=name,
            reason=f"fixed SynthDef {name}",
        )]

    spec = catalog.get(name)
    if spec is None:
        return [node(
            model_base=name,
            keep_all_for_model=True,
            reason="dynamic model is not in the .scd catalog; keeping all installed variants for safety",
        )]

    num_channels = parse_int(lookup_node_value(node_data, "N_Chan"))
    if num_channels is None:
        return [node(
            model_base=spec.name,
            keep_all_for_model=True,
            reason="dynamic node has no readable N_Chan; keeping all installed variants for safety",
        )]

    variable_values: list[int] = []
    missing_variables: list[str] = []
    for variable in spec.variables:
        value = parse_int(lookup_node_value(node_data, variable))
        if value is None:
            missing_variables.append(variable)
        else:
            variable_values.append(value)
    if missing_variables:
        return [node(
            model_base=spec.name,
            keep_all_for_model=True,
            reason="dynamic node is missing variable value(s) " + ", ".join(missing_variables) + "; keeping all installed variants for safety",
        )]

    expected = spec.expected_filename(num_channels, variable_values)
    return [node(
        exact_stem=expected[:-len(SCSYNDEF_SUFFIX)],
        model_base=spec.name,
        reason=f"dynamic SynthDef {expected}",
    )]

def collect_needed(source: Path, catalog: SourceCatalog) -> tuple[list[Path], list[NeededNode], int]:
    preset_roots = discover_preset_roots(source)
    needed: list[NeededNode] = []
    graph_count = 0
    for preset in preset_roots:
        for graph in iter_graph_dirs(preset):
            graph_count += 1
            try:
                modules = read_json(graph / "modules.json")
            except Exception:
                continue
            if not isinstance(modules, dict):
                continue
            for model, instances in modules.items():
                parsed = sc_model_name(model)
                if parsed is None or not isinstance(instances, dict):
                    continue
                for instance in instances:
                    needed.extend(needed_for_node(preset, graph, model, str(instance), catalog))
    return preset_roots, needed, graph_count


def scan_installed_files(synthdefs_root: Path) -> list[InstalledFile]:
    files: list[InstalledFile] = []
    for path in sorted(synthdefs_root.rglob("*")):
        if not path.is_file():
            continue
        if any(part in IGNORED_DIR_NAMES for part in path.parts):
            continue
        if path.name.startswith("."):
            continue
        if path.suffix not in {SCSYNDEF_SUFFIX, META_SUFFIX}:
            continue
        try:
            size = path.stat().st_size
        except OSError:
            size = 0
        files.append(
            InstalledFile(
                path=path,
                relative=str(path.relative_to(synthdefs_root)),
                kind="synthdef" if path.suffix == SCSYNDEF_SUFFIX else "metadata",
                stem=path.stem,
                normalized_stem=normalize(path.stem),
                parent_model=path.parent.name,
                normalized_parent_model=normalize(path.parent.name),
                size=size,
            )
        )
    return files


def belongs_to_keep_all_model(file: InstalledFile, keep_all_models_normalized: set[str]) -> bool:
    if file.normalized_parent_model in keep_all_models_normalized:
        return True
    for model in keep_all_models_normalized:
        if file.normalized_stem.startswith(model):
            suffix = file.normalized_stem[len(model):]
            if suffix == "" or suffix[0].isdigit():
                return True
    return False


def choose_files(
    installed: list[InstalledFile],
    needed: list[NeededNode],
) -> tuple[list[InstalledFile], list[InstalledFile], list[str], set[str], set[str]]:
    needed_exact = {node.exact_stem for node in needed if node.exact_stem}
    needed_exact_normalized = {normalize(stem) for stem in needed_exact}
    needed_models_normalized = {normalize(node.model_base) for node in needed if node.model_base}
    keep_all_models_normalized = {normalize(node.model_base) for node in needed if node.keep_all_for_model and node.model_base}

    installed_synth_stems = {file.normalized_stem for file in installed if file.kind == "synthdef"}
    installed_parent_models = {file.normalized_parent_model for file in installed if file.kind == "synthdef"}
    # If a fixed node is saved as "SC Info" but installed files are Info1,
    # Info2, etc., keep that whole model family instead of reporting Info as
    # missing and quarantining its real numbered variants.
    for stem in list(needed_exact):
        normalized_stem = normalize(stem)
        if normalized_stem in installed_synth_stems:
            continue
        has_numbered_family = normalized_stem in installed_parent_models or any(
            file.kind == "synthdef"
            and file.normalized_stem.startswith(normalized_stem)
            and file.normalized_stem[len(normalized_stem):len(normalized_stem) + 1].isdigit()
            for file in installed
        )
        if has_numbered_family:
            keep_all_models_normalized.add(normalized_stem)

    kept: list[InstalledFile] = []
    candidates: list[InstalledFile] = []
    kept_parent_dirs: set[Path] = set()

    for file in installed:
        keep = False
        if file.kind == "synthdef":
            keep = file.normalized_stem in needed_exact_normalized or belongs_to_keep_all_model(file, keep_all_models_normalized)
            if keep:
                kept_parent_dirs.add(file.path.parent)
        if keep:
            kept.append(file)

    for file in installed:
        if file.kind == "metadata":
            keep_metadata = (
                file.path.parent in kept_parent_dirs
                or file.normalized_stem in needed_models_normalized
                or file.normalized_parent_model in keep_all_models_normalized
            )
            if keep_metadata:
                kept.append(file)
            else:
                candidates.append(file)
        elif file.kind == "synthdef":
            if file not in kept:
                candidates.append(file)

    missing = sorted(
        stem for stem in needed_exact
        if normalize(stem) not in installed_synth_stems
        and normalize(stem) not in keep_all_models_normalized
    )
    return kept, candidates, missing, needed_exact_normalized, keep_all_models_normalized


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")


def make_default_quarantine_dir(synthdefs_root: Path) -> Path:
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    return synthdefs_root.parent / "Synthdefs_QUARANTINE" / timestamp


def move_candidates(candidates: Iterable[InstalledFile], synthdefs_root: Path, quarantine_dir: Path) -> tuple[list[str], int]:
    moved: list[str] = []
    bytes_moved = 0
    synthdefs_root = synthdefs_root.resolve()
    quarantine_dir = quarantine_dir.expanduser().resolve()
    if is_inside(quarantine_dir, synthdefs_root):
        raise ValueError("Quarantine folder must be outside the Synthdefs folder")
    quarantine_dir.mkdir(parents=True, exist_ok=True)
    for file in candidates:
        destination = quarantine_dir / file.relative
        if destination.exists():
            raise FileExistsError(f"Quarantine destination already exists: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(file.path), str(destination))
        moved.append(file.relative)
        bytes_moved += file.size
    return moved, bytes_moved


def prune_empty_dirs(root: Path) -> int:
    removed = 0
    for directory in sorted((p for p in root.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        try:
            directory.rmdir()
            removed += 1
        except OSError:
            pass
    return removed


def build_report(args: argparse.Namespace) -> tuple[SlimReport, list[InstalledFile]]:
    presets_root = args.source.expanduser().resolve()
    synthdefs_root = args.synthdefs.expanduser().resolve()
    newest_root = args.newest.expanduser().resolve()
    quarantine_dir = args.quarantine_dir.expanduser().resolve() if args.quarantine_dir else make_default_quarantine_dir(synthdefs_root)

    if not synthdefs_root.exists() or not synthdefs_root.is_dir():
        raise ValueError(f"Synthdefs folder does not exist: {synthdefs_root}")
    if not presets_root.exists() or not presets_root.is_dir():
        raise ValueError(f"Presets folder does not exist: {presets_root}")

    catalog = SourceCatalog(newest_root, synthdefs_root)
    presets, needed, graph_count = collect_needed(presets_root, catalog)
    installed = scan_installed_files(synthdefs_root)
    kept, candidates, missing, needed_exact_norm, keep_all_norm = choose_files(installed, needed)

    report = SlimReport(
        presets_root=str(presets_root),
        synthdefs_root=str(synthdefs_root),
        newest_root=str(newest_root),
        quarantine_dir=str(quarantine_dir),
        dry_run=not args.quarantine,
        preset_count=len(presets),
        graph_count=graph_count,
        sc_node_count=len(needed),
        needed_exact_stems=sorted({node.exact_stem for node in needed if node.exact_stem}),
        keep_all_models=sorted({node.model_base for node in needed if node.keep_all_for_model and node.model_base}),
        missing_needed_stems=missing,
        unresolved_nodes=[asdict(node) for node in needed if node.keep_all_for_model],
        kept_files=sorted(file.relative for file in kept),
        quarantine_candidates=sorted(file.relative for file in candidates),
        bytes_to_quarantine=sum(file.size for file in candidates),
    )
    return report, candidates


def print_summary(report: SlimReport) -> None:
    action = "would quarantine" if report.dry_run else "quarantined"
    bytes_mb = report.bytes_to_quarantine / (1024 * 1024)
    print(f"Presets scanned: {report.preset_count}; graphs: {report.graph_count}; SC nodes: {report.sc_node_count}")
    print(f"Needed exact SynthDef variants: {len(report.needed_exact_stems)}")
    if report.keep_all_models:
        print(f"Models kept conservatively: {len(report.keep_all_models)} ({', '.join(report.keep_all_models[:12])}{'…' if len(report.keep_all_models) > 12 else ''})")
    print(f"Installed files kept: {len(report.kept_files)}")
    print(f"Unused files {action}: {len(report.quarantine_candidates)} ({bytes_mb:.2f} MB)")
    if report.missing_needed_stems:
        print(f"WARNING: {len(report.missing_needed_stems)} needed SynthDef variants were not found installed.")
        for stem in report.missing_needed_stems[:20]:
            print(f"  - missing: {stem}.scsyndef")
        if len(report.missing_needed_stems) > 20:
            print(f"  - ... {len(report.missing_needed_stems) - 20} more")
    if report.unresolved_nodes:
        print(f"NOTE: {len(report.unresolved_nodes)} node(s) were kept conservatively; their whole model folders were kept for safety.")
    print(f"Quarantine folder: {report.quarantine_dir}")
    if report.manifest_path:
        print(f"Manifest: {report.manifest_path}")


def run_cli(args: argparse.Namespace) -> int:
    report, candidates = build_report(args)
    quarantine_dir = Path(report.quarantine_dir)
    if args.quarantine:
        moved, bytes_moved = move_candidates(candidates, Path(report.synthdefs_root), quarantine_dir)
        report.moved_files = moved
        report.bytes_moved = bytes_moved
        if args.prune_empty_dirs:
            prune_empty_dirs(Path(report.synthdefs_root))
        manifest = quarantine_dir / "synthdef_quarantine_manifest.json"
        report.manifest_path = str(manifest)
        write_json(manifest, asdict(report))
    elif args.report:
        report_path = args.report.expanduser().resolve()
        report.manifest_path = str(report_path)
        write_json(report_path, asdict(report))
    print_summary(report)
    return 2 if report.missing_needed_stems and args.require_complete else 0


class GuiCancelled(Exception):
    pass


def applescript_quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def run_osascript(script: str) -> str:
    process = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, check=False)
    if process.returncode != 0:
        message = (process.stderr or process.stdout or "").strip()
        if "-128" in message or "User canceled" in message:
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


def choose_macos_button(prompt: str, buttons: list[str], default: str, title: str = "Oceanode SynthDef Slimmer") -> str:
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


def run_gui(initial_args: argparse.Namespace) -> int:
    if sys.platform != "darwin":
        raise RuntimeError("The graphical picker currently uses macOS native dialogs; use CLI on this platform.")
    try:
        source = Path(choose_macos_folder("Choose the presets folder to analyze", initial_args.source.expanduser()))
        synthdefs = Path(choose_macos_folder("Choose the live SuperCollider/Synthdefs folder", initial_args.synthdefs.expanduser()))
        newest = Path(choose_macos_folder("Choose the NEWEST synthdef source catalog folder", initial_args.newest.expanduser()))
        action = choose_macos_button(
            "What should I do?\n\nAnalyze only is safe and moves nothing. Quarantine will move unused .scsyndef files out of the live Synthdefs folder.",
            ["Analyze only", "Quarantine unused files"],
            "Analyze only",
        )
        quarantine = action == "Quarantine unused files"
        quarantine_dir: Optional[Path] = None
        if quarantine:
            location = choose_macos_button(
                "Where should unused files be quarantined?",
                ["Default sibling folder", "Choose folder"],
                "Default sibling folder",
            )
            if location == "Choose folder":
                quarantine_dir = Path(choose_macos_folder("Choose quarantine folder outside Synthdefs", synthdefs.parent))
        args = argparse.Namespace(
            source=source,
            synthdefs=synthdefs,
            newest=newest,
            quarantine=quarantine,
            quarantine_dir=quarantine_dir,
            report=None,
            require_complete=False,
            prune_empty_dirs=True,
        )
        exit_code = run_cli(args)
        message = "Analysis finished. See Terminal for the report."
        if quarantine:
            message = "Quarantine finished. See Terminal and the manifest in the quarantine folder."
        display_macos_message("Oceanode SynthDef Slimmer", message)
        return exit_code
    except GuiCancelled:
        print("GUI operation canceled.")
        return 0
    except Exception as error:
        display_macos_message("Oceanode SynthDef Slimmer Error", str(error))
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze Oceanode presets and quarantine unused compiled SynthDefs.")
    parser.add_argument("source", nargs="?", type=Path, default=DEFAULT_APP_DATA / "Presets", help="Preset folder/bank/root to scan recursively")
    parser.add_argument("--gui", action="store_true", help="Open native macOS folder/action picker")
    parser.add_argument("--synthdefs", type=Path, default=DEFAULT_SYNTHDEFS, help="Live Supercollider/Synthdefs folder")
    parser.add_argument("--newest", type=Path, default=DEFAULT_NEWEST, help="NEWEST folder used to resolve dynamic SynthDef variants")
    parser.add_argument("--quarantine", action="store_true", help="Move unused files to the quarantine folder. Without this, only analyze.")
    parser.add_argument("--quarantine-dir", type=Path, help="Destination folder outside Synthdefs (default: sibling Synthdefs_QUARANTINE/timestamp)")
    parser.add_argument("--report", type=Path, help="Write dry-run report JSON to this path")
    parser.add_argument("--require-complete", action="store_true", help="Return exit status 2 if any needed variants are missing")
    parser.add_argument("--no-prune-empty-dirs", dest="prune_empty_dirs", action="store_false", help="After quarantine, leave empty model/category folders in place")
    parser.set_defaults(prune_empty_dirs=True)
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.gui:
        return run_gui(args)
    try:
        return run_cli(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
