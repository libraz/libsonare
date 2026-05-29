#include "mastering/match/reference_loudness.h"

#include <cmath>
#include <stdexcept>

// TODO(layer-violation): CLAUDE.md restricts `mastering/` (non-assistant) to
// `core/ + util/ + rt/`, yet `reference_loudness` is intrinsically a LUFS
// measurement and so has no implementation path that avoids `metering/lufs.h`.
// Proper fixes (pick one):
//   1. Relocate this entire helper under `mastering/assistant/` (the only
//      sub-module allowed to reach into `metering/`).
//   2. Change the public signature to take pre-measured LUFS values as
//      parameters, pushing the measurement up to the C API / WASM bridge layer
//      that links against `metering/` directly.
// Until that is decided, this include is the only remaining layer-rule
// violation in this translation unit.
#include "metering/lufs.h"

namespace sonare::mastering::match {

ReferenceLoudness reference_loudness(const Audio& source, const Audio& reference) {
  if (source.empty() || reference.empty()) {
    throw std::invalid_argument("audio must not be empty");
  }
  const float source_lufs = metering::lufs(source).integrated_lufs;
  const float reference_lufs = metering::lufs(reference).integrated_lufs;
  const float gain = std::isfinite(source_lufs) && std::isfinite(reference_lufs)
                         ? reference_lufs - source_lufs
                         : 0.0f;
  return {source_lufs, reference_lufs, gain};
}

}  // namespace sonare::mastering::match
