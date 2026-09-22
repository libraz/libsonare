"""The tree a fit renders through: write-back path translation, the build
directory, what a render is allowed to have come from, and startup.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_tree.py -q
"""

from __future__ import annotations

import argparse
import functools
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import build_lib
import report as report_module
from _repo import REPO_ROOT
from autofit import Evaluator
from autofit_test_fixtures import (
    _fit_args,
    _probe_args,
    _source_knob,
    _terms,
)
from build_lib import configure_build
from knobs import Knob, format_value
from render_model import DEFAULT_DYLIB, check_gm_fallback
from report import report_result
from writeback import (
    TUNING_LAYER_FILE,
    array_members,
    key_to_member_path,
    materialize,
    override_patch_names,
    restore,
    write_edits,
    write_patch_fields,
)


# --------------------------------------------------------------------------- #
# Write-back
# --------------------------------------------------------------------------- #
def test_array_elements_regain_their_brackets():
    assert key_to_member_path("pipe_organ.ranks2.level") == "pipe_organ.ranks[2].level"
    assert key_to_member_path("fm.ops1.ratio") == "fm.ops[1].ratio"
    assert key_to_member_path("modal.modes0.gain") == "modal.modes[0].gain"
    assert key_to_member_path("additive.drawbars3") == "additive.drawbars[3]"


def test_a_member_whose_name_ends_in_a_digit_is_not_an_array():
    assert key_to_member_path("lfo2_rate_hz") == "lfo2_rate_hz"
    assert key_to_member_path("bowed_string.bow_force") == "bowed_string.bow_force"


def test_every_indexed_key_the_library_forms_is_known_to_be_an_array():
    """The list is derived, because the hand-written one drifted and broke a build.

    Two percussion arrays were added to the tuning layer and not to the list, so
    a fitted drum value was written as `mode_ratios1` — not a member of
    anything. This asserts the derivation covers the whole key space rather than
    the four members somebody happened to remember.
    """
    members = array_members()
    text = (REPO_ROOT / TUNING_LAYER_FILE).read_text()
    assert len(members) >= 9
    for name in members:
        assert f'"{name}"' in text or f'{name}" +' in text


def test_an_array_whose_own_name_carries_digits_still_gets_its_brackets():
    # `shell_t60_s3` is the whole reason the index is read as the trailing run
    # rather than as everything after the last letter.
    assert "shell_t60_s" in array_members()
    assert key_to_member_path("percussion.shell_t60_s3") == "percussion.shell_t60_s[3]"
    assert key_to_member_path("percussion.mode_ratios1") == "percussion.mode_ratios[1]"
    assert key_to_member_path("percussion.mode_alpha2") == "percussion.mode_alpha[2]"


def test_the_patch_name_list_matches_the_x_macro():
    names = override_patch_names()
    assert len(names) == len(set(names))
    assert "violin" in names and "ocarina" in names and "church_organ" in names


def _unassigned_violin_field() -> str:
    """A `bowed_string` field the table does not set for the violin.

    Chosen here rather than written down: this case needs a field that is new to
    its block, and a round of fitting turns any named one into an assignment,
    which leaves the case exercising the replace path while asserting the append
    path's postcondition. That is how it went red, silently, once.
    """
    tables = "\n".join(
        p.read_text()
        for p in sorted((REPO_ROOT / "src/midi/synth").glob("gm_fallback_programs_*.h"))
    )
    assigned = set(re.findall(r"o\.violin\.bowed_string\.(\w+)\s*=", tables))
    headers = "\n".join(p.read_text() for p in sorted((REPO_ROOT / "src/midi/synth").glob("*.h")))
    block = re.search(r"struct\s+BowedStringPatchParams\s*\{(.*?)\n\};", headers, re.DOTALL)
    assert block, "BowedStringPatchParams is not declared where this case looks for it"
    declared = re.findall(
        r"^\s*(?:float|int|bool|uint8_t)\s+(\w+)\s*=", block.group(1), re.MULTILINE
    )
    free = sorted(set(declared) - assigned)
    assert free, "every bowed_string field is assigned; pick another struct for this case"
    return free[0]


