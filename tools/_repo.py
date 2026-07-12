"""Shared repository-root and public-header path helpers for ``tools/`` scripts.

Standalone tools under ``tools/`` repeatedly re-derive the repository root with
``Path(__file__).resolve().parents[N]`` and hardcode ``include/sonare/<name>.h``
paths. Centralizing them here keeps those paths correct across directory moves.

NOTE: ``tools/parity`` deliberately does NOT import this module -- it advertises
a stdlib-only, self-contained surface (see ``tools/parity/check_parity.py``) and
keeps its own ``_repo_root`` helper.
"""

from __future__ import annotations

from pathlib import Path

# tools/_repo.py -> the repository root is the directory above tools/.
REPO_ROOT = Path(__file__).resolve().parents[1]

# Public C-ABI headers live under include/sonare/.
INCLUDE_DIR = REPO_ROOT / "include"


def public_header(name: str) -> Path:
    """Return the path to the public C-ABI header ``include/sonare/<name>``."""
    return INCLUDE_DIR / "sonare" / name
