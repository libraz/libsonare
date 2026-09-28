#include "playback/front_end.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "playback/upmix.h"
#include "rt/delay_line.h"
#include "util/db.h"

namespace sonare::playback {

namespace {

constexpr int kMaxSourcePlanes = 8;
constexpr int kMaxBusPlanes = kVirtualSlotCount + 2;
constexpr int kQ8 = 256;

std::vector<std::vector<float>> make_planes(int count, int frames) {
  return std::vector<std::vector<float>>(static_cast<size_t>(count),
                                         std::vector<float>(static_cast<size_t>(frames), 0.0f));
}

}  // namespace

struct FrontEnd::Impl {
  int max_block = 0;
  ChannelLayout input = ChannelLayout::Stereo;
  ChannelLayout source = ChannelLayout::Stereo;
  OutputBus bus{};
  int input_count = 2;
  int source_count = 2;
  /// Canonical plane of each input channel.
  std::array<int, kMaxSourcePlanes> input_plane{};
  bool identity_map = true;

  bool has_upmix = false;
  /// Upmix length N, carried as a plain delay by a front end without an upmix.
  int upmix_frames = 0;
  Upmixer upmix;
  std::array<rt::DelayLine, kMaxSourcePlanes> pad{};
  NightModeDrc drc;
  LayoutConverter converter;

  std::vector<float> silence;
  std::array<const float*, kMaxSourcePlanes> canonical{};
  std::vector<std::vector<float>> src;
  std::array<float*, kMaxSourcePlanes> src_ptrs{};

  // Dialogue level (discrete C plane only) and loudness alignment, ramped over
  // one block when they change.
  int centre_plane = -1;
  float dialogue_target = 1.0f;
  float dialogue_current = 1.0f;
  float loudness_target = 1.0f;
  float loudness_current = 1.0f;

  // Truncated-drain fade: the converter writes a private bus, faded onto the real one.
  std::vector<std::vector<float>> fade_bus;
  std::array<float*, kMaxBusPlanes> fade_ptrs{};
  bool fading = false;
  int fade_len = 1;
  int fade_pos = 0;