def test_a_new_patch_field_is_appended_to_its_block():
    field = _unassigned_violin_field()
    edited = write_patch_fields({"violin": [(f"bowed_string.{field}", 0.61)]})
    assert len(edited) == 1
    text = next(iter(edited.values()))
    assert f"o.violin.bowed_string.{field} = 0.61f;" in text
    path = next(iter(edited))
    assert f"o.violin.bowed_string.{field}" not in path.read_text()


def test_an_existing_assignment_is_replaced_not_duplicated():
    """The table already sets `o.violin.cutoff_hz`; a fit must move it, not add a second."""
    edited = write_patch_fields({"violin": [("cutoff_hz", 5200.0)]})
    text = next(iter(edited.values()))
    assert text.count("o.violin.cutoff_hz") == 1
    assert "o.violin.cutoff_hz = 5200.0f;" in text


def test_the_appended_line_keeps_the_surrounding_indentation():
    edited = write_patch_fields({"violin": [("bowed_string.rosin", 0.2)]})
    text = next(iter(edited.values()))
    line = next(ln for ln in text.splitlines() if "o.violin.bowed_string.rosin" in ln)
    other = next(ln for ln in text.splitlines() if ln.strip().startswith("o.violin."))
    assert line[: len(line) - len(line.lstrip())] == other[: len(other) - len(other.lstrip())]


# --------------------------------------------------------------------------- #
# The build dir a fit renders through
# --------------------------------------------------------------------------- #
def _cache(build_dir: Path, **options) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    lines = [f"CMAKE_HOME_DIRECTORY:INTERNAL={REPO_ROOT}"]
    lines += [f"{name}:STRING={value}" for name, value in options.items()]
    (build_dir / "CMakeCache.txt").write_text("\n".join(lines) + "\n")
    return build_dir


def test_a_dir_configured_without_the_shared_target_is_reconfigured(tmp_path, monkeypatch):
    """`sonare_shared` would not exist, and the build error names no option."""
    build_dir = _cache(
        tmp_path / "build", BUILD_TUNING="ON", BUILD_SHARED="OFF", CMAKE_BUILD_TYPE="Release"
    )
    ran: list[list[str]] = []
    monkeypatch.setattr(build_lib.subprocess, "run", lambda cmd, **k: ran.append(cmd))
    configure_build(build_dir, "cmake", tuning=True)
    assert ran and "-DBUILD_SHARED=ON" in ran[0]


def test_a_debug_dir_is_reconfigured_to_release(tmp_path, monkeypatch):
    build_dir = _cache(
        tmp_path / "build", BUILD_TUNING="OFF", BUILD_SHARED="ON", CMAKE_BUILD_TYPE="Debug"
    )
    ran: list[list[str]] = []
    monkeypatch.setattr(build_lib.subprocess, "run", lambda cmd, **k: ran.append(cmd))
    configure_build(build_dir, "cmake", tuning=False)
    assert ran and "-DCMAKE_BUILD_TYPE=Release" in ran[0]


def test_a_matching_dir_is_left_alone(tmp_path, monkeypatch):
    build_dir = _cache(
        tmp_path / "build", BUILD_TUNING="ON", BUILD_SHARED="ON", CMAKE_BUILD_TYPE="Release"
    )
    monkeypatch.setattr(
        build_lib.subprocess, "run", lambda *a, **k: pytest.fail("reconfigured a compatible dir")
    )
    configure_build(build_dir, "cmake", tuning=True)


def test_a_dir_belonging_to_another_checkout_is_refused(tmp_path):
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    (build_dir / "CMakeCache.txt").write_text(
        "CMAKE_HOME_DIRECTORY:INTERNAL=/somewhere/else\nBUILD_SHARED:STRING=ON\n"
    )
    with pytest.raises(RuntimeError, match="/somewhere/else"):
        configure_build(build_dir, "cmake", tuning=True)


# --------------------------------------------------------------------------- #
# What a render is allowed to have come from
# --------------------------------------------------------------------------- #
class _ManifestEntry:
    def __init__(self, program: int, backend: int):
        self.program, self.backend = program, backend


