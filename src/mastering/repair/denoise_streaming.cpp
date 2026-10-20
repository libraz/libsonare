#include "mastering/repair/denoise_streaming.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

namespace {

/// @brief Validates @p config against every offline rule plus the streaming one.
/// @details Quantile is refused rather than swapped for a recursive estimator:
///   a substituted value becomes an in-domain value nothing downstream can
///   separate from a deliberate one.
DenoiseClassicalConfig streaming_config(const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (validated->noise_estimator == DenoiseNoiseEstimator::Quantile) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "denoise noise_estimator must not be Quantile when denoising a stream: it ranks every "
        "frame of the whole signal by energy, which a stream never reaches the end of. Set "
        "noise_estimator to Spp, which tracks recursively without a minimum window. Mcra and "
        "Imcra also track recursively and are accepted, but their minimum tracking over-reports "
        "the floor wherever the programme is intermittent.");
  }
  return validated.get();
}

}  // namespace

StreamingDenoise::StreamingDenoise(const DenoiseClassicalConfig& config)
    : config_(streaming_config(config)),
      n_fft_(config_.n_fft),
      hop_length_(config_.hop_length),
      n_bins_(config_.n_fft / 2 + 1),
      ola_(config_.n_fft, config_.hop_length) {
  // Built here rather than in prepare() so latency_samples() answers from the
  // configuration alone, which is what a host asks before it prepares anything.
  stage_ = std::make_unique<detail::GainStage>(n_bins_, config_);
  mask_latency_frames_ = stage_->latency();
}

void StreamingDenoise::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDenoise::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "StreamingDenoise");

  sample_rate_ = sample_rate;

  const auto bins = static_cast<std::size_t>(n_bins_);
  const auto channels = static_cast<std::size_t>(max_channels);
  held_spectra_.assign(bins * channels, std::complex<float>{});
  power_f_.assign(bins, 0.0f);
  power_d_.assign(bins, 0.0);
  channel_power_f_.assign(bins, 0.0f);
  channel_power_d_.assign(bins, 0.0);
  tracker_input_.assign(bins, 0.0f);
  noise_frame_.assign(bins, 0.0);
  ola_.prepare(max_block_size, max_channels, latency_samples());

  tracker_ = std::make_unique<common::NoiseTracker>(
      n_bins_, static_cast<int>(std::lround(sample_rate_)),
      detail::tracker_mode_for(config_.noise_estimator), hop_length_);

  prepared_ = true;
  reset();
}

void StreamingDenoise::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StreamingDenoise");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  // Once per block, and skipped entirely by a block that took no frame, so the sweep runs
  // min(blocks, frames) times rather than once per either. The cells outnumber a hop's samples
  // by an order of magnitude, so per-frame would cost more than the transforms it guards at a
  // long block, and per-block would cost more than that again at a short one. A block that took
  // no frame changed no state: the input ring poisons nothing until a frame is taken from it.
  ola_.process(
      channels, num_channels, num_samples, "StreamingDenoise", [this] { analyze_frame(); },
      [this](bool analyzed) {
        if (analyzed && discard_non_finite_state()) note_non_finite_discard();
      });
}

bool StreamingDenoise::discard_non_finite_state() noexcept {
  bool discarded = tracker_->discard_non_finite_state();
  discarded |= stage_->discard_non_finite_state();
  // Not redundant beside the tracker's reset: the reset only clears the seed, and
  // NoiseTracker reseeds from the next frame, which the stream's input ring still supplies.
  discarded |= ola_.discard_non_finite_state();
  if (discarded) {
    auto& spectra = ola_.spectra_storage();
    std::fill(spectra.begin(), spectra.end(), std::complex<float>{});
    std::fill(held_spectra_.begin(), held_spectra_.end(), std::complex<float>{});
  }
  return discarded;
}

void StreamingDenoise::analyze_frame() {
  const auto bins = static_cast<std::size_t>(n_bins_);
  const int channels = ola_.active_channels();
  std::complex<float>* spectra = ola_.spectra();

  std::fill(power_f_.begin(), power_f_.end(), 0.0f);
  std::fill(power_d_.begin(), power_d_.end(), 0.0);
  for (int ch = 0; ch < channels; ++ch) {
    detail::frame_powers(spectra + bins * static_cast<std::size_t>(ch), n_bins_,
                         channel_power_f_.data(), channel_power_d_.data());
    for (int b = 0; b < n_bins_; ++b) {
      power_f_[static_cast<std::size_t>(b)] += channel_power_f_[static_cast<std::size_t>(b)];
      power_d_[static_cast<std::size_t>(b)] += channel_power_d_[static_cast<std::size_t>(b)];
    }
  }

  for (int b = 0; b < n_bins_; ++b) {
    tracker_input_[static_cast<std::size_t>(b)] =
        std::max(power_f_[static_cast<std::size_t>(b)], 0.0f);
  }
  tracker_->update(tracker_input_.data());
  const float* tracked = tracker_->noise_psd();
  for (int b = 0; b < n_bins_; ++b) noise_frame_[static_cast<std::size_t>(b)] = tracked[b];

  const double* gains = stage_->push(power_d_.data(), noise_frame_.data());
  const bool lagged = mask_latency_frames_ != 0;
  if (gains != nullptr) ola_.emit_frame(gains, lagged ? held_spectra_.data() : spectra);
  if (lagged) {
    const auto span = static_cast<std::ptrdiff_t>(bins * static_cast<std::size_t>(channels));
    std::copy(spectra, spectra + span, held_spectra_.begin());
  }
}

void StreamingDenoise::reset() {
  ola_.reset();
  std::fill(held_spectra_.begin(), held_spectra_.end(), std::complex<float>{});
  if (tracker_ != nullptr) tracker_->reset();
  stage_->reset();
}

int StreamingDenoise::latency_samples() const noexcept {
  return n_fft_ - 1 + mask_latency_frames_ * hop_length_;
}

int StreamingDenoise::tail_samples() const noexcept {
  rt::TailBudget tail;
  return tail.delay(n_fft_ - 1).samples();
}

}  // namespace sonare::mastering::repair
