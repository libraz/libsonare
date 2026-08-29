#include "midi/synth/gm_fallback_families.h"

namespace sonare::midi::synth::detail {

const std::array<NativeSynthPatch, 16>& family_patches() noexcept {
#if defined(SONARE_TUNING) && SONARE_TUNING
  // The tuning build reads overrides from the environment, so the table can
  // only be built once the process is running.
  static const std::array<NativeSynthPatch, 16> kTable = build_family_table();
#else
  static constexpr std::array<NativeSynthPatch, 16> kTable = build_family_table();
#endif
  return kTable;
}

}  // namespace sonare::midi::synth::detail