def test_a_soundfont_render_is_not_reported_as_the_native_bank():
    check_gm_fallback([_ManifestEntry(0, 0), _ManifestEntry(40, 0)])
    with pytest.raises(RuntimeError, match="expected GM fallback"):
        check_gm_fallback([_ManifestEntry(0, 0), _ManifestEntry(40, 1)])


def test_the_corpus_renderer_checks_the_backend_too():
    """Read from the source: importing `render_corpus` loads the dylib at module scope."""
    source = (Path(__file__).resolve().parent / "render_corpus.py").read_text()
    assert "check_gm_fallback(manifest)" in source


# --------------------------------------------------------------------------- #
# Keeping the tree consistent with the candidate being scored
# --------------------------------------------------------------------------- #
def test_a_file_whose_knob_is_back_at_its_default_is_still_written(tmp_path):
    """Left out, it would keep the previous candidate's value through this render."""
    a_path, a = _source_knob(tmp_path, "a.cpp", "1.0")
    b_path, b = _source_knob(tmp_path, "b.cpp", "2.0")
    pristine = {a_path: a_path.read_text(), b_path: b_path.read_text()}
    full = materialize([a, b], [9.0, 2.0], pristine, full=True)
    assert set(full) == {a_path, b_path}
    assert full[b_path] == pristine[b_path]
    # The report keeps the minimal diff: nothing to say about a knob that stayed.
    assert set(materialize([a, b], [9.0, 2.0], pristine)) == {a_path}


def test_every_source_file_matches_the_candidate_being_rendered(tmp_path, monkeypatch):
    """A stale file would score a knob vector that was never assembled."""
    a_path, a = _source_knob(tmp_path, "a.cpp", "1.0")
    b_path, b = _source_knob(tmp_path, "b.cpp", "2.0")
    pristine = {a_path: a_path.read_text(), b_path: b_path.read_text()}
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    evaluator = Evaluator([a, b], pristine, [], None, _fit_args(), tmp_path / "build")

    on_disk: list[tuple[str, str]] = []

    def capture(values):
        on_disk.append((a_path.read_text(), b_path.read_text()))
        return _terms(harm=1.0)

    evaluator._render_terms = capture
    evaluator([1.0, 5.0])  # b moves away from its default
    evaluator([9.0, 2.0])  # ...and back to it, while a moves
    assert on_disk[1] == (materialize([a], [9.0], pristine, full=True)[a_path], pristine[b_path])


def test_a_cached_point_still_decides_the_best(tmp_path, monkeypatch):
    """Each stage re-scores the point it inherited, which is always a cache hit."""
    knob = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
    evaluator = Evaluator([knob], {}, [], None, _fit_args(), tmp_path / "build")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    scores = {"0.5": 10.0, "0.9": 40.0}
    evaluator._render_terms = lambda values: _terms(harm=scores[format_value(values[0])])

    evaluator([0.5])
    evaluator.restage({"harm": 1.0}, "decay")
    evaluator([0.5])  # cached, and better than anything this stage renders
    evaluator([0.9])
    assert evaluator.best_values == [0.5]


# --------------------------------------------------------------------------- #
# Undoing a fit's edits without undoing anyone else's
# --------------------------------------------------------------------------- #
def test_restore_puts_back_what_the_fit_wrote(tmp_path):
    path, _ = _source_knob(tmp_path, "a.cpp", "1.0")
    pristine = {path: path.read_text()}
    written: dict[Path, str] = {}
    write_edits({path: "spliced\n"}, written)
    restore(pristine, written)
    assert path.read_text() == pristine[path]


def test_restore_leaves_a_file_the_fit_never_wrote(tmp_path):
    """A runtime knob's declaration file is snapshotted for the diff, never written."""
    path, _ = _source_knob(tmp_path, "declaration.cpp", "1.0")
    pristine = {path: path.read_text()}
    path.write_text("edited by hand while the fit ran\n")
    restore(pristine, {})
    assert path.read_text() == "edited by hand while the fit ran\n"