  void apply_gains(int frames) noexcept {
    const bool steady = dialogue_current == dialogue_target && loudness_current == loudness_target;
    if (steady && dialogue_current == 1.0f && loudness_current == 1.0f) return;
    const float inv = 1.0f / static_cast<float>(frames);
    for (int p = 0; p < source_count; ++p) {
      float* plane = src_ptrs[static_cast<size_t>(p)];
      const bool centre = p == centre_plane;
      const float start = loudness_current * (centre ? dialogue_current : 1.0f);
      const float end = loudness_target * (centre ? dialogue_target : 1.0f);
      if (start == end) {
        if (end != 1.0f) {
          for (int i = 0; i < frames; ++i) plane[i] *= end;
        }
        continue;
      }
      const float step = (end - start) * inv;
      for (int i = 0; i < frames; ++i) {
        plane[i] *= i + 1 == frames ? end : start + step * static_cast<float>(i + 1);
      }
    }
    dialogue_current = dialogue_target;
    loudness_current = loudness_target;
  }
};

LoudnessGain compute_loudness_gain(const RealtimeConfig& config) noexcept {
  LoudnessGain gain;
  if (!config.has_program_lufs) return gain;
  const float raw = config.target_lufs - config.program_lufs;
  gain.gain_db = std::clamp(raw, kLoudnessMinGainDb, kLoudnessMaxGainDb);
  gain.clamped = gain.gain_db != raw;
  return gain;
}

FrontEnd::FrontEnd() : impl_(std::make_unique<Impl>()) {}
FrontEnd::~FrontEnd() = default;

void FrontEnd::prepare(double sample_rate, int max_block_size, ChannelLayout input,
                       const PrepareConfig& config, const OutputBus& bus) {
  Impl& m = *impl_;
  m.max_block = max_block_size;
  m.input = input;
  m.bus = bus;
  m.input_count = channel_count(input);
  m.source = converted_source_layout(input, bus);
  m.source_count = channel_count(m.source);

  const SpeakerRole* roles = speaker_roles(input);
  m.identity_map = true;
  for (int i = 0; i < m.input_count; ++i) {
    int plane = i;
    if (config.has_channel_map) {
      const SpeakerRole role = config.channel_map[static_cast<size_t>(i)];
      for (int p = 0; p < m.input_count; ++p) {
        if (roles[p] == role) plane = p;
      }
    }
    m.input_plane[static_cast<size_t>(i)] = plane;
    m.identity_map = m.identity_map && plane == i;
  }

  const bool stereo_speakers =
      bus.kind == TargetKind::Speakers && bus.speaker_layout == ChannelLayout::Stereo;
  m.has_upmix = m.source != input;
  m.upmix_frames = stereo_speakers ? 0 : upmix_latency_frames(sample_rate);
  if (m.has_upmix) m.upmix.prepare(sample_rate, max_block_size, m.source);
  for (int p = 0; p < kMaxSourcePlanes; ++p) {
    const bool padded = !m.has_upmix && p < m.input_count;
    m.pad[static_cast<size_t>(p)].prepare(padded ? static_cast<size_t>(m.upmix_frames) : 0);
  }

  m.drc.prepare(sample_rate, max_block_size, m.source);
  m.converter.prepare(sample_rate, max_block_size, m.source, bus);

  m.silence.assign(static_cast<size_t>(max_block_size), 0.0f);
  m.src = make_planes(m.source_count, max_block_size);
  for (int p = 0; p < m.source_count; ++p) {
    m.src_ptrs[static_cast<size_t>(p)] = m.src[static_cast<size_t>(p)].data();
  }
  m.fade_bus = make_planes(bus.channel_count(), max_block_size);
  for (int p = 0; p < bus.channel_count(); ++p) {
    m.fade_ptrs[static_cast<size_t>(p)] = m.fade_bus[static_cast<size_t>(p)].data();
  }

  const bool discrete_centre =
      input == ChannelLayout::FivePointOne || input == ChannelLayout::SevenPointOne;
  m.centre_plane = discrete_centre ? static_cast<int>(SpeakerRole::C) : -1;
  reset();
}

void FrontEnd::apply_realtime(const RealtimeConfig& config, bool immediate) noexcept {
  Impl& m = *impl_;
  if (m.has_upmix) {
    UpmixParams params;
    params.enabled = config.upmix_enabled;
    params.center_width = config.upmix_center_width;
    params.front_ambience = config.upmix_front_ambience;
    params.lfe_from_upmix = config.upmix_lfe_from_upmix;
    m.upmix.set_params(params, immediate);
  }
  m.converter.set_lfe_mix_db(config.lfe_mix_db);
  m.drc.set_amount(config.night_amount);
  m.drc.set_target_lufs(config.target_lufs);
  m.dialogue_target = m.centre_plane >= 0 ? db_to_linear(config.dialogue_level_db) : 1.0f;
  m.loudness_target = db_to_linear(compute_loudness_gain(config).gain_db);
  if (immediate) {
    m.dialogue_current = m.dialogue_target;
    m.loudness_current = m.loudness_target;
  }
}

void FrontEnd::process(const float* const* in, float* const* bus, int frames) noexcept {
  Impl& m = *impl_;
  if (frames <= 0) return;
  for (int i = 0; i < m.input_count; ++i) {
    m.canonical[static_cast<size_t>(m.input_plane[static_cast<size_t>(i)])] =
        in != nullptr ? in[i] : m.silence.data();
  }

  if (m.has_upmix) {
    m.upmix.process(m.canonical[0], m.canonical[1], m.src_ptrs.data(), frames);
  } else {
    for (int p = 0; p < m.source_count; ++p) {
      const float* from = m.canonical[static_cast<size_t>(p)];
      float* to = m.src_ptrs[static_cast<size_t>(p)];
      rt::DelayLine& delay = m.pad[static_cast<size_t>(p)];
      for (int i = 0; i < frames; ++i) to[i] = delay.process(from[i]);
    }
  }

  m.apply_gains(frames);
  m.drc.process(m.src_ptrs.data(), frames);

  if (!m.fading) {
    m.converter.process(m.src_ptrs.data(), bus, frames);
    return;
  }
  const int planes = m.bus.channel_count();
  for (int p = 0; p < planes; ++p) {
    std::fill(m.fade_ptrs[static_cast<size_t>(p)], m.fade_ptrs[static_cast<size_t>(p)] + frames,
              0.0f);
  }
  m.converter.process(m.src_ptrs.data(), m.fade_ptrs.data(), frames);
  const float inv = 1.0f / static_cast<float>(m.fade_len);
  for (int i = 0; i < frames; ++i) {
    m.fade_pos = std::min(m.fade_pos + 1, m.fade_len);
    const float gain = 1.0f - static_cast<float>(m.fade_pos) * inv;
    for (int p = 0; p < planes; ++p) {
      bus[p][i] += gain * m.fade_ptrs[static_cast<size_t>(p)][i];
    }
  }
}

void FrontEnd::reset() noexcept {
  Impl& m = *impl_;
  if (m.has_upmix) m.upmix.reset();
  for (auto& delay : m.pad) delay.reset();
  m.drc.reset();
  m.drc.set_frozen(false);
  m.converter.reset();
  m.fading = false;
  m.fade_pos = 0;
  m.dialogue_current = m.dialogue_target;
  m.loudness_current = m.loudness_target;
}

int FrontEnd::latency() const noexcept {
  return impl_->upmix_frames + impl_->drc.latency_samples();
}

int FrontEnd::drain_frames() const noexcept {
  const Impl& m = *impl_;
  const int decay = std::max(m.has_upmix ? m.upmix.decay_frames() : 0, m.converter.decay_frames());
  return latency() + (m.has_upmix ? m.upmix_frames : 0) + decay;
}

ChannelLayout FrontEnd::input_layout() const noexcept { return impl_->input; }

int FrontEnd::input_channel_count() const noexcept { return impl_->input_count; }

void FrontEnd::set_draining(bool draining) noexcept { impl_->drc.set_frozen(draining); }

void FrontEnd::start_fade_out(int frames) noexcept {
  Impl& m = *impl_;
  m.fading = true;
  m.fade_len = std::max(1, frames);
  m.fade_pos = 0;
}

bool FrontEnd::fade_out_done() const noexcept {
  return impl_->fading && impl_->fade_pos >= impl_->fade_len;
}

NightModeDrcHandover FrontEnd::handover() const noexcept { return impl_->drc.handover(); }

void FrontEnd::accept_handover(const NightModeDrcHandover& state) noexcept {
  impl_->drc.accept_handover(state);
}

uint32_t FrontEnd::inactive_stages(const RealtimeConfig& config) const noexcept {
  const Impl& m = *impl_;
  const auto bit = [](Stage stage) { return 1u << static_cast<unsigned>(stage); };
  uint32_t mask = 0;
  if (m.identity_map) mask |= bit(Stage::Reorder);
  if (m.centre_plane < 0 || config.dialogue_level_db == 0.0f) mask |= bit(Stage::DialogueLevel);
  if (!m.has_upmix || !config.upmix_enabled) mask |= bit(Stage::Upmix);
  if (compute_loudness_gain(config).gain_db == 0.0f) mask |= bit(Stage::Loudness);
  if (config.night_amount == 0.0f) mask |= bit(Stage::NightMode);
  if (m.bus.kind == TargetKind::Speakers && m.source == m.bus.speaker_layout) {
    mask |= bit(Stage::LayoutConvert);
  }
  return mask;
}

std::array<int, kStageCount> FrontEnd::stage_latency_q8() const noexcept {
  std::array<int, kStageCount> q8{};
  q8[static_cast<size_t>(Stage::Upmix)] = impl_->upmix_frames * kQ8;
  q8[static_cast<size_t>(Stage::NightMode)] = impl_->drc.latency_samples() * kQ8;
  return q8;
}

}  // namespace sonare::playback
