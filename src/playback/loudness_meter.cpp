#include "playback/loudness_meter.h"

#include <algorithm>
#include <vector>

#include "mixing/meter.h"
#include "util/exception.h"

namespace sonare::playback {

struct PlaybackLoudnessMeter::Impl {
  int channels = 0;
  mixing::MeterConfig meter_config{};
  mixing::MeterProcessor meter{};
  // Deinterleave scratch, one internal block per plane.
  std::vector<std::vector<float>> planar;
  std::vector<float*> planar_ptrs;

  explicit Impl(mixing::MeterConfig config) : meter_config(config), meter(config) {}
};

PlaybackLoudnessMeter::PlaybackLoudnessMeter(int channels, int sample_rate) {
  if (channels != 1 && channels != 2 && channels != 6 && channels != 8) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "PlaybackLoudnessMeter: channel count must be 1, 2, 6, or 8");
  }
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "PlaybackLoudnessMeter: sample rate must be positive");
  }

  // Configure meter for LUFS only.
  mixing::MeterConfig config;
  config.measure_lufs = true;
  config.measure_true_peak = false;

  impl_ = std::make_unique<Impl>(config);
  impl_->channels = channels;

  // Prepare meter processor.
  impl_->meter.prepare(static_cast<double>(sample_rate), kPlaybackLoudnessBlockFrames);

  // Allocate planar scratch buffers.
  impl_->planar.assign(channels, std::vector<float>(kPlaybackLoudnessBlockFrames, 0.0f));
  impl_->planar_ptrs.resize(channels);
  for (int ch = 0; ch < channels; ++ch) {
    impl_->planar_ptrs[ch] = impl_->planar[ch].data();
  }
}

PlaybackLoudnessMeter::~PlaybackLoudnessMeter() = default;

void PlaybackLoudnessMeter::push_interleaved(const float* samples, size_t frames) {
  if (frames == 0) return;

  Impl& im = *impl_;
  const size_t ch = static_cast<size_t>(im.channels);
  // Every frame reaches the meter in this call; the meter keeps its own gating state.
  for (size_t offset = 0; offset < frames; offset += kPlaybackLoudnessBlockFrames) {
    const size_t n = std::min(frames - offset, static_cast<size_t>(kPlaybackLoudnessBlockFrames));
    const float* block = samples + offset * ch;
    for (size_t c = 0; c < ch; ++c) {
      float* plane = im.planar[c].data();
      for (size_t i = 0; i < n; ++i) plane[i] = block[i * ch + c];
    }
    im.meter.process(im.planar_ptrs.data(), im.channels, static_cast<int>(n));
  }
}

float PlaybackLoudnessMeter::integrated_lufs() const {
  return impl_->meter.snapshot().integrated_lufs;
}

int PlaybackLoudnessMeter::channels() const noexcept { return impl_->channels; }

}  // namespace sonare::playback
