"""What a conformance comparison does when the build it needs is missing or stale.

By default it fails, naming the command that produces the build: a comparison
that could not run reports exactly what a passing one reports otherwise, so a
skip is indistinguishable from agreement. Setting SONARE_CONFORMANCE_REPORT_ONLY=1
(on the make line, or in the environment of a build-free job) reports the same
case as a skip instead.
"""

from __future__ import annotations

import os
import unittest
from typing import NoReturn

REPORT_ONLY_ENV = "SONARE_CONFORMANCE_REPORT_ONLY"


def report_only() -> bool:
    """Whether a comparison that cannot run is reported rather than failed."""
    return os.environ.get(REPORT_ONLY_ENV) == "1"


def not_compared_message(reason: str, build: str) -> str:
    """The line naming why a comparison did not run and what would let it."""
    return f"not compared: {reason}; build with: {build}"


def not_compared(reason: str, build: str) -> NoReturn:
    """Fail a test (or a ``setUpClass``) whose comparison cannot run, or skip it when report-only."""
    message = not_compared_message(reason, build)
    if report_only():
        raise unittest.SkipTest(f"{message} ({REPORT_ONLY_ENV}=1)")
    raise AssertionError(f"{message} (set {REPORT_ONLY_ENV}=1 to report instead)")
