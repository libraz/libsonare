#include "mastering/repair/dereverb_streaming.h"

#include <algorithm>
#include <cmath>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/repair/dereverb_internal.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

namespace {

DereverbClassicalConfig streaming_config(const DereverbClassicalConfig& config) {
  const auto validated = Validated<DereverbClassicalConfig>::make(config);
  if (validated->wpe_enabled) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb wpe_enabled must be false when dereverberating a stream: the "
                          "WPE predictor is fitted over the whole signal");
  }
  return validated.get();
}

}  // namespace

StreamingDereverb::StreamingDereverb(const DereverbClassicalConfig& config)
    : config_(streaming_config(config)),
      n_fft_(config_.n_fft),
      n_bins_(config_.n_fft / 2 + 1),
      ola_(config_.n_fft, config_.hop_length) {}

void StreamingDereverb::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDereverb::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "StreamingDereverb");
  const int rate = static_cast<int>(std::lround(sample_rate));
  delay_frames_ = detail::dereverb_late_delay_frames(config_, rate);
  decay_ = detail::dereverb_late_decay(config_, delay_frames_, rate);

  const auto bins = static_cast<std::size_t>(n_bins_);
  power_.assign(bins, 0.0f);
  gains_.assign(bins, 1.0);
  power_history_.assign(bins * static_cast<std::size_t>(delay_frames_), 0.0f);
  ola_.prepare(max_block_size, max_channels, latency_samples());

  prepared_ = true;
  reset();
}

void StreamingDereverb::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "StreamingDereverb");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ola_.process(
      channels, num_channels, num_samples, "StreamingDereverb", [this] { analyze_frame(); },
      [](bool) {});
}

void StreamingDereverb::analyze_frame() {
  const auto bins = static_cast<std::size_t>(n_bins_);
  const std::complex<float>* spectra = ola_.spectra();

  std::fill(power_.begin(), power_.end(), 0.0f);
  for (int ch = 0; ch < ola_.active_channels(); ++ch) {
    const std::complex<float>* spectrum = spectra + bins * static_cast<std::size_t>(ch);
    for (int b = 0; b < n_bins_; ++b) power_[static_cast<std::size_t>(b)] += std::norm(spectrum[b]);
  }

  // The frame delay_frames_ back, which the offline mask reads as power[t - delay].
  const bool has_late = frames_taken_ >= static_cast<std::size_t>(delay_frames_);
  float* late = power_history_.data() + bins * static_cast<std::size_t>(power_history_read_);
  for (int b = 0; b < n_bins_; ++b) {
    const auto bin = static_cast<std::size_t>(b);
    const double late_psd = has_late ? static_cast<double>(late[bin]) * decay_ : 0.0;
    bool suppressed = false;
    gains_[bin] = detail::dereverb_subtraction_gain(static_cast<double>(power_[bin]), late_psd,
                                                    config_, &suppressed);
  }
  std::copy(power_.begin(), power_.end(), late);
  power_history_read_ = power_history_read_ + 1 == delay_frames_ ? 0 : power_history_read_ + 1;
  ++frames_taken_;

  ola_.emit_frame(gains_.data(), spectra);
}

void StreamingDereverb::reset() {
  ola_.reset();
  std::fill(power_history_.begin(), power_history_.end(), 0.0f);
  power_history_read_ = 0;
  frames_taken_ = 0;
}

int StreamingDereverb::latency_samples() const noexcept { return n_fft_ - 1; }

int StreamingDereverb::tail_samples() const noexcept {
  rt::TailBudget tail;
  return tail.delay(n_fft_ - 1).samples();
}

}  // namespace sonare::mastering::repair