def test_restore_does_not_roll_back_an_edit_made_after_its_own(tmp_path, capsys):
    """Hours of fitting must not cost someone the file they were editing meanwhile."""
    path, _ = _source_knob(tmp_path, "a.cpp", "1.0")
    pristine = {path: path.read_text()}
    written: dict[Path, str] = {}
    write_edits({path: "spliced\n"}, written)
    path.write_text("edited by hand while the fit ran\n")
    restore(pristine, written)
    assert path.read_text() == "edited by hand while the fit ran\n"
    assert "was edited after the fit wrote it" in capsys.readouterr().err


# --------------------------------------------------------------------------- #
# Every named patch has somewhere to write to
# --------------------------------------------------------------------------- #
def _table_texts() -> dict[Path, str]:
    from writeback import PROGRAM_TABLE_FILES

    return {
        REPO_ROOT / name: (REPO_ROOT / name).read_text()
        for name in PROGRAM_TABLE_FILES
        if (REPO_ROOT / name).exists()
    }


def test_every_named_patch_has_a_write_back_site():
    """A patch with none falls silently into `unplaced` and the fit loop never closes."""
    tables = _table_texts()
    unplaced = [
        patch
        for patch in override_patch_names()
        if not write_patch_fields({patch: [("amp_env.release_ms", 123.0)]}, tables)
    ]
    assert unplaced == []


def test_a_patch_built_through_a_reference_is_written_through_it():
    """A third of the melodic bank never spells `o.<patch>` after the binding line."""
    edited = write_patch_fields({"vibraphone": [("body_mix", 0.33)]})
    text = next(iter(edited.values()))
    lines = text.splitlines()
    written = next(i for i, ln in enumerate(lines) if "vb.body_mix = 0.33f;" in ln)
    bound = next(i for i, ln in enumerate(lines) if "NativeSynthPatch& vb = o.vibraphone;" in ln)
    following = next(i for i, ln in enumerate(lines) if i > bound and "NativeSynthPatch&" in ln)
    assert bound < written < following


def test_a_reference_built_patch_replaces_its_own_line_rather_than_adding_one():
    edited = write_patch_fields({"vibraphone": [("modal.decay_s", 4.2)]})
    text = next(iter(edited.values()))
    assert text.count("vb.modal.decay_s =") == 1
    assert "vb.modal.decay_s = 4.2f;" in text


def test_two_write_backs_into_one_file_both_survive(tmp_path, monkeypatch, capsys):
    """A `SONARE_TUNABLE` splice and a patch-field line can land in the same file."""
    path, knob = _source_knob(tmp_path, "shared.h", "1.0")
    knob = Knob(**{**vars(knob), "tunable": "violin.kValue"})
    patch_knob = Knob(
        label="violin.cutoff_hz",
        lo=0.0,
        hi=1.0,
        log=False,
        start_value=0.5,
        tunable="violin.cutoff_hz",
    )

    def fake_write(per_patch, base=None):
        return {path: (base or {})[path] + "// patch field\n"}

    monkeypatch.setattr(report_module, "write_patch_fields", fake_write)
    monkeypatch.setattr(report_module, "REPO_ROOT", tmp_path)  # the knob file lives here
    evaluator = argparse.Namespace(trajectory=[], n_renders=0, best_loss=0.5, normalize=True)
    args = _probe_args(out="", dry_run=True)
    report_result([knob, patch_knob], {path: path.read_text()}, [7.0, 0.9], evaluator, args)
    printed = capsys.readouterr().out
    assert "kValue = 7.0f" in printed and "// patch field" in printed


# --------------------------------------------------------------------------- #
# Starting up the way a user starts it
# --------------------------------------------------------------------------- #
HERE = Path(__file__).resolve().parent


def _is_entry_point(path: Path) -> bool:
    """Whether a file has a `__main__` block — the line itself, not a mention of it.

    Anchored on the whole line because this file quotes that source line, and a
    substring search over the directory therefore matches the test module too.
    """
    if path.name.startswith("test_"):
        return False
    return any(
        line.rstrip() == 'if __name__ == "__main__":' for line in path.read_text().splitlines()
    )


# Every script here with a `__main__`, discovered rather than listed: the next
# module to grow one is covered without anyone remembering to add it.
CLI_SCRIPTS = sorted(path.name for path in HERE.glob("*.py") if _is_entry_point(path))

