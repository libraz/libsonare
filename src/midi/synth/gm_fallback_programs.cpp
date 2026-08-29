#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gm_fallback_programs_keyed.h"
#include "midi/synth/gm_fallback_programs_percussion.h"
#include "midi/synth/gm_fallback_programs_physical.h"
#include "midi/synth/gm_fallback_programs_sfx.h"
#include "midi/synth/gm_fallback_programs_synth.h"
#include "midi/synth/gm_fallback_programs_variations.h"
#include "midi/synth/patch_sections.h"
#include "midi/synth/patch_tuning.h"

namespace sonare::midi::synth::detail {
namespace {

SONARE_TUNED_CONSTEXPR ProgramOverrides build_program_overrides() noexcept {
  ProgramOverrides o{};
  configure_keyed_programs(o);
  configure_percussion_programs(o);
  configure_physical_programs(o);
  configure_synth_programs(o);
  configure_sfx_programs(o);
  configure_variation_programs(o);

  // Clamp member by member off the same X-macro list the struct is declared
  // from, so a patch added to the table is clamped without touching this line.
  // The contiguous `program_override_patches` view would express the same sweep
  // as a loop, but it reaches the members through a cast that a constant
  // expression may not perform; a macro expansion stays foldable, and once the
  // whole table folds, neither form emits an instruction to be counted.
#define SONARE_GM_CLAMP_ONE(name) o.name = clamp_synth_patch(o.name);
  SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_CLAMP_ONE)
#undef SONARE_GM_CLAMP_ONE

  // Development-only per-patch voicing override, keyed by the member name
  // (`SONARE_TUNING_OVERRIDES=violin.bowed_string.bow_force=0.61`). Compiled
  // out entirely in a normal build rather than left as an inert call per patch,
  // because the WebAssembly module is under a size gate. This is not a
  // behavioural fork: with no override set the call is the identity, so both
  // configurations build the same table.
#if defined(SONARE_TUNING) && SONARE_TUNING
#define SONARE_GM_TUNE_ONE(name) apply_patch_tuning(o.name, #name);
  SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_TUNE_ONE)
#undef SONARE_GM_TUNE_ONE
#endif

  // Last, so the clamp above still bounds every field for the tuning catalogue
  // and an override still reaches the section it names.
#define SONARE_GM_STRIP_ONE(name) o.name = strip_unvoiced_sections(o.name);
  SONARE_GM_OVERRIDE_PATCHES(SONARE_GM_STRIP_ONE)
#undef SONARE_GM_STRIP_ONE

  return o;
}

}  // namespace

const ProgramOverrides& program_overrides() noexcept {
#if defined(SONARE_TUNING) && SONARE_TUNING
  // The tuning build reads overrides from the environment, so the table can
  // only be built once the process is running.
  static const ProgramOverrides kTable = build_program_overrides();
#else
  static constexpr ProgramOverrides kTable = build_program_overrides();
#endif
  return kTable;
}

}  // namespace sonare::midi::synth::detail
