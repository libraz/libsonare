#include "mastering/final/output_chain.h"

#include <cstddef>

#include "util/exception.h"

namespace sonare::mastering::final {

Audio output_chain(const Audio& audio, const OutputChainConfig& config,
                   size_t* non_finite_samples) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  size_t dither_non_finite = 0;
  size_t bit_depth_non_finite = 0;
  Audio processed = dither(audio, {config.dither_type, config.target_bits}, &dither_non_finite);
  Audio result = bit_depth(processed, {config.target_bits, config.clamp}, &bit_depth_non_finite);
  if (non_finite_samples != nullptr) {
    *non_finite_samples = dither_non_finite + bit_depth_non_finite;
  }
  return result;
}

}  // namespace sonare::mastering::final
