#include "effects/formant_warp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;
using sonare::constants::kSpectrumEpsilon;
using sonare::constants::kTwoPi;

namespace {

constexpr int kFrameRateReference = 48000;
constexpr float kUnityTolerance = 1.0e-6f;

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

int formant_warp_frame_size(int sample_rate) noexcept {
  const double scaled = static_cast<double>(kFormantWarpFrameAt48k) *
                        static_cast<double>(sample_rate) / static_cast<double>(kFrameRateReference);
  return std::max(16, 4 * static_cast<int>(std::lround(scaled / 4.0)));
}

int formant_warp_lpc_order(int sample_rate) noexcept { return sample_rate / 1000 + 2; }

float effective_formant_factor(float factor, float amount) noexcept {
  const float clamped = std::clamp(factor, kFormantFactorMin, kFormantFactorMax);
  const float wet = std::clamp(amount, 0.0f, 1.0f);
  return std::clamp(1.0f + (clamped - 1.0f) * wet, kFormantFactorMin, kFormantFactorMax);
}

FormantWarpStream::FormantWarpStream() = default;
FormantWarpStream::~FormantWarpStream() = default;
FormantWarpStream::FormantWarpStream(FormantWarpStream&&) noexcept = default;
FormantWarpStream& FormantWarpStream::operator=(FormantWarpStream&&) noexcept = default;

void FormantWarpStream::prepare(int frame_size, int lpc_order) {
  SONARE_CHECK(frame_size >= 16 && frame_size % 4 == 0, ErrorCode::InvalidParameter);
  frame_size_ = frame_size;
  hop_size_ = frame_size / 4;
  order_ = std::max(2, std::min(lpc_order, frame_size - 1));
  bypass_step_ = 1.0f / static_cast<float>(hop_size_);

  // A power of two at least 2 * frame_size: zero-padding headroom for the autocorrelation, and a
  // size PFFFT factors at every rate.
  int n_fft = 2;
  while (n_fft < 2 * frame_size) n_fft *= 2;
  const size_t n_bins = static_cast<size_t>(n_fft / 2 + 1);
  fft_ = std::make_unique<FFT>(n_fft);
  fft_->prepare(true, true, false);
  hann_ = make_hann(frame_size);
  ring_.assign(static_cast<size_t>(frame_size), 0.0f);
  acc_.assign(static_cast<size_t>(2 * frame_size), 0.0f);
  norm_.assign(static_cast<size_t>(2 * frame_size), 0.0f);
  windowed_.assign(static_cast<size_t>(frame_size), 0.0f);
  ar_.assign(static_cast<size_t>(order_ + 1), 0.0f);
  lags_.assign(static_cast<size_t>(order_ + 1), 0.0);
  levinson_a_.assign(static_cast<size_t>(order_ + 1), 0.0);
  levinson_next_.assign(static_cast<size_t>(order_ + 1), 0.0);
  padded_.assign(static_cast<size_t>(n_fft), 0.0f);
  time_frame_.assign(static_cast<size_t>(n_fft), 0.0f);
  envelope_.assign(n_bins, 0.0f);
  warped_.assign(n_bins, 0.0f);
  spectrum_.assign(n_bins, {});
  predictor_spectrum_.assign(n_bins, {});
  bypass_target_ = std::abs(factor_ - 1.0f) < kUnityTolerance ? 1.0f : 0.0f;
  reset();
}

void FormantWarpStream::reset() noexcept {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  std::fill(acc_.begin(), acc_.end(), 0.0f);
  std::fill(norm_.begin(), norm_.end(), 0.0f);
  hop_count_ = 0;
  pushed_ = 0;
  ring_pos_ = 0;
  emit_slot_ = 0;
  first_frame_ = true;
  bypass_ = bypass_target_;
}

void FormantWarpStream::set_factor(float factor) noexcept {
  factor_ = std::isfinite(factor) ? std::clamp(factor, kFormantFactorMin, kFormantFactorMax) : 1.0f;
  bypass_target_ = std::abs(factor_ - 1.0f) < kUnityTolerance ? 1.0f : 0.0f;
}

// Accumulator slot of the oldest sample of the frame just gathered; negative positions wrap.
size_t FormantWarpStream::frame_start_slot() const noexcept {
  const auto slots = static_cast<std::int64_t>(acc_.size());
  return static_cast<size_t>((((pushed_ - frame_size_) % slots) + slots) % slots);
}

// Adds windowed * window, the power the warped path carries, so an unwarped frame does not step the
// level.
void FormantWarpStream::accumulate_unwarped() noexcept {
  const size_t slots = acc_.size();
  size_t slot = frame_start_slot();
  for (int i = 0; i < frame_size_; ++i) {
    const std::int64_t idx = pushed_ - frame_size_ + i;
    if (idx >= 0) {
      const float win = hann_[static_cast<size_t>(i)];
      acc_[slot] += windowed_[static_cast<size_t>(i)] * win;
      norm_[slot] += win * win;
    }
    if (++slot == slots) slot = 0;
  }
}

void FormantWarpStream::process_frame() noexcept {
  // The first frame holds only a window tail of the signal; the grid starts a half frame early.
  if (first_frame_) {
    first_frame_ = false;
    return;
  }

  // The ring's write position holds the oldest of the last frame_size samples.
  size_t read = ring_pos_;
  for (int i = 0; i < frame_size_; ++i) {
    windowed_[static_cast<size_t>(i)] = ring_[read] * hann_[static_cast<size_t>(i)];
    if (++read == ring_.size()) read = 0;
  }

  if (std::abs(factor_ - 1.0f) < kUnityTolerance) {
    accumulate_unwarped();
    return;
  }

  // Autocorrelation lags through the power spectrum, then Levinson-Durbin.
  const size_t order = static_cast<size_t>(order_);
  const double inv_n = 1.0 / static_cast<double>(frame_size_);
  std::fill(padded_.begin(), padded_.end(), 0.0f);
  std::copy(windowed_.begin(), windowed_.end(), padded_.begin());
  fft_->forward(padded_.data(), spectrum_.data());
  for (auto& bin : spectrum_) bin = std::complex<float>(std::norm(bin), 0.0f);
  fft_->inverse(spectrum_.data(), time_frame_.data());
  for (size_t lag = 0; lag <= order; ++lag) {
    lags_[lag] = static_cast<double>(time_frame_[lag]) * inv_n;
  }
  std::fill(levinson_a_.begin(), levinson_a_.end(), 0.0);
  levinson_a_[0] = 1.0;
  double error = lags_[0];
  if (error <= 1.0e-20) {
    error = 0.0;
  } else {
    for (size_t i = 1; i <= order; ++i) {
      double acc = lags_[i];
      for (size_t j = 1; j < i; ++j) acc += levinson_a_[j] * lags_[i - j];
      const double reflection = -acc / error;
      levinson_next_ = levinson_a_;
      for (size_t j = 1; j < i; ++j) {
        levinson_next_[j] = levinson_a_[j] + reflection * levinson_a_[i - j];
      }
      levinson_next_[i] = reflection;
      levinson_a_.swap(levinson_next_);
      error *= 1.0 - reflection * reflection;
      if (error <= 1.0e-20) {
        error = 0.0;
        break;
      }
    }
  }

  // A silent or non-finite frame passes unwarped; !(x >= y) also catches a NaN.
  const float variance = static_cast<float>(error);
  if (!(variance >= kEpsilon)) {
    accumulate_unwarped();
    return;
  }
  for (size_t k = 0; k <= order; ++k) ar_[k] = static_cast<float>(levinson_a_[k]);

  // LPC residual (inverse filtering), zero-padded to n_fft.
  std::fill(padded_.begin(), padded_.end(), 0.0f);
  for (size_t i = 0; i < windowed_.size(); ++i) {
    double e = windowed_[i];
    const size_t max_k = std::min(order, i);
    for (size_t k = 1; k <= max_k; ++k) {
      e += static_cast<double>(ar_[k]) * windowed_[i - k];
    }
    padded_[i] = static_cast<float>(e);
  }
  fft_->forward(padded_.data(), spectrum_.data());

  // Unit-gain envelope 1 / |A| from the zero-padded predictor; the level is already in the
  // residual.
  std::fill(padded_.begin(), padded_.end(), 0.0f);
  std::copy(ar_.begin(), ar_.end(), padded_.begin());
  fft_->forward(padded_.data(), predictor_spectrum_.data());
  const size_t n_bins = envelope_.size();
  for (size_t k = 0; k < n_bins; ++k) {
    envelope_[k] = 1.0f / (std::abs(predictor_spectrum_[k]) + kSpectrumEpsilon);
  }

  // Warp the envelope by resampling along frequency (src = k / factor); DC stays fixed.
  const float src_max = static_cast<float>(n_bins - 1);
  warped_[0] = envelope_[0];
  for (size_t k = 1; k < n_bins; ++k) {
    const float src = std::clamp(static_cast<float>(k) / factor_, 0.0f, src_max);
    const size_t lo = static_cast<size_t>(std::floor(src));
    const size_t hi = std::min(lo + 1, n_bins - 1);
    const float frac = src - static_cast<float>(lo);
    warped_[k] = envelope_[lo] * (1.0f - frac) + envelope_[hi] * frac;
  }

  // Re-colour the whitened residual with the warped envelope.
  for (size_t k = 0; k < n_bins; ++k) spectrum_[k] *= warped_[k];
  fft_->inverse(spectrum_.data(), time_frame_.data());

  // Synthesis window + overlap-add.
  const size_t slots = acc_.size();
  size_t slot = frame_start_slot();
  for (int i = 0; i < frame_size_; ++i) {
    const std::int64_t idx = pushed_ - frame_size_ + i;
    if (idx >= 0) {
      const float win = hann_[static_cast<size_t>(i)];
      acc_[slot] += time_frame_[static_cast<size_t>(i)] * win;
      norm_[slot] += win * win;
    }
    if (++slot == slots) slot = 0;
  }
}

void FormantWarpStream::process(const float* input, float* output, int num_samples) noexcept {
  if (frame_size_ == 0 || input == nullptr || output == nullptr) return;
  const size_t slots = acc_.size();
  for (int n = 0; n < num_samples; ++n) {
    // A non-finite sample would sit in the ring for a frame and poison every analysis it joins.
    const float x = std::isfinite(input[n]) ? input[n] : 0.0f;
    const float dry = ring_[ring_pos_];
    ring_[ring_pos_] = x;
    if (++ring_pos_ == ring_.size()) ring_pos_ = 0;
    ++pushed_;
    if (++hop_count_ == hop_size_) {
      hop_count_ = 0;
      process_frame();
    }

    // Every frame covering sample pushed - 1 - frame_size has been added by now.
    float wet = 0.0f;
    if (pushed_ > frame_size_) {
      const float total = norm_[emit_slot_];
      wet = total > kSpectrumEpsilon ? acc_[emit_slot_] / total : acc_[emit_slot_];
      acc_[emit_slot_] = 0.0f;
      norm_[emit_slot_] = 0.0f;
      if (++emit_slot_ == slots) emit_slot_ = 0;
    }

    if (bypass_ != bypass_target_) {
      bypass_ += bypass_target_ > bypass_ ? bypass_step_ : -bypass_step_;
      bypass_ = std::clamp(bypass_, 0.0f, 1.0f);
      if (std::abs(bypass_ - bypass_target_) < bypass_step_) bypass_ = bypass_target_;
    }
    if (bypass_ >= 1.0f) {
      output[n] = dry;
    } else if (bypass_ <= 0.0f) {
      output[n] = wet;
    } else {
      output[n] = dry * bypass_ + wet * (1.0f - bypass_);
    }
  }
}

FormantWarp::FormantWarp(FormantWarpConfig config) : config_(config) {}

Audio FormantWarp::process(const Audio& audio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config_.factor), ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.lpc_order >= 0, ErrorCode::InvalidParameter);

  const int sr = audio.sample_rate();
  const int frame_size =
      config_.frame_in_time ? formant_warp_frame_size(sr) : kFormantWarpFrameAt48k;
  const size_t n = audio.size();

  // Effective warp factor folds the dry/wet amount into the shift strength.
  const float effective_factor = effective_formant_factor(config_.factor, config_.amount);
  // LPC analysis/re-synthesis is not identity even when its envelope is not
  // shifted. Bypass it at unity so pitch-only voice changes preserve samples.
  if (std::abs(effective_factor - 1.0f) < kUnityTolerance) return audio;

  // Explicit order, else the rate-scaled one.
  const int order = config_.lpc_order > 0 ? config_.lpc_order : formant_warp_lpc_order(sr);

  FormantWarpStream stream;
  stream.prepare(frame_size, order);
  stream.set_factor(effective_factor);

  // The stream lags by one frame: flush it with silence and keep the output from the latency on.
  const size_t latency = static_cast<size_t>(stream.latency_samples());
  std::vector<float> out(n + latency, 0.0f);
  stream.process(audio.data(), out.data(), static_cast<int>(n));
  const std::vector<float> silence(latency, 0.0f);
  stream.process(silence.data(), out.data() + n, static_cast<int>(latency));
  out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(latency));
  return Audio::from_vector(std::move(out), sr);
}

}  // namespace sonare
