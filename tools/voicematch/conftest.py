"""Shared test setup for the voicematch harness tests."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))


@pytest.fixture(autouse=True)
def _a_clean_tree_for_gate_writes(monkeypatch):
    """Gate writes refuse a dirty tree; tests write scratch gates beside other sessions' edits."""
    import profile_gate

    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "test", "dirty": []})
