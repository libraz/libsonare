#pragma once

#include <cstdint>

#include "core/audio.h"

namespace sonare::mastering::final {

/// @brief Dither noise shape added before quantization.
/// @details @c None passes the input through unquantized (only non-finite
///          samples are sanitized). Every other mode adds its noise and then
///          rounds to the @ref DitherConfig::target_bits grid, so the choice of
///          mode changes the noise, never the output resolution.
enum class DitherType { None, Rpdf, Tpdf, NoiseShaped };

struct DitherConfig {
  DitherType type = DitherType::Tpdf;
  /// @brief Word length the output is quantized to, in [2, 32].
  /// @details Interpreted identically by every non-@c None mode; the grid step
  ///          is 2^-(target_bits - 1) and output stays within [-1, 1 - step].
  int target_bits = 16;
  uint32_t seed = 0x51A7E5u;
};

/// @brief Dithers and quantizes @p audio to @c config.target_bits.
/// @details A separate @ref bit_depth pass over the result is a no-op: the
///          samples are already on that grid. @ref output_chain runs both so a
///          @c None dither still reaches the target word length.
Audio dither(const Audio& audio, const DitherConfig& config = {});

}  // namespace sonare::mastering::final
