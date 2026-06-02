#include "editing/voice_changer/formant_warp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/lpc.h"

namespace sonare::editing::voice_changer {

using sonare::constants::kEpsilon;
using sonare::constants::kSpectrumEpsilon;
using sonare::constants::kTwoPi;

namespace {

constexpr int kFrameSize = 1024;
constexpr int kHopSize = 256;
constexpr float kFactorMin = 0.25f;
constexpr float kFactorMax = 4.0f;

// Periodic Hann window (good COLA at 75% overlap).
std::vector<float> make_hann(int size) {
  std::vector<float> w(static_cast<size_t>(size));
  for (int i = 0; i < size; ++i) {
    w[static_cast<size_t>(i)] =
        0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) / static_cast<float>(size));
  }
  return w;
}

}  // namespace

FormantWarp::FormantWarp(FormantWarpConfig config) : config_(config) {}

Audio FormantWarp::process(const Audio& audio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.factor > 0.0f && std::isfinite(config_.factor), ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.lpc_order >= 0, ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  const size_t n = audio.size();
  const float* x = audio.data();

  // Effective warp factor folds the dry/wet amount into the shift strength.
  const float amount = std::clamp(config_.amount, 0.0f, 1.0f);
  const float factor = std::clamp(config_.factor, kFactorMin, kFactorMax);
  const float effective_factor =
      std::clamp(1.0f + (factor - 1.0f) * amount, kFactorMin, kFactorMax);

  // Effective LPC order: explicit config, else sr-based heuristic.
  const int default_order = static_cast<int>(sr / 1000) + 2;
  int order = (config_.lpc_order > 0) ? config_.lpc_order : default_order;
  order = std::max(2, std::min(order, kFrameSize - 1));

  // kiss_fftr requires an even FFT size; 2*frame_size gives zero-padding headroom.
  const int n_fft = 2 * kFrameSize;
  const int n_bins = n_fft / 2 + 1;
  const float src_max = static_cast<float>(n_bins - 1);

  const std::vector<float> hann = make_hann(kFrameSize);
  FFT fft(n_fft);

  std::vector<float> out(n, 0.0f);
  std::vector<float> norm(n, 0.0f);

  std::vector<float> windowed(static_cast<size_t>(kFrameSize));
  std::vector<float> padded(static_cast<size_t>(n_fft));
  std::vector<std::complex<float>> spec(static_cast<size_t>(n_bins));
  std::vector<float> env(static_cast<size_t>(n_bins));
  std::vector<float> warped(static_cast<size_t>(n_bins));
  std::vector<float> time_frame(static_cast<size_t>(n_fft));

  // Start so that the first frame's analysis window is centred near sample 0.
  for (long start = -kFrameSize / 2; start < static_cast<long>(n); start += kHopSize) {
    // Extract windowed frame with edge zero-padding.
    for (int i = 0; i < kFrameSize; ++i) {
      const long idx = start + i;
      const float s = (idx >= 0 && idx < static_cast<long>(n)) ? x[idx] : 0.0f;
      windowed[static_cast<size_t>(i)] = s * hann[static_cast<size_t>(i)];
    }

    const auto model = sonare::lpc_autocorrelation(windowed.data(), windowed.size(), order);

    // Degenerate frame: pass the windowed signal through OLA unchanged. The main
    // path reconstructs the analysis-windowed signal (time_frame[i] ~= s*hann[i])
    // and then applies the synthesis window, so its effective energy is the
    // already-windowed sample times one more window. Accumulating
    // `windowed[i] * win` here (s * hann^2) matches that effective window power
    // and avoids a level step at the boundary between degenerate and normal
    // frames; using the raw sample (s * win) would under-weight by one analysis
    // window factor.
    if (model.variance < kEpsilon || model.ar.size() < 2 || model.ar[0] == 0.0f) {
      for (int i = 0; i < kFrameSize; ++i) {
        const long idx = start + i;
        if (idx < 0 || idx >= static_cast<long>(n)) continue;
        const float win = hann[static_cast<size_t>(i)];
        out[static_cast<size_t>(idx)] += windowed[static_cast<size_t>(i)] * win;
        norm[static_cast<size_t>(idx)] += win * win;
      }
      continue;
    }

    const size_t ord = model.ar.size() - 1;

    // LPC residual (inverse filtering), zero-padded to n_fft.
    const std::vector<float> residual =
        sonare::lpc_residual(windowed.data(), windowed.size(), model);
    std::fill(padded.begin(), padded.end(), 0.0f);
    std::copy(residual.begin(), residual.end(), padded.begin());
    fft.forward(padded.data(), spec.data());

    // Original all-pole envelope magnitude: |H[k]| = sqrt(gain) / |A(e^{jw_k})|.
    const float gain = std::sqrt(std::max(model.variance, 0.0f));
    for (int k = 0; k < n_bins; ++k) {
      const float w = kTwoPi * static_cast<float>(k) / static_cast<float>(n_fft);
      float re = 1.0f;
      float im = 0.0f;
      for (size_t p = 1; p <= ord; ++p) {
        const float phase = -static_cast<float>(p) * w;
        re += model.ar[p] * std::cos(phase);
        im += model.ar[p] * std::sin(phase);
      }
      const float mag_a = std::sqrt(re * re + im * im);
      env[static_cast<size_t>(k)] = gain / (mag_a + kSpectrumEpsilon);
    }

    // Warp envelope by resampling along frequency (src = k' / effective_factor).
    warped[0] = env[0];  // hold DC fixed
    for (int k = 1; k < n_bins; ++k) {
      const float src = std::clamp(static_cast<float>(k) / effective_factor, 0.0f, src_max);
      const int lo = static_cast<int>(std::floor(src));
      const int hi = std::min(lo + 1, n_bins - 1);
      const float frac = src - static_cast<float>(lo);
      warped[static_cast<size_t>(k)] =
          env[static_cast<size_t>(lo)] * (1.0f - frac) + env[static_cast<size_t>(hi)] * frac;
    }

    // Re-color the whitened residual with the warped envelope: out_R[k] = R[k] * Hw[k].
    for (int k = 0; k < n_bins; ++k) {
      spec[static_cast<size_t>(k)] *= warped[static_cast<size_t>(k)];
    }

    fft.inverse(spec.data(), time_frame.data());

    // Synthesis window + OLA.
    for (int i = 0; i < kFrameSize; ++i) {
      const long idx = start + i;
      if (idx < 0 || idx >= static_cast<long>(n)) continue;
      const float win = hann[static_cast<size_t>(i)];
      out[static_cast<size_t>(idx)] += time_frame[static_cast<size_t>(i)] * win;
      norm[static_cast<size_t>(idx)] += win * win;
    }
  }

  // OLA normalization only. Do NOT clamp to [-1, 1]: formant warping reshapes
  // the spectral envelope, it is not a limiter, and hard-clipping here would add
  // nonlinear distortion to any frame whose normalized output legitimately
  // exceeds unity. Downstream limiting (if desired) is a separate stage.
  for (size_t i = 0; i < n; ++i) {
    out[i] = norm[i] > kSpectrumEpsilon ? out[i] / norm[i] : out[i];
  }

  return Audio::from_vector(std::move(out), sr);
}

}  // namespace sonare::editing::voice_changer
