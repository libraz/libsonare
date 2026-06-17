#include "effects/spectral_edit.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief True if n is a power of two and >= 2.
bool is_power_of_two(int n) { return n >= 2 && (n & (n - 1)) == 0; }

/// @brief Resolved bin/frame window for one region op (inclusive on both edges).
struct RegionWindow {
  int bin_lo;
  int bin_hi;
  int frame_lo;
  int frame_hi;
  bool empty;  ///< True when the region maps to no bins or no frames.
};

/// @brief Maps a region op's time/frequency rectangle to an inclusive bin/frame window.
RegionWindow resolve_window(const SpectralRegionOp& op, int n_bins, int n_frames, int n_fft,
                            int hop_length, int sample_rate, int64_t length) {
  RegionWindow w{0, 0, 0, 0, true};

  // Frequency -> bin. bin spacing = sample_rate / n_fft; n_bins = n_fft/2 + 1.
  const double nyquist = static_cast<double>(sample_rate) / 2.0;
  const double bin_scale = static_cast<double>(n_fft) / static_cast<double>(sample_rate);
  int bin_lo = op.low_hz <= 0.0f
                   ? 0
                   : static_cast<int>(std::ceil(static_cast<double>(op.low_hz) * bin_scale));
  int bin_hi = (op.high_hz <= 0.0f || static_cast<double>(op.high_hz) >= nyquist)
                   ? n_bins - 1
                   : static_cast<int>(std::floor(static_cast<double>(op.high_hz) * bin_scale));
  bin_lo = std::clamp(bin_lo, 0, n_bins - 1);
  bin_hi = std::clamp(bin_hi, 0, n_bins - 1);

  // Time -> frame. center=true => frame f is centered near input sample f*hop.
  int64_t start = std::clamp<int64_t>(op.start_sample, 0, length);
  int64_t end = std::clamp<int64_t>(op.end_sample, 0, length);
  if (end <= start || bin_lo > bin_hi) {
    return w;  // empty region: nothing to apply.
  }
  int frame_lo = static_cast<int>(start / hop_length);
  int frame_hi = static_cast<int>(std::ceil(static_cast<double>(end) / hop_length));
  frame_lo = std::clamp(frame_lo, 0, n_frames - 1);
  frame_hi = std::clamp(frame_hi, 0, n_frames - 1);
  if (frame_lo > frame_hi) {
    return w;
  }

  w = {bin_lo, bin_hi, frame_lo, frame_hi, false};
  return w;
}

/// @brief Heals a single bin's gap [frame_lo, frame_hi] by continuing the tonal
/// component from the neighbouring unmasked frames.
/// @details A click/cough is broadband and short; the true signal under it is
/// usually tonal/voiced with a per-frame phase advance. Averaging the complex
/// bins (frozen phase) would cancel that tone, so instead the magnitude is
/// interpolated across the gap while the phase is extrapolated from the left
/// neighbour's instantaneous per-hop phase advance (phase-vocoder style). The
/// nominal bin-center advance is the fallback when only one frame is available.
void heal_bin(std::vector<std::complex<float>>& buf, int n_frames, int base, int frame_lo,
              int frame_hi, int radius, float nominal_dphi) {
  // Gather left/right neighbour statistics (mean magnitude + mean phase slope).
  float mag_left = 0.0f;
  int n_left = 0;
  for (int k = 1; k <= radius; ++k) {
    const int f = frame_lo - k;
    if (f < 0) break;
    mag_left += std::abs(buf[base + f]);
    ++n_left;
  }
  float mag_right = 0.0f;
  int n_right = 0;
  for (int k = 1; k <= radius; ++k) {
    const int f = frame_hi + k;
    if (f >= n_frames) break;
    mag_right += std::abs(buf[base + f]);
    ++n_right;
  }
  if (n_left == 0 && n_right == 0) {
    for (int f = frame_lo; f <= frame_hi; ++f) {
      buf[base + f] = std::complex<float>(0.0f, 0.0f);
    }
    return;
  }
  if (n_left > 0) mag_left /= static_cast<float>(n_left);
  if (n_right > 0) mag_right /= static_cast<float>(n_right);

  if (n_left > 0) {
    // Continue forward from the left boundary frame.
    const int lb = frame_lo - 1;
    const float phi0 = std::arg(buf[base + lb]);
    float dphi = nominal_dphi;
    if (lb - 1 >= 0) {
      dphi = std::arg(buf[base + lb] * std::conj(buf[base + lb - 1]));
    }
    const float span = static_cast<float>((frame_hi + 1) - lb);
    for (int f = frame_lo; f <= frame_hi; ++f) {
      const float p = n_right > 0 ? static_cast<float>(f - lb) / span : 0.0f;
      const float mag = mag_left + (mag_right - mag_left) * p;
      const float phi = phi0 + static_cast<float>(f - lb) * dphi;
      buf[base + f] = std::polar(mag, phi);
    }
  } else {
    // Only right neighbours: continue backward from the right boundary frame.
    const int rb = frame_hi + 1;
    const float phi0 = std::arg(buf[base + rb]);
    float dphi = nominal_dphi;
    if (rb + 1 < n_frames) {
      dphi = std::arg(buf[base + rb + 1] * std::conj(buf[base + rb]));
    }
    for (int f = frame_lo; f <= frame_hi; ++f) {
      const float phi = phi0 - static_cast<float>(rb - f) * dphi;
      buf[base + f] = std::polar(mag_right, phi);
    }
  }
}

