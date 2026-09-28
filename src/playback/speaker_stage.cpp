#include "playback/speaker_stage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::playback {

using sonare::constants::kInvSqrt2;
using sonare::constants::kSoundSpeedMps;

namespace {

/// Fixed cutoff of the LFE fold-down rule reused for the `subwoofer=false`
/// destination: the same rule a bus without its own LFE plane uses (LFE ×
/// lfe_mix_db, 120 Hz LR4 LPF, -3 dB into each of two planes).
constexpr float kFoldCutoffHz = 120.0f;

mastering::multiband::CrossoverConfig single_cutoff_config(float cutoff_hz) {
  mastering::multiband::CrossoverConfig config;
  config.cutoffs_hz = {cutoff_hz};
  config.slope = mastering::multiband::CrossoverSlope::LR4;
  config.mode = mastering::multiband::CrossoverMode::LinkwitzRiley;
  return config;
}

/// Buffer size a Lagrange-3 fractional tap needs for an integer delay part, or
/// 0 for a zero delay (pass-through, no storage).
std::size_t fractional_buffer_size(int delay_q8) {
  if (delay_q8 <= 0) return 0;
  const auto integer_delay = static_cast<std::size_t>(delay_q8 >> 8);
  return std::max<std::size_t>(8, integer_delay + 8);
}

}  // namespace

struct SpeakerStage::Impl {
  ChannelLayout layout = ChannelLayout::Stereo;
  int plane_count = 2;
  int lfe_plane = -1;
  std::array<SpeakerRole, kSpeakerRoleCount> roles{};

  // Per-plane distance delay (Q8.8 samples), the Lagrange-3 tap and its buffer.
  struct PlaneDelay {
    int delay_q8 = 0;
    std::vector<float> buffer;
    std::size_t write_index = 0;
  };
  std::array<PlaneDelay, kSpeakerRoleCount> delays{};
  int max_delay_q8 = 0;

  // Bass management.
  BassManagementConfig bass{};
  std::vector<int> small_planes;  ///< bus plane indices with SpeakerSize::Small
  int plane_l = -1;
  int plane_r = -1;
  mastering::multiband::Crossover small_crossover{single_cutoff_config(80.0f)};
  mastering::multiband::CrossoverScratch small_scratch;
  std::vector<float*> small_ptrs;
  mastering::multiband::Crossover fold_filter{single_cutoff_config(kFoldCutoffHz)};
  mastering::multiband::CrossoverScratch fold_scratch;
  std::vector<float> low_sum;
  std::vector<float> fold_input;

  // Realtime levels.
  std::array<float, kSpeakerRoleCount> trim_linear{};
  float lfe_gain_linear = 1.0f;
  float lfe_mix_linear = 1.0f;
};

SpeakerStage::SpeakerStage() : impl_(std::make_unique<Impl>()) { impl_->trim_linear.fill(1.0f); }
SpeakerStage::~SpeakerStage() = default;

void SpeakerStage::prepare(double sample_rate, int max_block_size, ChannelLayout layout,
                           const std::array<SpeakerPrepare, kSpeakerRoleCount>& speakers,
                           const BassManagementConfig& bass) {
  Impl& m = *impl_;
  m.layout = layout;
  m.plane_count = channel_count(layout);
  m.lfe_plane = lfe_index(layout);
  m.bass = bass;

  const SpeakerRole* roles = speaker_roles(layout);
  for (int p = 0; p < m.plane_count; ++p) m.roles[static_cast<std::size_t>(p)] = roles[p];

  // Distance delay = (max distance among the speakers with one - this speaker's
  // distance) / speed of sound. A speaker without `distance_m` gets no delay,
  // matching "null = no compensation".
  double max_distance = 0.0;
  bool any_distance = false;
  for (int p = 0; p < m.plane_count; ++p) {
    if (p == m.lfe_plane) continue;
    const SpeakerPrepare& sp = speakers[static_cast<std::size_t>(roles[p])];
    if (sp.has_distance) {
      any_distance = true;
      max_distance = std::max(max_distance, static_cast<double>(sp.distance_m));
    }
  }

  int max_delay_q8 = 0;
  for (int p = 0; p < m.plane_count; ++p) {
    Impl::PlaneDelay& d = m.delays[static_cast<std::size_t>(p)];
    d.delay_q8 = 0;
    if (p != m.lfe_plane && any_distance) {
      const SpeakerPrepare& sp = speakers[static_cast<std::size_t>(roles[p])];
      if (sp.has_distance) {
        const double delay_seconds = (max_distance - static_cast<double>(sp.distance_m)) /
                                     static_cast<double>(kSoundSpeedMps);
        d.delay_q8 = static_cast<int>(std::lround(delay_seconds * sample_rate * 256.0));
      }
    }
    max_delay_q8 = std::max(max_delay_q8, d.delay_q8);
    d.buffer.assign(fractional_buffer_size(d.delay_q8), 0.0f);
    d.write_index = 0;
  }
  m.max_delay_q8 = max_delay_q8;

  // Bass management: which planes are small, and where the large L/R pair is
  // for the `subwoofer=false` fold destination.
  m.small_planes.clear();
  m.plane_l = -1;
  m.plane_r = -1;
  for (int p = 0; p < m.plane_count; ++p) {
    if (roles[p] == SpeakerRole::L) m.plane_l = p;
    if (roles[p] == SpeakerRole::R) m.plane_r = p;
    if (p == m.lfe_plane) continue;
    if (speakers[static_cast<std::size_t>(roles[p])].size == SpeakerSize::Small) {
      m.small_planes.push_back(p);
    }
  }

  m.low_sum.assign(static_cast<std::size_t>(max_block_size), 0.0f);
  m.fold_input.assign(static_cast<std::size_t>(max_block_size), 0.0f);

  if (bass.enabled && !m.small_planes.empty()) {
    m.small_crossover.set_config(single_cutoff_config(bass.crossover_hz));
    const auto small_count = static_cast<int>(m.small_planes.size());
    m.small_crossover.prepare(sample_rate, max_block_size, small_count);
    m.small_crossover.prepare_scratch(m.small_scratch, small_count, max_block_size);
    m.small_ptrs.assign(static_cast<std::size_t>(small_count), nullptr);
  }
  if (bass.enabled && !bass.subwoofer && m.lfe_plane >= 0) {
    m.fold_filter.prepare(sample_rate, max_block_size, 1);
    m.fold_filter.prepare_scratch(m.fold_scratch, 1, max_block_size);
  }
}

