"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

from ._ffi_types_analysis import *  # noqa: F403
from ._ffi_types_core import *  # noqa: F403
from ._ffi_types_mastering_project import *  # noqa: F403
from ._ffi_types_repair import *  # noqa: F403
from ._ffi_types_streaming import *  # noqa: F403

# Do not synthesize ``__all__`` here.  ctypes signature modules import this
# aggregate with ``*`` and mypy cannot resolve a dynamically-computed export
# list, which made every generated C struct appear undefined to strict checks.
# `_ffi.py` keeps the runtime's intentionally narrow public export list.