/// @brief Applies one op to the mutable complex STFT buffer (row-major bin*n_frames + frame).
void apply_op(std::vector<std::complex<float>>& buf, int n_frames, int n_fft, int hop_length,
              const RegionWindow& w, const SpectralRegionOp& op, int heal_radius_frames) {
  switch (op.mode) {
    case SpectralEditMode::Gain:
    case SpectralEditMode::Attenuate: {
      const float g = std::pow(10.0f, op.gain_db / 20.0f);
      for (int bin = w.bin_lo; bin <= w.bin_hi; ++bin) {
        const int base = bin * n_frames;
        for (int f = w.frame_lo; f <= w.frame_hi; ++f) {
          buf[base + f] *= g;
        }
      }
      break;
    }
    case SpectralEditMode::Mute: {
      for (int bin = w.bin_lo; bin <= w.bin_hi; ++bin) {
        const int base = bin * n_frames;
        for (int f = w.frame_lo; f <= w.frame_hi; ++f) {
          buf[base + f] = std::complex<float>(0.0f, 0.0f);
        }
      }
      break;
    }
    case SpectralEditMode::Heal: {
      // Per-bin tonal continuation from neighbouring unmasked frames. Reading
      // neighbours before writing the region is safe: neighbour frames are
      // outside the region, so the per-bin fill never disturbs them.
      for (int bin = w.bin_lo; bin <= w.bin_hi; ++bin) {
        const int base = bin * n_frames;
        const float nominal_dphi = static_cast<float>(constants::kTwoPi) * static_cast<float>(bin) *
                                   static_cast<float>(hop_length) / n_fft;
        heal_bin(buf, n_frames, base, w.frame_lo, w.frame_hi, heal_radius_frames, nominal_dphi);
      }
      break;
    }
  }
}

}  // namespace

Audio spectral_edit(const Audio& audio, const SpectralEditConfig& config,
                    const SpectralRegionOp* ops, std::size_t n_ops) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(is_power_of_two(config.n_fft), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0 && config.hop_length <= config.n_fft / 2,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.heal_radius_frames >= 1, ErrorCode::InvalidParameter);
  SONARE_CHECK(ops != nullptr || n_ops == 0, ErrorCode::InvalidParameter);

  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = config.window;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const int sample_rate = audio.sample_rate();
  const auto length = static_cast<int64_t>(audio.size());

  if (n_bins == 0 || n_frames == 0) {
    return Audio::from_buffer(audio.data(), audio.size(), sample_rate);
  }

  std::vector<std::complex<float>> buf(
      spec.complex_data(), spec.complex_data() + static_cast<size_t>(n_bins) * n_frames);

  for (std::size_t i = 0; i < n_ops; ++i) {
    const RegionWindow w = resolve_window(ops[i], n_bins, n_frames, config.n_fft, config.hop_length,
                                          sample_rate, length);
    if (!w.empty) {
      apply_op(buf, n_frames, config.n_fft, config.hop_length, w, ops[i],
               config.heal_radius_frames);
    }
  }

  Spectrogram edited =
      Spectrogram::from_complex(buf.data(), n_bins, n_frames, config.n_fft, config.hop_length,
                                sample_rate, /*center=*/true, spec.win_length());
  return edited.to_audio(static_cast<int>(audio.size()), config.window);
}

}  // namespace sonare
