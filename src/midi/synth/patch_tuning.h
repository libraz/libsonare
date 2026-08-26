#pragma once

/// @file patch_tuning.h
/// @brief Development-only per-program override of a GM fallback patch's
///        fields, the voicing counterpart to `util/tunable.h`.
/// @details `SONARE_TUNABLE` covers an engine's calibration constants, the
/// physics every program on that engine shares. What separates a violin from a
/// cello on the same bowed-string engine is the *patch* — bow force, brightness,
/// body mix — and those are literals in the GM fallback tables, so fitting one
/// otherwise costs a rebuild. `apply_patch_tuning` runs once per patch as the
/// tables are built and rewrites every float field named in
/// `SONARE_TUNING_OVERRIDES`; in a normal build it does nothing and the field
/// table is not compiled at all.
///
/// Keys are `<prefix>.<field path>`, and the prefix names the PATCH rather than
/// the program, since one patch commonly voices several. It is the
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