void SpeakerStage::set_levels(const std::array<float, kSpeakerRoleCount>& trim_db,
                              float lfe_gain_db, float lfe_mix_db) noexcept {
  Impl& m = *impl_;
  for (std::size_t r = 0; r < static_cast<std::size_t>(kSpeakerRoleCount); ++r) {
    m.trim_linear[r] = db_to_linear(trim_db[r]);
  }
  m.lfe_gain_linear = db_to_linear(lfe_gain_db);
  m.lfe_mix_linear = db_to_linear(lfe_mix_db);
}

void SpeakerStage::process(float* const* planes, int frames) noexcept {
  Impl& m = *impl_;

  if (m.bass.enabled) {
    std::fill(m.low_sum.begin(), m.low_sum.begin() + frames, 0.0f);
    if (!m.small_planes.empty()) {
      for (std::size_t i = 0; i < m.small_planes.size(); ++i) {
        m.small_ptrs[i] = planes[m.small_planes[i]];
      }
      m.small_crossover.split_into(m.small_ptrs.data(), static_cast<int>(m.small_planes.size()),
                                   frames, m.small_scratch);
      for (std::size_t i = 0; i < m.small_planes.size(); ++i) {
        const float* low = m.small_scratch.band_channels[0][i];
        const float* high = m.small_scratch.band_channels[1][i];
        float* plane = planes[m.small_planes[i]];
        for (int f = 0; f < frames; ++f) {
          m.low_sum[static_cast<std::size_t>(f)] += low[f];
          plane[f] = high[f];
        }
      }
    }

    if (m.lfe_plane >= 0) {
      float* lfe = planes[m.lfe_plane];
      if (m.bass.subwoofer) {
        // Sub-feed rule: LFE * lfe_gain_db + the small speakers' low-band sum,
        // straight into the LFE plane.
        for (int f = 0; f < frames; ++f) {
          lfe[f] = m.lfe_gain_linear * lfe[f] + m.low_sum[static_cast<std::size_t>(f)];
        }
      } else {
        // No subwoofer: the LFE fold-down rule applied to (low-band sum + LFE)
        // instead of to LFE alone, since neither has anywhere else to go.
        for (int f = 0; f < frames; ++f) {
          m.fold_input[static_cast<std::size_t>(f)] =
              m.lfe_mix_linear * (lfe[f] + m.low_sum[static_cast<std::size_t>(f)]);
        }
        float* fold_ptr = m.fold_input.data();
        m.fold_filter.split_into(&fold_ptr, 1, frames, m.fold_scratch);
        const float* low = m.fold_scratch.band_channels[0][0];
        for (int f = 0; f < frames; ++f) {
          const float v = kInvSqrt2 * low[f];
          planes[m.plane_l][f] += v;
          planes[m.plane_r][f] += v;
        }
        for (int f = 0; f < frames; ++f) lfe[f] = 0.0f;
      }
    }
  }

  for (int p = 0; p < m.plane_count; ++p) {
    if (p == m.lfe_plane) continue;
    const float trim =
        m.trim_linear[static_cast<std::size_t>(m.roles[static_cast<std::size_t>(p)])];
    Impl::PlaneDelay& d = m.delays[static_cast<std::size_t>(p)];
    float* plane = planes[p];
    if (d.delay_q8 == 0) {
      if (trim != 1.0f) {
        for (int f = 0; f < frames; ++f) plane[f] *= trim;
      }
    } else {
      for (int f = 0; f < frames; ++f) {
        plane[f] =
            rt::lagrange3_fractional_delay(d.buffer, d.write_index, d.delay_q8, trim * plane[f]);
      }
    }
  }
}

void SpeakerStage::reset() noexcept {
  Impl& m = *impl_;
  for (Impl::PlaneDelay& d : m.delays) {
    std::fill(d.buffer.begin(), d.buffer.end(), 0.0f);
    d.write_index = 0;
  }
  m.small_crossover.reset();
  m.fold_filter.reset();
}

int SpeakerStage::latency_samples() const noexcept {
  return static_cast<int>(std::lround(impl_->max_delay_q8 / 256.0));
}

int SpeakerStage::latency_samples_q8() const noexcept { return impl_->max_delay_q8; }

int SpeakerStage::plane_delay_q8(int plane) const noexcept {
  if (plane < 0 || plane >= impl_->plane_count) return 0;
  return impl_->delays[static_cast<std::size_t>(plane)].delay_q8;
}

}  // namespace sonare::playback
