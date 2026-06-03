#!/usr/bin/env python3
"""Regression tests for the TypeScript facade extractor.

Focus: ``ts_common.extract_ts`` must follow ``export ... from './module'``
re-exports transitively (cycle-safe, deduped) so that free functions and class
methods spread across sibling modules are all counted as part of the surface —
not just what physically lives in ``index.ts`` / ``*_gen.ts``. Before this was
fixed the checker reported ~200 phantom coverage gaps.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_ts_reexport.py
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from extractors.ts_common import _reexport_closure, extract_ts  # noqa: E402


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def _keys(ex) -> set[str]:
    return {f.key for f in ex.functions}


def _raw_names(ex) -> set[str]:
    return {f.raw_name for f in ex.functions}


def test_transitive_reexport_following() -> None:
    """``index -> features -> feature_spectral`` chain is fully resolved."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        _write(
            root,
            "bindings/x/src/index.ts",
            "export * from './features.js';\n"
            "export { mastering } from './effects';\n"
            "export type { Foo } from './types';\n",
        )
        _write(
            root,
            "bindings/x/src/features.ts",
            "export { cqt } from './feature_spectral';\n"
            "export function chroma(samples: Float32Array): Float32Array {\n"
            "  return samples;\n}\n",
        )
        _write(
            root,
            "bindings/x/src/feature_spectral.ts",
            "export function cqt(\n"
            "  samples: Float32Array,\n"
            "  hopLength = 512,\n"
            "): Float32Array {\n  return samples;\n}\n",
        )
        _write(
            root,
            "bindings/x/src/effects.ts",
            "export function mastering(samples: Float32Array): Float32Array {\n"
            "  return samples;\n}\n",
        )
        _write(root, "bindings/x/src/types.ts", "export type Foo = number;\n")

        ex = extract_ts(root, "node", "bindings/x/src/index.ts", "bindings/x/src/generated")
        keys = _keys(ex)
        # Deep re-export chain (index -> features -> feature_spectral) is followed.
        assert "cqt" in keys, keys
        assert "chroma" in keys, keys
        assert "mastering" in keys, keys


def test_class_method_in_reexported_module() -> None:
    """A class facade in a re-exported module contributes its methods."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        _write(root, "bindings/x/src/index.ts", "export { Audio } from './audio';\n")
        _write(
            root,
            "bindings/x/src/audio.ts",
            "export class Audio {\n"
            "  resample(targetSr: number): Float32Array {\n"
            "    return new Float32Array();\n"
            "  }\n}\n",
        )
        ex = extract_ts(root, "wasm", "bindings/x/src/index.ts", "bindings/x/src/generated")
        assert "resample" in _keys(ex), _keys(ex)
        assert "Audio.resample" in _raw_names(ex), _raw_names(ex)


def test_cycle_safe_and_deduped() -> None:
    """A re-export cycle terminates, and a symbol reached twice is deduped."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        # index <-> a form a cycle; `shared` is reachable from both.
        _write(
            root,
            "bindings/x/src/index.ts",
            "export * from './a';\nexport * from './shared';\n",
        )
        _write(
            root,
            "bindings/x/src/a.ts",
            "export * from './index';\nexport * from './shared';\n",
        )
        _write(
            root,
            "bindings/x/src/shared.ts",
            "export function helper(x: number): number {\n  return x;\n}\n",
        )
        # Should not hang and should record `helper` exactly once.
        ex = extract_ts(root, "node", "bindings/x/src/index.ts", "bindings/x/src/generated")
        helpers = [f for f in ex.functions if f.key == "helper"]
        assert len(helpers) == 1, helpers


def test_internal_module_not_globbed() -> None:
    """A module NOT re-exported from index is excluded (no full-tree glob)."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        _write(root, "bindings/x/src/index.ts", "export * from './public';\n")
        _write(
            root,
            "bindings/x/src/public.ts",
            "export function publicFn(x: number): number {\n  return x;\n}\n",
        )
        # internal.ts exports a function but is never re-exported from index.
        _write(
            root,
            "bindings/x/src/internal.ts",
            "export function internalOnly(x: number): number {\n  return x;\n}\n",
        )
        ex = extract_ts(root, "node", "bindings/x/src/index.ts", "bindings/x/src/generated")
        keys = _keys(ex)
        assert "public_fn" in keys, keys
        assert "internal_only" not in keys, keys


def test_js_extension_resolves_to_ts() -> None:
    """``./m.js`` and ``./m`` both resolve onto ``m.ts``."""
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        idx = root / "bindings/x/src/index.ts"
        _write(root, "bindings/x/src/index.ts", "export * from './m.js';\n")
        _write(root, "bindings/x/src/m.ts", "export function fn(x: number): number {\n  return x;\n}\n")
        closure = _reexport_closure(idx.resolve())
        names = {p.name for p in closure}
        assert "m.ts" in names, names


def test_real_repo_surface_is_substantial() -> None:
    """On the real repo, node/wasm must each cover a large share of the C API.

    This is the anti-regression guard for the original bug (node 64/396,
    wasm 5/396 phantom undercount). We don't pin exact numbers (the surface
    grows), only that re-export following yields a realistic, large surface.
    """
    repo = _HERE.parent.parent
    node_idx = repo / "bindings/node/src/index.ts"
    wasm_idx = repo / "bindings/wasm/src/index.ts"
    if not node_idx.exists() or not wasm_idx.exists():
        return  # not in the libsonare tree; skip
    node = extract_ts(repo, "node", "bindings/node/src/index.ts", "bindings/node/src/generated")
    wasm = extract_ts(repo, "wasm", "bindings/wasm/src/index.ts", "bindings/wasm/src/generated")
    assert len(node.functions) > 200, len(node.functions)
    assert len(wasm.functions) > 200, len(wasm.functions)
    # Sample symbols that live behind re-export chains / class facades.
    for k in ("cqt", "decompose", "nn_filter", "phase_vocoder"):
        assert k in _keys(wasm), (k, "missing from wasm")
    assert "cqt" in _keys(node), "cqt missing from node"


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:  # noqa: PERF203
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
