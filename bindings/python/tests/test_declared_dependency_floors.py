"""The declared dependency floors must be versions this suite actually runs.

A floor is a support claim, and nothing about declaring one makes it true: the
package declared a numpy minimum several major versions below anything the lock
files, the CI jobs or any developer environment had ever installed, so the
oldest numpy it promised to work with had never once been imported.

These checks are deliberately coarse. They do not try to establish that the
floor works -- only a run against that version could -- they establish that the
floor is not a version nobody has run.

Version parsing is done here rather than through ``packaging`` on purpose: that
distribution is not declared by this package either, and a guard against
undeclared reliance should not rest on one.
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

import numpy as np
import pytest

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
PYPROJECT = PACKAGE_ROOT / "pyproject.toml"
RUNTIME_LOCK = PACKAGE_ROOT / "requirements.lock"

_RELEASE = re.compile(r"(\d+(?:\.\d+)*)")


def _release(version: str) -> tuple[int, ...]:
    """Numeric release segment, as a tuple that orders like a version."""
    match = _RELEASE.match(version.strip())
    if match is None:
        pytest.fail(f"cannot read a version out of {version!r}")
    return tuple(int(part) for part in match.group(1).split("."))


def _dependency(distribution: str) -> str:
    data = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))
    for raw in data["project"]["dependencies"]:
        if re.match(rf"{distribution}\b", raw.strip(), flags=re.IGNORECASE):
            return raw.strip()
    pytest.fail(f"{distribution} is not declared in [project.dependencies]")


def _declared_floor(distribution: str) -> tuple[int, ...]:
    """Lowest release the package's own dependency specifier admits."""
    raw = _dependency(distribution)
    floors = re.findall(r"(?:>=|==|~=)\s*([0-9][^,\s]*)", raw)
    if not floors:
        pytest.fail(f"{distribution} is declared without a lower bound: {raw!r}")
    return max(_release(floor) for floor in floors)


def _locked_release(distribution: str) -> tuple[int, ...]:
    for line in RUNTIME_LOCK.read_text(encoding="utf-8").splitlines():
        name, separator, version = line.partition("==")
        if separator and name.strip().lower() == distribution:
            return _release(version)
    pytest.fail(f"{distribution} is not pinned in {RUNTIME_LOCK.name}")


def test_numpy_floor_is_not_above_the_pinned_version() -> None:
    """A floor above the pinned version would make the lock uninstallable."""
    assert _declared_floor("numpy") <= _locked_release("numpy")


def test_numpy_floor_shares_a_major_with_the_pinned_version() -> None:
    """The floor may trail the pin, but not across a major boundary.

    Crossing one means the declared minimum predates an intentionally breaking
    release, so the promise covers an API the package has never been run on.
    """
    floor = _declared_floor("numpy")
    locked = _locked_release("numpy")
    assert floor[0] == locked[0], (
        f"declared numpy floor {floor} is a major version below the pinned {locked}, "
        "so the oldest supported version has never been exercised"
    )


def test_the_running_numpy_satisfies_the_declared_floor() -> None:
    """The environment proving the package works must itself meet the claim."""
    assert _release(np.__version__) >= _declared_floor("numpy")
