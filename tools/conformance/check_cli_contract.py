#!/usr/bin/env python3
"""Validate and exercise the cross-surface CLI JSON contract.

The manifest is deliberately independent of either CLI implementation.  That
lets a surface author land the inventory and semantic behavior in a separate
change while this checker already provides a stable, machine-readable target.
``--schema`` and ``--list`` never execute a CLI; the maintenance-only
``--emit-shared-option-snapshot`` mode executes only the two inventory dumps.
Invoking the script without executables is also a schema-only check.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

# The checker runs as a script and is also loaded by path from the conformance
# tests, so its own directory is not always on sys.path already.
sys.path.insert(0, str(Path(__file__).resolve().parent))

# `_validate_option` and `validate_payload` are not called here; the conformance
# self-tests reach every checker helper through this module, so both stay part of
# its surface.
from cli_contract_artifacts import _check_artifact  # noqa: E402
from cli_contract_fixtures import _resolve_argv, _write_fixtures  # noqa: E402
from cli_contract_inventory import (  # noqa: E402
    _accepted_names,
    _build_shared_option_snapshot,
    _compare_active_inventory_options,
    _expected_paths,
    _validate_inventory,
)
from cli_contract_payload import (  # noqa: E402
    _compare_payloads,
    _compare_values,
    _validate_analyze_case_relationships,
    _validate_case_payload,
    _validate_voice_case_relationships,
    parse_single_json,
    validate_payload,  # noqa: F401
)
from cli_contract_schema import (  # noqa: E402
    _normalized_option_inventory,
    _validate_option,  # noqa: F401
    validate_manifest,
)

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tests" / "conformance" / "cli_contract_v2.json"


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read manifest {path}: {exc}") from exc
    errors = validate_manifest(value)
    if errors:
        raise ValueError(
            "manifest schema validation failed:\n"
            + "\n".join(f"- {error}" for error in errors)
        )
    return value


def _resolve_executable(executable: str) -> str:
    """Resolve repository-relative executable paths before a surface changes cwd.

    Bare command names intentionally remain bare so callers can select an
    executable through PATH (for example ``python3`` or an installed
    ``sonare``).  A path such as ``bindings/python/.venv/bin/python`` instead
    denotes a repository artifact and must retain that meaning when the Python
    surface runs from ``bindings/python``.
    """
    separators = {os.sep}
    if os.altsep:
        separators.add(os.altsep)
    if any(separator in executable for separator in separators):
        # Do not resolve symlinks here. Virtual-environment launchers are often
        # symlinks whose *spelling* determines discovery of pyvenv.cfg; resolving
        # them would invoke the base interpreter and drop its dependencies.
        return (
            os.path.abspath(os.path.join(ROOT, executable))
            if not Path(executable).is_absolute()
            else executable
        )
    return executable


def _resolved_python_library(python_executable: str, timeout: float) -> str | None:
    """Ask the Python surface which shared library it will load.

    The search order (``SONARE_LIB_PATH``, then the build tree, then the
    package-adjacent copy) belongs to the surface, and it does not follow this
    checker's ``--native`` directory, so the answer is taken from the
    interpreter that is about to run rather than reconstructed from a path.
    Returns None when the surface cannot be asked, which leaves the comparison
    to proceed exactly as before.
    """
    if Path(python_executable).name.startswith("sonare"):
        return None
    cwd = ROOT / "bindings" / "python"
    environment = os.environ.copy()
    environment["PYTHONPATH"] = (
        str(cwd / "src") + os.pathsep + environment.get("PYTHONPATH", "")
    )
    try:
        completed = subprocess.run(
            [
                python_executable,
                "-c",
                "from libsonare._ffi import resolved_library_path;"
                "print(resolved_library_path())",
            ],
            cwd=str(cwd),
            env=environment,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip() or None


# What counts as a core source for the skew check below: a file that can change
# what either artifact links to. `src/` also holds the specification pages
# (`src/midi/synth/docs/*.md`), and taking every file made a docs-only edit
# report as artifact skew -- a failure no rebuild can clear, since nothing the
# build reads had changed.
_CORE_SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".h", ".hh", ".hpp", ".inc", ".ipp"}
)
_TRANSLATION_UNIT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".m", ".mm"})

# A suffix cannot express the rule above, because `src/wasm/` holds .cpp and .h
# files that only the WASM module compiles -- identical suffixes, linked into
# neither artifact under comparison. The compilation database says which
# translation units this build actually compiled, so the surface-specific trees
# are derived from the build rather than listed here.
_COMPILE_DB = ROOT / "build" / "compile_commands.json"


def _uncompiled_source_dirs() -> frozenset[Path]:
    """Directories under `src/` whose translation units this build never compiled.

    A directory qualifies only when it holds translation units and none of them
    appear in the compilation database, which is the signature of a tree built
    for another surface. Header-only directories are never excluded: nothing
    compiles them directly and the TUs that include them live elsewhere. An
    absent or unreadable database excludes nothing, leaving the skew check on
    every source rather than silently narrowing it.
    """
    try:
        with _COMPILE_DB.open(encoding="utf-8") as handle:
            entries = json.load(handle)
    except (OSError, ValueError):
        return frozenset()
    compiled: set[Path] = set()
    for entry in entries:
        try:
            compiled.add(Path(entry["file"]).resolve().parent)
        except (KeyError, TypeError, OSError):
            continue
    uncompiled: set[Path] = set()
    src_root = ROOT / "src"
    if not src_root.is_dir():
        return frozenset()
    for directory in {path.parent for path in src_root.rglob("*") if path.is_file()}:
        if directory in compiled:
            continue
        if any(
            child.suffix in _TRANSLATION_UNIT_SUFFIXES for child in directory.iterdir()
        ):
            uncompiled.add(directory)
    return frozenset(uncompiled)


def _is_core_source(path: Path, uncompiled_dirs: frozenset[Path] = frozenset()) -> bool:
    if not path.is_file():
        return False
    if path.parent in uncompiled_dirs:
        return False
    return path.suffix in _CORE_SOURCE_SUFFIXES or path.name == "CMakeLists.txt"


def _straddling_source(older: float, newer: float) -> Path | None:
    """Return a core source file last modified between two artifact link times."""
    uncompiled = _uncompiled_source_dirs()
    for directory in ("src", "include"):
        root = ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not _is_core_source(path, uncompiled):
                continue
            try:
                mtime = path.stat().st_mtime
            except OSError:
                continue
            if older < mtime <= newer:
                return path
    return None


def _count_sources_after(mtime: float) -> int:
    """Count core source files last modified after a given time."""
    count = 0
    uncompiled = _uncompiled_source_dirs()
    for directory in ("src", "include"):
        root = ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not _is_core_source(path, uncompiled):
                continue
            try:
                if path.stat().st_mtime > mtime:
                    count += 1
            except OSError:
                continue
    return count


def _check_artifact_skew(
    native_executable: str,
    python_executable: str,
    timeout: float,
    report: list[tuple[str, str]],
) -> None:
    """Name the compared artifacts, and reject two built from different cores.

    Every run prints which two artifacts were compared. Whether the Python
    surface loads the build tree, a ``SONARE_LIB_PATH`` override, or the
    package-adjacent copy is not visible from the invocation, so a passing
    contract run that does not say leaves the reader to assume it.

    The rejection is narrower than the announcement. The two surfaces are
    separate link products and the targets that refresh them are separate as
    well: ``build-shared`` rebuilds only the shared library, so a core edit
    followed by ``build-shared`` alone leaves the native CLI a generation
    behind, and comparing across that gap reports a difference belonging to the
    build rather than to either surface. Two equally stale artifacts still agree
    with each other, so staleness alone is reported and not rejected; what is
    rejected is a core source change landing between the two link times.
    """
    library = _resolved_python_library(python_executable, timeout)
    if library is None:
        return
    try:
        native_mtime = os.path.getmtime(native_executable)
        library_mtime = os.path.getmtime(library)
    except OSError:
        return
    provenance = f"cli contract v2: comparing {native_executable} and {library}"
    pending = _count_sources_after(max(native_mtime, library_mtime))
    if pending:
        provenance += f" ({pending} core source file(s) are newer)"
    print(provenance)
    straddling = _straddling_source(*sorted((native_mtime, library_mtime)))
    if straddling is None:
        return
    behind, ahead = (
        (native_executable, library)
        if native_mtime < library_mtime
        else (library, native_executable)
    )
    report.append(
        (
            "fail",
            f"artifact skew: {straddling.relative_to(ROOT)} changed after {behind} "
            f"was linked and before {ahead} was -- rebuild both before comparing",
        )
    )


def _run(
    surface: str, executable: str, argv: list[str], legacy: bool, timeout: float
) -> dict[str, Any]:
    environment = os.environ.copy()
    if legacy:
        environment["SONARE_LEGACY_EXIT"] = "1"
    if surface == "native":
        command = [executable, *argv]
        cwd = ROOT
    else:
        executable_name = Path(executable).name
        if executable_name.startswith("sonare"):
            command = [executable, *argv]
        else:
            command = [executable, "-m", "libsonare.cli", *argv]
        cwd = ROOT / "bindings" / "python"
        python_src = str(cwd / "src")
        environment["PYTHONPATH"] = (
            python_src + os.pathsep + environment.get("PYTHONPATH", "")
        )
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "returncode": None,
            "stdout": "",
            "stderr": "",
            "error": str(exc),
            "command": command,
        }
    return {
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "error": "",
        "command": command,
    }


def _read_inventory_dump(
    surface: str, executable: str, timeout: float
) -> tuple[Any | None, str | None]:
    """Read one fresh ``--dump-cli-contract`` document for snapshot mode."""

    result = _run(surface, executable, ["--dump-cli-contract"], False, timeout)
    if result["returncode"] != 0 or not result["stdout"].strip():
        detail = result.get("error") or f"exit {result['returncode']}"
        return (
            None,
            f"inventory.{surface}: --dump-cli-contract is not available ({detail})",
        )
    try:
        return parse_single_json(result["stdout"]), None
    except ValueError as exc:
        return (
            None,
            f"inventory.{surface}: --dump-cli-contract did not return one JSON document ({exc})",
        )


def _inventory_checks(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    timeout: float,
    report: list[tuple[str, str]],
) -> dict[str, dict[str, Any]] | None:
    result = _run(surface, executable, ["--dump-cli-contract"], False, timeout)
    if result["returncode"] != 0 or not result["stdout"].strip():
        detail = result.get("error") or f"exit {result['returncode']}"
        report.append(
            (
                "expected",
                f"inventory.{surface}: --dump-cli-contract is not available yet ({detail})",
            )
        )
        return None
    try:
        value = parse_single_json(result["stdout"])
    except ValueError as exc:
        message = f"inventory.{surface}: --dump-cli-contract did not return one JSON document ({exc})"
        report.append(
            (
                "expected",
                message,
            )
        )
        return None
    errors, commands = _validate_inventory(value, surface)
    for error in errors:
        report.append(("fail", error))
    expected = _expected_paths(manifest["commands"], surface)
    actual = set(commands)
    for path in sorted(expected - _accepted_names(commands)):
        report.append(("fail", f"inventory.{surface}: missing classified path {path}"))
    for path in sorted(actual - expected):
        report.append(("fail", f"inventory.{surface}: unclassified path {path}"))
    expected_options_by_path = manifest.get("inventory", {}).get("expected_options", {})
    if not isinstance(expected_options_by_path, dict):
        report.append(
            (
                "fail",
                f"manifest.inventory.expected_options: expected an object, got {type(expected_options_by_path).__name__}",
            )
        )
        expected_options_by_path = {}
    for path, expected_options in expected_options_by_path.items():
        if path not in commands:
            continue
        actual_options = commands[path]["options"]
        actual_normalized = _normalized_option_inventory(actual_options)
        expected_normalized = _normalized_option_inventory(expected_options)
        if actual_normalized is None:
            # _validate_inventory already emitted the detailed field-level
            # schema errors.  Keep the live checker total and avoid a KeyError
            # while still making this path fail explicitly.
            report.append(
                (
                    "fail",
                    f"inventory.{surface}.{path}: malformed option metadata cannot be compared",
                )
            )
            continue
        if expected_normalized is None:
            report.append(
                (
                    "fail",
                    f"manifest.inventory.expected_options.{path}: malformed option metadata cannot be compared",
                )
            )
            continue
        if actual_normalized != expected_normalized:
            message = (
                f"inventory.{surface}.{path}: active option schema differs\n"
                f"  expected: {expected_normalized!r}\n"
                f"  actual:   {actual_normalized!r}"
            )
            report.append(
                (
                    "fail",
                    message,
                )
            )
    return commands


def _run_active_cases(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    paths: dict[str, str],
    timeout: float,
    report: list[tuple[str, str]],
) -> dict[tuple[str, str], Any]:
    payloads: dict[tuple[str, str], Any] = {}
    for contract in manifest["active_paths"]:
        path = contract["path"]
        for case in contract["cases"]:
            label = f"{surface}.{path}.{case['id']}"
            normal_paths = dict(paths)
            if case["artifact"] != "none":
                artifact = contract["artifacts"][case["artifact"]]
                normal_paths[artifact["placeholder"]] = str(
                    Path(paths[artifact["placeholder"]]).with_name(
                        f"{surface}_{Path(paths[artifact['placeholder']]).name}"
                    )
                )
                Path(normal_paths[artifact["placeholder"]]).unlink(missing_ok=True)
            result = _run(
                surface,
                executable,
                _resolve_argv(case["argv"], normal_paths),
                False,
                timeout,
            )
            if result["returncode"] != case["exit"]:
                detail = result.get("error") or f"got exit {result['returncode']}"
                report.append(
                    ("fail", f"{label}: expected exit {case['exit']}, {detail}")
                )
            expected_stderr = case.get("stderr")
            if expected_stderr is not None and (
                (expected_stderr == "empty") != (not result["stderr"].strip())
            ):
                report.append(
                    (
                        "fail",
                        f"{label}: expected {expected_stderr} stderr, "
                        f"got {result['stderr'][:160]!r}",
                    )
                )
            _check_text_expectations(case, result, label, report)
            parsed: Any = None
            if case["stdout"] in {"empty", "text"}:
                if case["stdout"] == "empty" and result["stdout"].strip():
                    report.append(
                        (
                            "fail",
                            f"{label}: expected empty stdout, got {result['stdout'][:160]!r}",
                        )
                    )
            else:
                try:
                    parsed = parse_single_json(result["stdout"])
                except ValueError as exc:
                    report.append(("fail", f"{label}: {exc}"))
                if parsed is not None and case["payload"] != "none":
                    _validate_case_payload(
                        path, case, parsed, contract, surface, label, report
                    )
                    payloads[(path, case["id"])] = parsed
                    _check_artifact(
                        case["artifact"], contract, parsed, normal_paths, label, report
                    )
            legacy_paths = dict(paths)
            if case["artifact"] != "none":
                artifact = contract["artifacts"][case["artifact"]]
                legacy_paths[artifact["placeholder"]] = str(
                    Path(paths[artifact["placeholder"]]).with_name(
                        f"{surface}_legacy_{Path(paths[artifact['placeholder']]).name}"
                    )
                )
                Path(legacy_paths[artifact["placeholder"]]).unlink(missing_ok=True)
            legacy_result = _run(
                surface,
                executable,
                _resolve_argv(case["argv"], legacy_paths),
                True,
                timeout,
            )
            if legacy_result["returncode"] != case["legacy_exit"]:
                detail = (
                    legacy_result.get("error")
                    or f"got exit {legacy_result['returncode']}"
                )
                report.append(
                    (
                        "fail",
                        f"{label} [legacy]: expected exit {case['legacy_exit']}, {detail}",
                    )
                )
            if expected_stderr is not None and (
                (expected_stderr == "empty") != (not legacy_result["stderr"].strip())
            ):
                report.append(
                    (
                        "fail",
                        f"{label} [legacy]: expected {expected_stderr} stderr, "
                        f"got {legacy_result['stderr'][:160]!r}",
                    )
                )
            _check_text_expectations(case, legacy_result, f"{label} [legacy]", report)
            if case["stdout"] in {"empty", "text"}:
                if case["stdout"] == "empty" and legacy_result["stdout"].strip():
                    message = (
                        f"{label} [legacy]: expected empty stdout, "
                        f"got {legacy_result['stdout'][:160]!r}"
                    )
                    report.append(
                        (
                            "fail",
                            message,
                        )
                    )
            else:
                try:
                    legacy_payload = parse_single_json(legacy_result["stdout"])
                    if case["payload"] != "none":
                        _validate_case_payload(
                            path,
                            case,
                            legacy_payload,
                            contract,
                            surface,
                            f"{label} [legacy]",
                            report,
                        )
                        if parsed is not None:
                            normal_for_compare = parsed
                            legacy_for_compare = legacy_payload
                            if case["artifact"] != "none":
                                normal_for_compare = dict(parsed)
                                legacy_for_compare = dict(legacy_payload)
                                normal_for_compare.pop("output", None)
                                legacy_for_compare.pop("output", None)
                            mismatch = _compare_values(
                                normal_for_compare,
                                legacy_for_compare,
                                f"{label} [legacy]",
                                float(manifest["comparison"]["absolute"]),
                                float(manifest["comparison"]["relative"]),
                            )
                            if mismatch:
                                report.append(
                                    (
                                        "fail",
                                        mismatch
                                        + " (legacy payload must match normal mode)",
                                    )
                                )
                        if case["artifact"] != "none":
                            _check_artifact(
                                case["artifact"],
                                contract,
                                legacy_payload,
                                legacy_paths,
                                f"{label} [legacy]",
                                report,
                            )
                except ValueError as exc:
                    report.append(("fail", f"{label} [legacy]: {exc}"))
    if any(contract["path"] == "analyze" for contract in manifest["active_paths"]):
        _validate_analyze_case_relationships(surface, payloads, manifest, report)
    if any(contract["path"] == "voice-change" for contract in manifest["active_paths"]):
        _validate_voice_case_relationships(payloads, manifest, surface, report)
    return payloads


def _check_text_expectations(
    case: dict[str, Any],
    result: dict[str, Any],
    label: str,
    report: list[tuple[str, str]],
) -> None:
    """Check the human-readable expectations a case declares.

    ``stdout: "text"`` marks a case whose output is prose rather than JSON, so
    it is asserted by content instead of being parsed.  ``stdout_contains`` and
    ``stderr_contains`` hold substrings that must appear, which is how a case
    pins a diagnostic that must be shown or a message that must name the
    option and the value it rejected.  ``stdout_excludes`` holds substrings that
    must NOT appear: a command whose text output claims success while it returns
    a failing status cannot be caught by any positive assertion, because the
    diagnostic it should print and the success line it should not print can both
    be present at once.
    """
    if case["stdout"] == "text" and not result["stdout"].strip():
        report.append(("fail", f"{label}: expected text stdout, got nothing"))
    for stream in ("stdout", "stderr"):
        expected = case.get(f"{stream}_contains")
        if not expected:
            continue
        actual = result[stream]
        for needle in expected:
            if needle not in actual:
                report.append(
                    (
                        "fail",
                        f"{label}: expected {stream} to contain {needle!r}, "
                        f"got {actual[:160]!r}",
                    )
                )
    for needle in case.get("stdout_excludes") or []:
        if needle in result["stdout"]:
            report.append(
                (
                    "fail",
                    f"{label}: expected stdout not to contain {needle!r}, "
                    f"got {result['stdout'][:160]!r}",
                )
            )


def _run_parser_cases(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    paths: dict[str, str],
    timeout: float,
    report: list[tuple[str, str]],
) -> None:
    for case in manifest["parser_cases"]:
        label = f"{surface}.parser.{case['id']}"
        argv = _resolve_argv(case["argv"], paths)
        expected_exit = case["exit"]
        result = _run(surface, executable, argv, False, timeout)
        if result["returncode"] != expected_exit:
            detail = result.get("error") or f"got exit {result['returncode']}"
            report.append(("fail", f"{label}: expected exit {expected_exit}, {detail}"))
        if case["stdout"] == "empty" and result["stdout"].strip():
            report.append(
                (
                    "fail",
                    f"{label}: expected empty stdout, got {result['stdout'][:160]!r}",
                )
            )
        _check_text_expectations(case, result, label, report)
        legacy = _run(surface, executable, argv, True, timeout)
        if legacy["returncode"] != case["legacy_exit"]:
            detail = legacy.get("error") or f"got exit {legacy['returncode']}"
            report.append(
                (
                    "fail",
                    f"{label} [legacy]: expected exit {case['legacy_exit']}, {detail}",
                )
            )
        if case["stdout"] == "empty" and legacy["stdout"].strip():
            report.append(
                (
                    "fail",
                    f"{label} [legacy]: expected empty stdout, got {legacy['stdout'][:160]!r}",
                )
            )
        _check_text_expectations(case, legacy, f"{label} [legacy]", report)


def _print_listing(manifest: dict[str, Any]) -> None:
    print(f"{manifest['contract']} schema v{manifest['schema_version']}: OK")
    commands = manifest["commands"]
    for classification in (
        "shared",
        "native_only",
        "python_only",
        "intentional_variant",
    ):
        print(f"{classification}:")
        for path, record in commands.items():
            if record["classification"] == classification:
                print(f"  {path} [{record['status']}]")
    print("active semantic paths:")
    for contract in manifest["active_paths"]:
        print(f"  {contract['path']}: {len(contract['cases'])} cases")
    print("generic parser cases:")
    for case in manifest["parser_cases"]:
        print(f"  {case['id']}: exit {case['exit']} (legacy {case['legacy_exit']})")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--native", help="native sonare-cli executable")
    parser.add_argument("--python", help="Python interpreter or sonare console script")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--schema", action="store_true", help="validate the manifest only"
    )
    parser.add_argument(
        "--list", action="store_true", help="validate and list manifest paths"
    )
    parser.add_argument(
        "--emit-shared-option-snapshot",
        action="store_true",
        help="maintenance-only: emit canonical options for every shared path",
    )
    parser.add_argument(
        "--no-inventory", action="store_true", help="skip --dump-cli-contract checks"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.schema and args.list:
        print("--schema and --list are mutually exclusive", file=sys.stderr)
        return 2
    if args.emit_shared_option_snapshot and (
        args.schema or args.list or args.no_inventory
    ):
        print(
            "--emit-shared-option-snapshot cannot be combined with "
            "--schema, --list, or --no-inventory",
            file=sys.stderr,
        )
        return 2
    try:
        manifest = load_manifest(args.manifest)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    if args.schema:
        print(f"{manifest['contract']} schema v{manifest['schema_version']}: OK")
        return 0
    if args.list:
        _print_listing(manifest)
        return 0
    if args.emit_shared_option_snapshot:
        if not args.native or not args.python:
            print(
                "--emit-shared-option-snapshot requires both --native and --python",
                file=sys.stderr,
            )
            return 2
        if args.timeout <= 0 or not math.isfinite(args.timeout):
            print("--timeout must be a positive finite number", file=sys.stderr)
            return 2

        native_executable = _resolve_executable(args.native)
        python_executable = _resolve_executable(args.python)
        native_inventory, native_error = _read_inventory_dump(
            "native", native_executable, args.timeout
        )
        python_inventory, python_error = _read_inventory_dump(
            "python", python_executable, args.timeout
        )
        errors = [error for error in (native_error, python_error) if error]
        if not errors:
            try:
                snapshot = _build_shared_option_snapshot(
                    native_inventory, python_inventory, manifest
                )
            except ValueError as exc:
                errors.append(str(exc))
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        print(json.dumps(snapshot, indent=2, ensure_ascii=False))
        return 0
    if not args.native and not args.python:
        message = (
            f"{manifest['contract']} schema v{manifest['schema_version']}: OK "
            "(schema-only; pass --native and --python for live checks)"
        )
        print(message)
        return 0
    if not args.native or not args.python:
        print("live checks require both --native and --python", file=sys.stderr)
        return 2
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        print("--timeout must be a positive finite number", file=sys.stderr)
        return 2

    native_executable = _resolve_executable(args.native)
    python_executable = _resolve_executable(args.python)

    report: list[tuple[str, str]] = []
    _check_artifact_skew(native_executable, python_executable, args.timeout, report)
    with tempfile.TemporaryDirectory(prefix="libsonare-cli-contract-") as temporary:
        paths = _write_fixtures(Path(temporary), manifest)
        native_inventory: dict[str, dict[str, Any]] | None = None
        python_inventory: dict[str, dict[str, Any]] | None = None
        if not args.no_inventory:
            native_inventory = _inventory_checks(
                "native", native_executable, manifest, args.timeout, report
            )
            python_inventory = _inventory_checks(
                "python", python_executable, manifest, args.timeout, report
            )
            if native_inventory is not None and python_inventory is not None:
                _compare_active_inventory_options(
                    native_inventory, python_inventory, manifest, report
                )
        native_payloads = _run_active_cases(
            "native", native_executable, manifest, paths, args.timeout, report
        )
        python_payloads = _run_active_cases(
            "python", python_executable, manifest, paths, args.timeout, report
        )
        for contract in manifest["active_paths"]:
            path = contract["path"]
            for case in contract["cases"]:
                key = (path, case["id"])
                if key not in native_payloads or key not in python_payloads:
                    continue
                mismatch = _compare_payloads(
                    path,
                    native_payloads[key],
                    python_payloads[key],
                    manifest,
                    case["id"],
                    case["artifact"] != "none",
                )
                if mismatch:
                    report.append(("fail", f"surface comparison: {mismatch}"))
                if (
                    path == "project.validate"
                    and case["id"] == "warning_strict_artifact"
                ):
                    warning_key = (path, "warning")
                    if warning_key in native_payloads:
                        mismatch = _compare_values(
                            native_payloads[key],
                            native_payloads[warning_key],
                            f"native.{path}.strict",
                            float(manifest["comparison"]["absolute"]),
                            float(manifest["comparison"]["relative"]),
                        )
                        if mismatch:
                            report.append(
                                (
                                    "fail",
                                    mismatch + " (strict must preserve the payload)",
                                )
                            )
                    if warning_key in python_payloads:
                        mismatch = _compare_values(
                            python_payloads[key],
                            python_payloads[warning_key],
                            f"python.{path}.strict",
                            float(manifest["comparison"]["absolute"]),
                            float(manifest["comparison"]["relative"]),
                        )
                        if mismatch:
                            report.append(
                                (
                                    "fail",
                                    mismatch + " (strict must preserve the payload)",
                                )
                            )
        _run_parser_cases(
            "native", native_executable, manifest, paths, args.timeout, report
        )
        _run_parser_cases(
            "python", python_executable, manifest, paths, args.timeout, report
        )

    if report:
        for kind, message in report:
            prefix = "EXPECTED-FAILURE" if kind == "expected" else "FAIL"
            print(f"[{prefix}] {message}")
        print(f"cli contract v2: {len(report)} issue(s)")
        return 1
    print("cli contract v2: live checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
