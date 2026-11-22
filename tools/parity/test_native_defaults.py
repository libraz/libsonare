"""Regression tests for the native fallback-default check.

The Node addon and the embind layer substitute a literal for an absent value.
The check follows each literal to the C++ default it stands in for and compares
the two. Each path the walker takes is pinned on a small synthetic tree, with a
control that can go the other way: a planted mismatch must be found and its
corrected twin must not, a guarded sentinel must be declined with its reason,
each C-ABI hop must land on the C++ default behind it, and a run that reaches
no verdict in the category must be named by the zero-verdict gate.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_native_defaults.py
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod
import check_parity
import compare
from extractors import native_defaults
from model import SURFACES

_CORE_HEADER = """
namespace sonare {
struct ThingConfig {
  int k = 5;
  float gain = 1.0f;
  bool smooth = true;
};
int run_thing(const float* x, int n, int width = 9);
}  // namespace sonare
"""

_C_API = """
SonareError sonare_thing(const SonareThingOptions* options) {
  ThingConfig config;
  config.k = options->k;
  return SONARE_OK;
}

static void apply_options(const SonareThingOptions& options, ThingConfig& config) {
  config.smooth = options.smooth != 0;
}

SonareError sonare_width(const float* x, int n, int width) {
  run_thing(x, n, width);
  return SONARE_OK;
}
"""

_ADDON = """
Napi::Value Direct(const Napi::CallbackInfo& info) {
  sonare::ThingConfig config;
  int k{};
  if (!OptionalIntArg(env, info, 0, "k", K_LITERAL, &k)) return env.Undefined();
  config.k = k;
  return env.Undefined();
}

Napi::Value Guarded(const Napi::CallbackInfo& info) {
  sonare::ThingConfig config;
  float gain{};
  if (!OptionalFloatArg(env, info, 0, "gain", 0.0f, &gain)) return env.Undefined();
  if (gain != 0) config.gain = gain;
  return env.Undefined();
}

Napi::Value ThroughCStruct(const Napi::CallbackInfo& info) {
  SonareThingOptions options{};
  options.k = IntProperty(obj, "kViaC", 5);
  sonare_thing(&options);
  return env.Undefined();
}

Napi::Value ThroughBoolHelper(const Napi::CallbackInfo& info) {
  SonareThingOptions options{};
  bool smooth{};
  if (!OptionalBoolArg(env, info, 0, "smooth", SMOOTH_LITERAL, &smooth)) return env.Undefined();
  options.smooth = smooth ? 1 : 0;
  sonare_thing(&options);
  return env.Undefined();
}

Napi::Value PresenceGuarded(const Napi::CallbackInfo& info) {
  sonare::ThingConfig config;
  const Napi::Value given = options.Get("guardedK");
  if (!given.IsUndefined() && !given.IsNull()) {
    config.k = IntProperty(options, "guardedK", 0);
  }
  return env.Undefined();
}

Napi::Value Unguarded(const Napi::CallbackInfo& info) {
  sonare::ThingConfig config;
  const Napi::Value given = options.Get("otherKey");
  if (!given.IsUndefined()) {
    config.k = IntProperty(options, "unguardedK", 5);
  }
  return env.Undefined();
}

