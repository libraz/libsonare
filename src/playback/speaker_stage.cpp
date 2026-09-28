#include "playback/speaker_stage.h"

#include <algorithm>

namespace sonare::playback {

struct SpeakerStage::Impl {
  ChannelLayout layout = ChannelLayout::Stereo;
};

SpeakerStage::SpeakerStage() : impl_(std::make_unique<Impl>()) {}
SpeakerStage::~SpeakerStage() = default;

void SpeakerStage::prepare(double sample_rate, int max_block_size, ChannelLayout layout,
                           const std::array<SpeakerPrepare, kSpeakerRoleCount>& speakers,
                           const BassManagementConfig& bass) {
  (void)sample_rate;
  (void)max_block_size;
  (void)speakers;
  (void)bass;
  impl_->layout = layout;
}

void SpeakerStage::set_levels(const std::array<float, kSpeakerRoleCount>& trim_db,
                              float lfe_gain_db, float lfe_mix_db) noexcept {
  (void)trim_db;
  (void)lfe_gain_db;
  (void)lfe_mix_db;
}

void SpeakerStage::process(float* const* planes, int frames) noexcept {
  if (planes == nullptr || frames <= 0) return;
  for (int ch = 0; ch < channel_count(impl_->layout); ++ch) {
    if (planes[ch] != nullptr) std::fill(planes[ch], planes[ch] + frames, 0.0f);
  }
}

void SpeakerStage::reset() noexcept {}

int SpeakerStage::latency_samples() const noexcept { return 0; }

int SpeakerStage::latency_samples_q8() const noexcept { return 0; }

int SpeakerStage::plane_delay_q8(int plane) const noexcept {
  (void)plane;
  return 0;
}

}  // namespace sonare::playback
