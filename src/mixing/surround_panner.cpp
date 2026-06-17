#include "mixing/surround_panner.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mixing {

namespace {

/// One speaker on the horizontal panning ring.
struct RingNode {
  int plane = 0;
  float azimuth = 0.0f;
};

}  // namespace

SurroundPanGains compute_surround_pan_gains(const SurroundPanParams& params, ChannelLayout layout) {
  const int count = channel_count(layout);
  SONARE_CHECK_MSG(count > 2, ErrorCode::InvalidParameter,
                   "surround panner requires a >2-channel layout");

  const SpeakerRole* roles = speaker_roles(layout);
  const int lfe = lfe_index(layout);

  // Build the horizontal ring of non-LFE speakers, sorted by azimuth.
  RingNode ring[kMaxSurroundPlanes];
  int ring_size = 0;
  for (int p = 0; p < count; ++p) {
    if (p == lfe) continue;
    ring[ring_size++] = {p, speaker_azimuth_deg(roles[p])};
  }
  std::sort(ring, ring + ring_size,
            [](const RingNode& a, const RingNode& b) { return a.azimuth < b.azimuth; });

  // Point-source gains: constant-power crossfade between the two adjacent
  // speakers bracketing the target azimuth (rear arc wraps last -> first).
  std::array<float, kMaxSurroundPlanes> point{};
  const float a = std::clamp(params.azimuth, -180.0f, 180.0f);
  int lo_idx = ring_size - 1;
  int hi_idx = 0;
  float t = 0.0f;
  if (a <= ring[0].azimuth || a >= ring[ring_size - 1].azimuth) {
    // Rear arc: from the right-most speaker around the back to the left-most.
    const float lo_az = ring[ring_size - 1].azimuth;
    const float hi_az = ring[0].azimuth + 360.0f;
    const float pos = a >= lo_az ? a : a + 360.0f;
    t = (pos - lo_az) / (hi_az - lo_az);
  } else {
    for (int i = 0; i + 1 < ring_size; ++i) {
      if (a >= ring[i].azimuth && a <= ring[i + 1].azimuth) {
        lo_idx = i;
        hi_idx = i + 1;
        t = (a - ring[i].azimuth) / (ring[i + 1].azimuth - ring[i].azimuth);
        break;
      }
    }
  }
  const float angle = std::clamp(t, 0.0f, 1.0f) * constants::kHalfPi;
  point[ring[lo_idx].plane] = std::cos(angle);
  point[ring[hi_idx].plane] = std::sin(angle);

  // Equal-front-spread direction (unit power across the front L/C/R speakers).
  std::array<float, kMaxSurroundPlanes> spread{};
  int front_count = 0;
  for (int p = 0; p < count; ++p) {
    if (roles[p] == SpeakerRole::L || roles[p] == SpeakerRole::C || roles[p] == SpeakerRole::R) {
      spread[p] = 1.0f;
      ++front_count;
    }
  }
  if (front_count > 0) {
    const float inv = 1.0f / std::sqrt(static_cast<float>(front_count));
    for (int p = 0; p < count; ++p) spread[p] *= inv;
  }

  // Blend point -> spread by divergence, then renormalize to unit power so that
  // divergence does not modulate perceived loudness.
  const float d = std::clamp(params.divergence, 0.0f, 1.0f);
  SurroundPanGains result;
  result.count = count;
  float power = 0.0f;
  for (int p = 0; p < count; ++p) {
    if (p == lfe) continue;
    const float g = (1.0f - d) * point[p] + d * spread[p];
    result.gain[p] = g;
    power += g * g;
  }
  if (power > 0.0f) {
    const float inv = 1.0f / std::sqrt(power);
    for (int p = 0; p < count; ++p) {
      if (p != lfe) result.gain[p] *= inv;
    }
  }

  // LFE is a raw scalar send, deliberately outside the unit-power normalization.
  if (lfe >= 0) result.gain[lfe] = std::clamp(params.lfe, 0.0f, 1.0f);

  return result;
}

