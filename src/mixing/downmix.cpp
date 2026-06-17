#include "mixing/downmix.h"

#include <algorithm>

#include "util/exception.h"

namespace sonare::mixing {

void downmix(ChannelLayout from, ChannelLayout to, const float* const* in, float* const* out,
             size_t n, const DownmixOptions& options) {
  using downmix_coeff::kMinus3dB;

  const int nin = channel_count(from);
  const int nout = channel_count(to);
  SONARE_CHECK_MSG(in != nullptr && out != nullptr, ErrorCode::InvalidParameter,
                   "downmix: null plane array");
  SONARE_CHECK_MSG(nout <= nin, ErrorCode::InvalidParameter, "downmix cannot upmix");

  if (from == to) {
    for (int c = 0; c < nin; ++c) std::copy(in[c], in[c] + n, out[c]);
    return;
  }

  // -3 dB LFE feed when folding it in; 0 (dropped) by default per BS.775.
  const float lfe = options.include_lfe ? kMinus3dB : 0.0f;

  // Folds a surround source frame down to a front L/R pair (BS.775).
  const auto fold_stereo = [&](size_t i, float& lo, float& ro) {
    if (from == ChannelLayout::FivePointOne) {
      // L R C LFE Ls Rs
      lo = in[0][i] + kMinus3dB * in[2][i] + kMinus3dB * in[4][i] + lfe * in[3][i];
      ro = in[1][i] + kMinus3dB * in[2][i] + kMinus3dB * in[5][i] + lfe * in[3][i];
    } else {
      // 7.1: L R C LFE Lss Rss Ls Rs — side+back combined into each surround feed.
      lo = in[0][i] + kMinus3dB * in[2][i] + kMinus3dB * (in[6][i] + in[4][i]) + lfe * in[3][i];
      ro = in[1][i] + kMinus3dB * in[2][i] + kMinus3dB * (in[7][i] + in[5][i]) + lfe * in[3][i];
    }
  };

  // Worst-case (all inputs at full scale, in phase) peak of one folded front
  // channel, used as the normalization divisor.
  const auto stereo_peak = [&]() {
    const float surround = (from == ChannelLayout::SevenPointOne) ? 2.0f : 1.0f;
    return 1.0f + kMinus3dB * (1.0f + surround) + lfe;
  };

  // 7.1 -> 5.1: pass L R C LFE through, fold side+back into the surrounds.
  if (to == ChannelLayout::FivePointOne && from == ChannelLayout::SevenPointOne) {
    const float surround_gain = options.normalize ? 0.5f : 1.0f;
    for (size_t i = 0; i < n; ++i) {
      out[0][i] = in[0][i];
      out[1][i] = in[1][i];
      out[2][i] = in[2][i];
      out[3][i] = in[3][i];
      out[4][i] = surround_gain * (in[6][i] + in[4][i]);  // Ls = Ls + Lss
      out[5][i] = surround_gain * (in[7][i] + in[5][i]);  // Rs = Rs + Rss
    }
    return;
  }

  // 5.1 / 7.1 -> stereo.
  if (to == ChannelLayout::Stereo &&
      (from == ChannelLayout::FivePointOne || from == ChannelLayout::SevenPointOne)) {
    const float gain = options.normalize ? 1.0f / stereo_peak() : 1.0f;
    for (size_t i = 0; i < n; ++i) {
      float lo = 0.0f;
      float ro = 0.0f;
      fold_stereo(i, lo, ro);
      out[0][i] = gain * lo;
      out[1][i] = gain * ro;
    }
    return;
  }

  // Anything -> mono.
  if (to == ChannelLayout::Mono) {
    if (from == ChannelLayout::Stereo) {
      for (size_t i = 0; i < n; ++i) out[0][i] = 0.5f * (in[0][i] + in[1][i]);
      return;
    }
    // Surround -> mono = stereo fold, then average.
    const float gain = options.normalize ? 1.0f / stereo_peak() : 1.0f;
    for (size_t i = 0; i < n; ++i) {
      float lo = 0.0f;
      float ro = 0.0f;
      fold_stereo(i, lo, ro);
      out[0][i] = 0.5f * gain * (lo + ro);
    }
    return;
  }

  SONARE_CHECK_MSG(false, ErrorCode::InvalidParameter, "downmix: unsupported layout conversion");
}

}  // namespace sonare::mixing
