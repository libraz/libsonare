"""Regression tests for the lint-scope agreement check.

The failure it exists for is quiet: widening a gate in the Makefile alone leaves
the workflow that actually runs on a push exactly as narrow, and the run is
green either way. So what these pin is that the check sees a disagreement rather
than reporting the value it happens to read first -- and that it still fails when
an invocation moves out from under its parsing, since a check that silently
matches nothing agrees with everything.

Stdlib only; no build needed. Run directly:

    python3 tools/conformance/test_lint_scope.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_lint_scope as mod

MAKEFILE = """
format:
\truff format bindings/python/src
\truff check --fix .
\tgit ls-files -z -- '*.py' | xargs -0 ruff check --fix
\t$(MAKE) lint
lint:
\truff check .
format-check:
\tgit ls-files -z -- '*.h' '*.cpp' ':!:third_party/**' | xargs -0 clang-format --dry-run
"""

WORKFLOW = """
      - name: Check formatting
        run: |
          git ls-files -z -- '*.h' '*.cpp' ':!:third_party/**' |
            xargs -0 clang-format --dry-run --Werror
      - name: Lint Python
        run: |
          uv sync --locked --project bindings/python --only-group lint
          uv run --no-sync --project bindings/python ruff check .
"""


def test_a_fix_flag_is_not_a_narrower_scope() -> None:
    """`make format` runs the same path with `--fix`; only the path is scope."""
    assert mod.ruff_targets(MAKEFILE) == ["."]


def test_an_xargs_fed_call_contributes_no_target() -> None:
    """The auto-fix side ends its line at the flags and takes files from `xargs`.

    A match allowed to cross the newline reads the next recipe line instead, so
    the Makefile reported a scope of `$(MAKE)` alongside `.` and disagreed with
    both workflows over an invocation that names no path at all.
    """
    assert mod.ruff_targets("\tgit ls-files -z | xargs -0 ruff check --fix\n\t$(MAKE) lint\n") == []


def test_the_ruff_target_is_read_out_of_a_workflow_too() -> None:
    assert mod.ruff_targets(WORKFLOW) == ["."]


def test_a_narrower_workflow_scope_is_a_disagreement() -> None:
    """The defect this exists for: the Makefile widened, the workflow did not."""
    failures: list[str] = []
    mod.check_agreement(
        "the ruff scope",
        {
            "Makefile": mod.ruff_targets(MAKEFILE),
            "ci.yml": mod.ruff_targets(
                WORKFLOW.replace("ruff check .", "ruff check bindings/python")
            ),
        },
        failures,
    )
    assert len(failures) == 1, failures
    assert "disagrees" in failures[0]


def test_matching_scopes_raise_nothing() -> None:
    failures: list[str] = []
    mod.check_agreement(
        "the ruff scope",
        {
            "Makefile": mod.ruff_targets(MAKEFILE),
            "ci.yml": mod.ruff_targets(WORKFLOW),
        },
        failures,
    )
    assert failures == []


def test_a_scope_no_file_carries_fails_rather_than_passing_vacuously() -> None:
    """An invocation that moved leaves the parsing matching nothing, and a check
    that matches nothing agrees with everything."""
    failures: list[str] = []
    mod.check_agreement("the ruff scope", {"Makefile": [], "ci.yml": []}, failures)
    assert len(failures) == 1
    assert "no file carries it" in failures[0]


def test_only_a_pathspec_reaching_clang_format_counts() -> None:
    """A `git ls-files` feeding something else is a different question."""
    other = "git ls-files -z -- '*.py' | xargs -0 ruff check\n" + MAKEFILE
    assert mod.clang_format_specs(other) == [("*.h", "*.cpp", ":!:third_party/**")]


def test_a_glob_dropped_from_one_workflow_is_a_disagreement() -> None:
    narrowed = WORKFLOW.replace("'*.h' '*.cpp'", "'*.h'")
    failures: list[str] = []
    mod.check_agreement(
        "the clang-format pathspec",
        {
            "Makefile": sorted(set(mod.clang_format_specs(MAKEFILE))),
            "ci.yml": sorted(set(mod.clang_format_specs(narrowed))),
        },
        failures,
    )
    assert len(failures) == 1, failures


def test_the_locked_installs_raise_nothing() -> None:
    lines = [
        "uv sync --locked --project bindings/python --only-group lint",
        "uv run --no-sync --project bindings/python ruff check .",
        "uv run --locked --only-group wheel auditwheel show dist/*.whl",
        'uv pip install --python "$RUNNER_TEMP/v" --resolution lowest-direct -e . -r "$T/test.txt"',
        "pip install dist/*.whl",
    ]
    assert [mod.install_problems(line) for line in lines] == [[]] * len(lines)


def test_a_tool_installed_by_name_is_a_problem() -> None:
    """The defect this half exists for: a version copied into the workflow by hand."""
    for line in (
        "python3 -m pip install --disable-pip-version-check ruff==0.15.14",
        "uv pip install --python .venv clang-format",
        "pip install dist/*.whl pytest",
    ):
        problems = mod.install_problems(line)
        assert problems and "by name" in problems[0], (line, problems)


def test_an_editable_install_of_anything_but_the_project_is_a_problem() -> None:
    assert mod.install_problems("uv pip install -e ../other") != []


def test_an_unlocked_sync_or_run_is_a_problem() -> None:
    assert mod.install_problems("uv sync --project bindings/python") != []
    assert mod.install_problems("uv run --project bindings/python mypy src") != []


def test_a_continued_line_is_read_whole() -> None:
    """The floor job wraps its install; the package names sit on the second line."""
    text = "uv pip install --python v \\\n            -e . numpy==1.0\n"
    (line,) = mod.shell_lines(text)
    assert mod.install_problems(line) == ["`numpy==1.0` is installed by name, outside the lock"]


def test_a_line_installing_nothing_is_not_counted() -> None:
    assert mod.install_problems("uv build --wheel --out-dir dist/") is None
    assert mod.install_problems("      - name: Install clang-format") is None


def test_the_repository_agrees_with_itself() -> None:
    """The live check, so a real divergence fails here and not only in CI."""
    assert mod.main() == 0


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
