#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/patch_tuning.h"

namespace sonare::midi::synth::detail {
namespace {

ProgramOverrides build_program_overrides() noexcept {
  ProgramOverrides o{};
  configure_keyed_programs(o);
  configure_percussion_programs(o);
  configure_physical_programs(o);

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

  return o;
}

}  // namespace

const ProgramOverrides& program_overrides() noexcept {
  static const ProgramOverrides kTable = build_program_overrides();
  return kTable;
}

}  // namespace sonare::midi::synth::detail