Napi::Value ThroughCArg(const Napi::CallbackInfo& info) {
  int width{};
  if (!OptionalIntArg(env, info, 1, "width", 9, &width)) return env.Undefined();
  sonare_width(nullptr, 0, width);
  return env.Undefined();
}
"""


def _tree(root: Path, k_literal: str = "7", smooth_literal: str = "false") -> Path:
    (root / "src/core").mkdir(parents=True)
    (root / "src/util").mkdir(parents=True)
    (root / "src/c_api").mkdir(parents=True)
    (root / "src/wasm").mkdir(parents=True)
    (root / "include").mkdir(parents=True)
    (root / "bindings/node/src/addon").mkdir(parents=True)
    (root / "src/core/thing.h").write_text(_CORE_HEADER)
    (root / "src/util/constants.h").write_text("")
    (root / "src/c_api/thing.cpp").write_text(_C_API)
    (root / "bindings/node/src/addon/thing.cpp").write_text(
        _ADDON.replace("K_LITERAL", k_literal).replace("SMOOTH_LITERAL", smooth_literal)
    )
    return root


def _sites(root: Path) -> dict[str, native_defaults.NativeDefault]:
    sites, _constants = native_defaults.extract(root)
    return {f"{s.function}.{s.param}": s for s in sites}


def _report(root: Path) -> compare.Report:
    sites, constants = native_defaults.extract(root)
    rep = compare.Report(surfaces=list(SURFACES), allowlist=allowlist_mod.Allowlist())
    compare._native_default_drift(sites, constants, rep.allowlist, rep)
    return rep


def _active(rep: compare.Report) -> list[str]:
    return [f.key for f in rep.active() if f.category == "native_default"]


def test_a_planted_mismatch_is_found() -> None:
    with tempfile.TemporaryDirectory() as d:
        rep = _report(_tree(Path(d), k_literal="7"))
    assert _active(rep) == ["Direct.k", "ThroughBoolHelper.smooth"], _active(rep)


def test_the_corrected_twin_is_compared_and_agrees() -> None:
    with tempfile.TemporaryDirectory() as d:
        rep = _report(_tree(Path(d), k_literal="5", smooth_literal="true"))
    assert _active(rep) == []
    # Agreement is a verdict, not a silence: the category reached it.
    assert rep.comparison_counts().get("native_default", 0) >= 1


def test_a_guarded_zero_is_declined_as_a_sentinel() -> None:
    with tempfile.TemporaryDirectory() as d:
        site = _sites(_tree(Path(d)))["Guarded.gain"]
    assert site.unresolved == native_defaults.GUARDED_SENTINEL, site


def test_a_fallback_read_only_when_its_key_is_present_reaches_no_verdict() -> None:
    with tempfile.TemporaryDirectory() as d:
        sites = _sites(_tree(Path(d)))
    guarded, unguarded = sites["PresenceGuarded.guardedK"], sites["Unguarded.unguardedK"]
    assert guarded.unresolved == native_defaults.PRESENCE_GUARDED, guarded
    # A guard on a different key does not decline the read: it is compared and agrees.
    assert (unguarded.anchor, unguarded.core, unguarded.unresolved) == ("ThingConfig.k", "5", "")


def test_a_c_struct_field_lands_on_the_cpp_field_the_c_abi_copies_it_into() -> None:
    with tempfile.TemporaryDirectory() as d:
        site = _sites(_tree(Path(d)))["ThroughCStruct.kViaC"]
    assert (site.anchor, site.core, site.unresolved) == ("ThingConfig.k", "5", ""), site


def test_a_bool_spelled_as_an_int_lands_on_a_field_a_c_abi_helper_copies_it_into() -> None:
    with tempfile.TemporaryDirectory() as d:
        site = _sites(_tree(Path(d)))["ThroughBoolHelper.smooth"]
    assert (site.anchor, site.core, site.unresolved) == ("ThingConfig.smooth", "true", ""), site


def test_a_c_abi_argument_lands_on_the_default_argument_its_implementation_passes_it_to() -> None:
    with tempfile.TemporaryDirectory() as d:
        site = _sites(_tree(Path(d)))["ThroughCArg.width"]
    assert (site.anchor, site.core, site.unresolved) == ("run_thing#2", "9", ""), site


# Sites in this tree where the native fallback is known to differ from the core
# default but the walker cannot reach a verdict. Each stays pinned to its decline
# reason, so a walker change that starts comparing one shows up here.
_KNOWN_BLIND = {
    ("bindings/node/src/addon/sonare_wrap_features.cpp", "Mfcc", "nMfcc"): (
        "the value is passed to a function with no C++ declaration in a header"
    ),
    ("bindings/node/src/addon/features/rhythm.cpp", "OnsetStrengthMulti", "nBands"): (
        "value passed to several calls whose defaults differ or are missing"
    ),
    ("bindings/node/src/addon/sonare_wrap_harmony.cpp", "DetectChords", "chromaMethod"): (
        native_defaults.C_FIELD_SWITCHED
    ),
    ("bindings/node/src/addon/sonare_wrap_harmony.cpp", "FunctionalAnalysis", "chromaMethod"): (
        native_defaults.C_FIELD_SWITCHED
    ),
}


def test_known_blind_sites_in_this_tree_stay_declined_with_their_reason() -> None:
    sites, _constants = native_defaults.extract(_HERE.parent.parent)
    found = {(s.file, s.function, s.param): s.unresolved for s in sites}
    for key, reason in _KNOWN_BLIND.items():
        assert key in found, f"{key} is no longer a fallback site; update _KNOWN_BLIND"
        assert found[key] == reason, f"{key}: {found[key]!r}"


def test_a_category_that_reached_no_verdict_is_named() -> None:
    rep = compare.Report(surfaces=list(SURFACES), allowlist=allowlist_mod.Allowlist())
    compare._native_default_drift([], {}, rep.allowlist, rep)
    assert "native_default" in check_parity._blind_categories(rep, list(SURFACES))


def test_the_category_is_held_at_zero_allowlist_entries() -> None:
    assert "native_default" in allowlist_mod.RATCHETED_SECTIONS


def _main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
