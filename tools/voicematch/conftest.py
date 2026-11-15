"""Shared test setup for the voicematch harness tests."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo


@pytest.fixture(autouse=True)
def _a_clean_tree_for_gate_writes(monkeypatch):
    """Gate writes refuse a dirty tree; tests write scratch gates beside concurrent edits."""
    import profile_gate

    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "test", "dirty": []})


@pytest.fixture(autouse=True)
def _no_harness_lock_leak():
    """A test that reaches a CLI entry point locks the harness; none may leave it locked."""
    import harness_lock

    yield
    harness_lock.unlock()
