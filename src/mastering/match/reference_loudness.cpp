#include "mastering/match/reference_loudness.h"

#include <cmath>
#include <stdexcept>

#include "analysis/meter/lufs.h"

namespace sonare::mastering::match {

ReferenceLoudness reference_loudness(const Audio& source, const Audio& reference) {
  if (source.empty() || reference.empty()) {
    throw std::invalid_argument("audio must not be empty");
  }
  const float source_lufs = analysis::meter::lufs(source).integrated_lufs;
  const float reference_lufs = analysis::meter::lufs(reference).integrated_lufs;
  const float gain = std::isfinite(source_lufs) && std::isfinite(reference_lufs)
                         ? reference_lufs - source_lufs
                         : 0.0f;
  return {source_lufs, reference_lufs, gain};
}

}  // namespace sonare::mastering::match
