#include "playback/loudness_meter.h"

namespace sonare::playback {

struct PlaybackLoudnessMeter::Impl {
  int channels = 0;
};

PlaybackLoudnessMeter::PlaybackLoudnessMeter(int channels, int sample_rate)
    : impl_(std::make_unique<Impl>()) {
  (void)sample_rate;
  impl_->channels = channels;
}

PlaybackLoudnessMeter::~PlaybackLoudnessMeter() = default;

void PlaybackLoudnessMeter::push_interleaved(const float* samples, size_t frames) {
  (void)samples;
  (void)frames;
}

float PlaybackLoudnessMeter::integrated_lufs() const { return 0.0f; }

int PlaybackLoudnessMeter::channels() const noexcept { return impl_->channels; }

}  // namespace sonare::playback
