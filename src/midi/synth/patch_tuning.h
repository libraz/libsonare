#pragma once

/// @file patch_tuning.h
/// @brief Development-only per-program override of a GM fallback patch's
///        fields, the voicing counterpart to `util/tunable.h`.
/// @details `SONARE_TUNABLE` covers an engine's calibration constants — the
/// physics shared by every program that engine serves. What differs between,
/// say, a violin and a cello on the same bowed-string engine is the *patch*:
/// bow force, brightness, body mix. Those live as literals in the GM fallback
/// program tables, so fitting one costs a rebuild.
///
/// `apply_patch_tuning` closes that gap. In a `BUILD_TUNING=ON` build it is
/// called once per patch as the fallback tables are built, and rewrites every
/// float field whose key appears in `SONARE_TUNING_OVERRIDES`. In a normal
/// build it does nothing (and the field table is not compiled at all).
///
/// Keys are `<prefix>.<field path>`, where the prefix identifies the PATCH, not
/// the program. One patch commonly voices several GM programs, so a program
/// number would not name it. The three prefix forms are the `ProgramOverrides`
/// member name for a named melodic patch, `famN` for a family patch, and `dNNN`
/// for a drum note:
///
///     violin.bowed_string.bow_force=0.61       # the violin patch (program 40)
///     church_organ.pipe_organ.ranks2.level=0.72
///     church_organ.amp_env.release_ms=900      # the shared envelope section
///     fam0.piano.brightness=0.55               # GM family 0 (programs 0-7)
///     d038.percussion.tone_gain=0.4            # drum note 38 (acoustic snare)
///
/// The match is exact, and a key absent from the table keeps its compiled-in
/// default without a diagnostic, so a misspelled prefix is a silent no-op.
///
/// The field path mirrors the C++ member names, so a key is greppable in the
/// engine header it came from. Array members are indexed by appending the
/// index to the member name (`ranks2`, `ops1`, `modes0`, `drawbars3`) because
/// `[` and `]` would need quoting in a shell-set environment variable.
///
/// Only the section matching the patch's engine mode is offered, so a key
/// naming the wrong engine for that program is simply never asked for — the
/// fitter validates its knob names against the engine, as it does for
/// `SONARE_TUNABLE`.

#if defined(SONARE_TUNING) && SONARE_TUNING
#include <string>
#include <vector>
#endif

namespace sonare::midi::synth {

struct NativeSynthPatch;

/// Apply `SONARE_TUNING_OVERRIDES` entries prefixed with @p prefix to @p patch.
/// No-op in a normal build. Called while the fallback tables are built, never
/// on the audio thread.
void apply_patch_tuning(NativeSynthPatch& patch, const char* prefix) noexcept;

#if defined(SONARE_TUNING) && SONARE_TUNING
/// Every field path `apply_patch_tuning` offers for @p patch's engine (the
/// `<prefix>.` omitted), in walk order. Reads the same field table the override
/// layer walks, so a section that forgets a field reports one here too — which
/// is what makes "every clamped field of the engine is reachable by exactly one
/// key" checkable instead of a claim. Not compiled into a normal build.
std::vector<std::string> patch_tuning_field_paths(const NativeSynthPatch& patch);
#endif

}  // namespace sonare::midi::synth
