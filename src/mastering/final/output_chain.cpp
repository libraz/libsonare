#include "mastering/final/output_chain.h"

#include "util/exception.h"

namespace sonare::mastering::final {

Audio output_chain(const Audio& audio, const OutputChainConfig& config,
                   size_t* non_finite_samples) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  // A dithered result already sits on the target grid, so only an undithered input still needs
  // the bit-depth stage; each stage substitutes non-finite samples and reports their count.
  if (config.dither_type == DitherType::None) {
    return bit_depth(audio, {config.target_bits, config.clamp}, non_finite_samples);
  }
  return dither(audio, {config.dither_type, config.target_bits}, non_finite_samples);
}

}  // namespace sonare::mastering::final