SurroundPannerProcessor::SurroundPannerProcessor(ChannelLayout layout, SurroundPanParams params,
                                                 float smoothing_ms)
    : smoothing_ms_(smoothing_ms),
      layout_(static_cast<uint8_t>(layout)),
      azimuth_(params.azimuth),
      elevation_(params.elevation),
      divergence_(params.divergence),
      lfe_(params.lfe),
      distance_(params.distance) {}

void SurroundPannerProcessor::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  for (auto& s : smoothers_) s.prepare(sample_rate_, smoothing_ms_);
  reset();
}

void SurroundPannerProcessor::load_target_gains(SurroundPanGains& out) const {
  SurroundPanParams p;
  p.azimuth = azimuth_.load(std::memory_order_relaxed);
  p.elevation = elevation_.load(std::memory_order_relaxed);
  p.divergence = divergence_.load(std::memory_order_relaxed);
  p.lfe = lfe_.load(std::memory_order_relaxed);
  p.distance = distance_.load(std::memory_order_relaxed);
  out = compute_surround_pan_gains(p, layout());
}

void SurroundPannerProcessor::reset() {
  SurroundPanGains gains;
  load_target_gains(gains);
  for (int p = 0; p < kMaxSurroundPlanes; ++p) {
    smoothers_[p].reset(p < gains.count ? gains.gain[p] : 0.0f);
  }
}

void SurroundPannerProcessor::set_params(const SurroundPanParams& params) noexcept {
  azimuth_.store(std::clamp(params.azimuth, -180.0f, 180.0f), std::memory_order_relaxed);
  elevation_.store(params.elevation, std::memory_order_relaxed);
  divergence_.store(std::clamp(params.divergence, 0.0f, 1.0f), std::memory_order_relaxed);
  lfe_.store(std::clamp(params.lfe, 0.0f, 1.0f), std::memory_order_relaxed);
  distance_.store(params.distance, std::memory_order_relaxed);
}

SurroundPanParams SurroundPannerProcessor::params() const noexcept {
  SurroundPanParams p;
  p.azimuth = azimuth_.load(std::memory_order_relaxed);
  p.elevation = elevation_.load(std::memory_order_relaxed);
  p.divergence = divergence_.load(std::memory_order_relaxed);
  p.lfe = lfe_.load(std::memory_order_relaxed);
  p.distance = distance_.load(std::memory_order_relaxed);
  return p;
}

void SurroundPannerProcessor::set_layout(ChannelLayout layout) noexcept {
  layout_.store(static_cast<uint8_t>(layout), std::memory_order_relaxed);
}

void SurroundPannerProcessor::process_add(const float* const* in, int num_in_channels,
                                          float* const* out, int num_out_planes, int num_samples) {
  if (in == nullptr || out == nullptr || num_in_channels <= 0 || num_out_planes <= 0 ||
      num_samples <= 0) {
    return;
  }

  SurroundPanGains gains;
  load_target_gains(gains);
  const int planes = std::min(gains.count, num_out_planes);
  for (int p = 0; p < planes; ++p) smoothers_[p].set_target(gains.gain[p]);

  // Collapse the source to a point: mono passes through, stereo is summed at
  // -6 dB so a correlated centre image stays at unity.
  const bool stereo_in = num_in_channels >= 2 && in[0] != nullptr && in[1] != nullptr;
  const float* in0 = in[0];
  const float* in1 = stereo_in ? in[1] : nullptr;
  if (in0 == nullptr) return;

  for (int i = 0; i < num_samples; ++i) {
    const float src = stereo_in ? 0.5f * (in0[i] + in1[i]) : in0[i];
    for (int p = 0; p < planes; ++p) {
      const float g = smoothers_[p].process();
      if (out[p] != nullptr) out[p][i] += g * src;
    }
  }
}

}  // namespace sonare::mixing