# Run the file's module scope in a child, with the path a direct `python
# <script>` produces and nothing else. `run_name` keeps `main()` out of it —
# what is under test is what happens before the first line of a command runs.
_IMPORT_SMOKE = (
    "import runpy, sys\n"
    "sys.path[0] = sys.argv[1]\n"
    "runpy.run_path(sys.argv[2], run_name='__voicematch_import_smoke__')\n"
)


def _needs_native_library(path: Path) -> bool:
    """Whether a script loads the C library while it imports rather than when it runs.

    Column zero only: `render_model` imports `libsonare` inside a function, on
    purpose, so that `SONARE_LIB_PATH` is set before the dylib is mapped.
    """
    return any(
        line.startswith(("import libsonare", "from libsonare import"))
        for line in path.read_text().splitlines()
    )


# What `render_model.ensure_lib_path` would resolve to, replayed in a child with
# no repo imports of its own, so the only thing that can fail in it is the load.
_NATIVE_PROBE = (
    "import os, sys\n"
    "if 'SONARE_LIB_PATH' not in os.environ and os.path.exists(sys.argv[1]):\n"
    "    os.environ['SONARE_LIB_PATH'] = sys.argv[1]\n"
    "import libsonare\n"
)


@functools.lru_cache(maxsize=1)
def _native_library_loads() -> bool:
    """Whether a libsonare dylib is available to load at all."""
    proc = subprocess.run(
        [sys.executable, "-c", _NATIVE_PROBE, str(DEFAULT_DYLIB)],
        capture_output=True,
        check=False,
        text=True,
    )
    return proc.returncode == 0


def test_every_entry_point_is_covered_by_the_import_smoke():
    """The discovery is the test's reach; an empty or shrunken list is a silent pass."""
    assert len(CLI_SCRIPTS) >= 7
    assert {"voicematch.py", "autofit.py"} <= set(CLI_SCRIPTS)
    # Covered even where the case below can only skip: a script that cannot run
    # here still has to be one the suite knows about.
    assert "render_corpus.py" in CLI_SCRIPTS
    assert not [s for s in CLI_SCRIPTS if s.startswith("test_")]  # not entry points


@pytest.mark.parametrize("script", CLI_SCRIPTS)
def test_a_cli_entry_point_imports_as_shipped(script, tmp_path):
    """Each CLI must resolve its own imports without this test file's help.

    `python tools/voicematch/<script>.py` puts `tools/voicematch/` on the path
    and nothing else, so a module-scope import of anything living in `tools/`
    — `_repo`, reached through `catalogue`, `knobs` or `writeback` — needs the
    script to insert that directory itself. No in-process test can see the
    difference: this file puts `tools/` on the path when pytest imports it, so
    every module resolves either way and a CLI that dies on the user's first
    keystroke passes anyway. Hence a child process, started the way the
    docstrings say to start it.

    `render_corpus.py` imports libsonare at module scope, so its case needs the
    built dylib as well as a resolvable path. That one is skipped when the
    library is absent rather than failed: this suite is otherwise pure Python
    and runs from a bare checkout, and a developer who reads one environmental
    red tends to stop trusting every other case with it. The skip is keyed on
    whether the library loads at all, so an ImportError from inside
    `render_corpus.py` is still red, it stays a hard failure wherever a dylib is
    present, and the coverage test above still requires the script to be
    discovered.
    """
    if _needs_native_library(HERE / script) and not _native_library_loads():
        pytest.skip(
            f"{script} imports libsonare at module scope and no shared library is "
            f"available: set SONARE_LIB_PATH, or build one with "
            f"`cmake --build build-python-shared --target sonare_shared` "
            f"(expected at {DEFAULT_DYLIB})"
        )
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)  # never let an ambient path answer the question
    proc = subprocess.run(
        [sys.executable, "-c", _IMPORT_SMOKE, str(HERE), str(HERE / script)],
        capture_output=True,
        check=False,
        text=True,
        cwd=tmp_path,
        env=env,
    )
    assert proc.returncode == 0, (
        f"{script} does not import on a clean interpreter "
        f"(exit {proc.returncode}):\n{proc.stderr.strip()[-1200:]}"
    )
