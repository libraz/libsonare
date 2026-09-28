#include "playback/binaural.h"

#include <algorithm>

namespace sonare::playback {

struct BinauralRenderer::Impl {};

SpeakerDirection head_relative_direction(SpeakerDirection world, const HeadPose& pose) noexcept {
  (void)world;
  (void)pose;
  return {};
}

BinauralRenderer::BinauralRenderer() : impl_(std::make_unique<Impl>()) {}
BinauralRenderer::~BinauralRenderer() = default;

void BinauralRenderer::prepare(double sample_rate, int max_block_size, const HrtfSet& hrtf,
                               const HeadphoneSlotSet& slots, RoomPreset room) {
  (void)sample_rate;
  (void)max_block_size;
  (void)hrtf;
  (void)slots;
  (void)room;
}

void BinauralRenderer::set_params(const BinauralParams& params) noexcept { (void)params; }

void BinauralRenderer::set_head_pose(const HeadPose& pose) noexcept { (void)pose; }

void BinauralRenderer::process(const float* const* bus, float* left, float* right,
                               int frames) noexcept {
  (void)bus;
  if (frames <= 0) return;
  if (left != nullptr) std::fill(left, left + frames, 0.0f);
  if (right != nullptr) std::fill(right, right + frames, 0.0f);
}

void BinauralRenderer::reset() noexcept {}

int BinauralRenderer::latency_samples() const noexcept { return 0; }

}  // namespace sonare::playback
