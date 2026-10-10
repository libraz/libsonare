#pragma once

/// @file patch_tuning.h
/// @brief The field table over a NativeSynthPatch: the development-only
///        per-program override of a GM fallback patch's fields, and the public
///        string-keyed access to an engine section built on the same walk.
/// @details `SONARE_TUNABLE` covers an engine's calibration constants, the
/// physics every program on that engine shares. What separates a violin from a
/// cello on the same bowed-string engine is the *patch* — bow force, brightness,
/// body mix — and one walk over its fields serves two callers. In a tuning build
/// `apply_patch_tuning` rewrites every field named in `SONARE_TUNING_OVERRIDES`
/// as the fallback tables are built; in a normal build it does nothing. In every
/// build `apply_engine_params` and `engine_param_descriptors` reach the selected
/// engine's own section by public key, with ranges measured through
/// `clamp_synth_patch` rather than mirrored from it.
///
/// Tuning keys are `<prefix>.<field path>`, and the prefix names the PATCH rather
/// than the program, since one patch commonly voices several. It is the
/// `ProgramOverrides` member name, `famN` for a family patch, or `dNNN` for a
/// drum note:
///
///     violin.bowed_string.bow_force=0.61       # the violin patch (program 40)
///     church_organ.pipe_organ.ranks2.level=0.72
///     church_organ.amp_env.release_ms=900      # the shared envelope section
///     fam0.piano.brightness=0.55               # GM family 0 (programs 0-7)
///     d038.percussion.tone_gain=0.4            # drum note 38 (acoustic snare)
///
/// The match is exact and an absent key keeps its compiled-in default without a
/// diagnostic, so a misspelled prefix is a silent no-op. The field path mirrors
/// the C++ member names and is greppable in the engine header; an array member
/// appends its index (`ranks2`, `ops1`) because `[` and `]` would need quoting in
/// an environment variable. Only the section matching the patch's engine mode is
/// offered, so a key naming the wrong engine is never asked for.
///
/// A public engine-param key is the engine-section path without its first
/// segment, each remaining segment in lowerCamelCase: `bowed_string.bow_force`
/// is `bowForce`, `pipe_organ.ranks2.level` is `ranks2.level`, `ks.decay_s` is
/// `decayS`. The common (wrapper) section has no public key here.

#include <cstddef>
#include <string>
#include <vector>

#include "mastering/api/param_meta.h"

namespace sonare::midi::synth {

struct NativeSynthPatch;

/// Apply `SONARE_TUNING_OVERRIDES` entries prefixed with @p prefix to @p patch.
/// No-op in a normal build. Called while the fallback tables are built, never
/// on the audio thread.
void apply_patch_tuning(NativeSynthPatch& patch, const char* prefix) noexcept;

/// One engine-section field set by public key.
struct EngineParam {
  const char* key;
  double value;
};

/// Assign @p count engine-section fields of @p patch's engine by public key.
///
/// All or nothing: the first refusal leaves @p patch untouched, writes
/// `engine param '<key>': <reason>` (followed by ` [<lo>, <hi>]` where the field
/// has a range) to @p error when it is non-null, and returns false. Refused: a
/// null or unknown key, a key outside this engine's section (the common section
/// included), a key given twice, a non-finite value, a fractional value for an
/// integer or boolean field, and a value outside the field's measured range or
/// outside what the field's type can hold. Nothing is clamped. @p count 0 is a
/// no-op returning true. Not for the audio thread.
bool apply_engine_params(NativeSynthPatch& patch, const EngineParam* params, size_t count,
                         std::string* error);

/// What an engine-section field accepts, in walk order.
struct EngineParamDescriptor {
  /// Public key (see the file comment).
  std::string key;
  /// Whole numbers only; false for a boolean, which sets @ref boolean instead.
  bool integer;
  /// A switch: 0 or 1.
  bool boolean;
  /// Whether @ref lo and @ref hi hold the accepted range; when false the field
  /// takes any finite value and @ref lo / @ref hi are 0.
  bool bounded;
  float lo, hi;
  mastering::api::detail::Unit unit;
  /// The field's value on the patch the descriptors were taken from.
  float value;
};

/// One descriptor per field of @p base's engine section (the common section
/// excluded); empty for an engine without a section. Not for the audio thread.
std::vector<EngineParamDescriptor> engine_param_descriptors(const NativeSynthPatch& base);

namespace patch_tuning_detail {

/// One engine-section field as the walker visits it, for the tests of the key
/// and unit rules.
struct EngineFieldSite {
  std::string path;
  /// @ref path with the index removed where the walker built the path from an
  /// array index (`percussion.shell_freq_hz3` -> `percussion.shell_freq_hz`).
  std::string member_path;
  bool indexed;
  bool has_unit;
};

/// Every engine-section field the walker visits for @p patch's engine.
std::vector<EngineFieldSite> engine_field_sites(const NativeSynthPatch& patch);

}  // namespace patch_tuning_detail

#if defined(SONARE_TUNING) && SONARE_TUNING
/// Every field path `apply_patch_tuning` offers for @p patch's engine (the
/// `<prefix>.` omitted), in walk order. Reads the same field table the override
/// layer walks, so a section that forgets a field reports one here too — which
/// is what makes "every clamped field of the engine is reachable by exactly one
/// key" checkable instead of a claim. Not compiled into a normal build.
std::vector<std::string> patch_tuning_field_paths(const NativeSynthPatch& patch);
#endif

}  // namespace sonare::midi::synth
