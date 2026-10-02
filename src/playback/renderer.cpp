#include "playback/renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

#include "mastering/dynamics/brickwall_limiter.h"
#include "playback/binaural.h"
#include "playback/front_end.h"
#include "playback/layout_convert.h"
#include "playback/speaker_stage.h"
#include "rt/delay_line.h"
#include "rt/overflow_counter.h"
#include "rt/rt_publisher.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"
#include "util/non_finite_sample.h"
#include "util/number_format.h"
#include "util/numeric_validation.h"

namespace sonare::playback {

namespace {

constexpr int kMaxInputPlanes = 8;
constexpr int kMaxOutputPlanes = 8;
constexpr int kMaxBusPlanes = kVirtualSlotCount + 2;
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 384000;
constexpr int kMaxBlockFrames = 65536;
constexpr int kQ8 = 256;
constexpr double kSecondsPerMs = 1e-3;
constexpr int kLayoutCount = 4;
/// Instances per input layout under `auto`: a truncated drain fades out on one
/// while its layout becomes active again on the other.
constexpr int kInstancesPerLayout = 2;
/// Limiter parameter id of `ceiling_db` (see BrickwallLimiter::set_parameter).
constexpr unsigned kLimiterCeilingParam = 0;

int layout_index(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
      return 0;
    case ChannelLayout::Stereo:
      return 1;
    case ChannelLayout::FivePointOne:
      return 2;
    case ChannelLayout::SevenPointOne:
      return 3;
  }
  return 1;
}

constexpr ChannelLayout kLayouts[kLayoutCount] = {ChannelLayout::Mono, ChannelLayout::Stereo,
                                                  ChannelLayout::FivePointOne,
                                                  ChannelLayout::SevenPointOne};

bool layout_for_channel_count(int channels, ChannelLayout* out) noexcept {
  for (ChannelLayout layout : kLayouts) {
    if (channel_count(layout) == channels) {
      *out = layout;
      return true;
    }
  }
  return false;
}

const char* layout_text(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
      return "mono";
    case ChannelLayout::Stereo:
      return "stereo";
    case ChannelLayout::FivePointOne:
      return "5.1";
    case ChannelLayout::SevenPointOne:
      return "7.1";
  }
  return "stereo";
}

uint32_t stage_bit(Stage stage) noexcept { return 1u << static_cast<unsigned>(stage); }

std::vector<std::vector<float>> make_planes(int count, int frames) {
  return std::vector<std::vector<float>>(static_cast<size_t>(count),
                                         std::vector<float>(static_cast<size_t>(frames), 0.0f));
}

}  // namespace

struct PlaybackRenderer::Impl {
  // Control thread.
  RendererConfig config;
  int sample_rate = 0;
  int max_block = 0;
  bool auto_layout = false;
  OutputBus bus{};
  int out_count = 0;
  int latency = 0;
  std::array<int, kStageCount> stage_q8{};
  bool hrtf_ignored = false;
  rt::RtPublisher<RealtimeConfig> publisher;

  // Front ends: one for a fixed layout, kInstancesPerLayout per layout under auto.
  std::vector<std::unique_ptr<FrontEnd>> front_ends;
  int active = 0;
  int draining = -1;
  int fading = -1;
  int drain_left = 0;
  int fade_frames = 1;

  // Audio thread.
  const RealtimeConfig* adopted = nullptr;
  bool limiter_enabled = true;
  std::vector<std::vector<float>> staging;
  std::array<const float*, kMaxInputPlanes> staging_ptrs{};
  std::vector<std::vector<float>> bus_planes;
  std::array<float*, kMaxBusPlanes> bus_ptrs{};
  std::vector<std::vector<float>> ear_planes;
  std::array<float*, kMaxOutputPlanes> out_ptrs{};

  std::unique_ptr<SpeakerStage> speakers;
  std::unique_ptr<BinauralRenderer> binaural;

  mastering::dynamics::BrickwallLimiter main_limiter;
  mastering::dynamics::BrickwallLimiter lfe_limiter;
  int lfe_plane = -1;
  std::array<float*, kMaxOutputPlanes> main_ptrs{};
  int main_count = 0;
  std::array<rt::DelayLine, kMaxOutputPlanes> bypass{};
  std::vector<std::vector<float>> bypass_planes;

  // Diagnostics, written by the audio thread.
  std::atomic<int> active_index{0};
  std::atomic<uint64_t> layout_switches{0};
  std::atomic<uint64_t> truncated_drains{0};
  std::atomic<float> limiter_gain_reduction_db{0.0f};
  rt::OverflowCounter non_finite;

  bool busy(int index) const noexcept {
    return index == active || index == draining || index == fading;
  }

  void apply(const RealtimeConfig& rt, bool immediate) noexcept {
    for (int i = 0; i < static_cast<int>(front_ends.size()); ++i) {
      front_ends[static_cast<size_t>(i)]->apply_realtime(rt, immediate || !busy(i));
    }
    if (speakers) speakers->set_levels(rt.trim_db, rt.lfe_gain_db, rt.lfe_mix_db);
    if (binaural) {
      BinauralParams params;
      params.room_enabled = rt.room_enabled;
      params.room_mix_db = rt.room_mix_db;
      params.head_tracking_enabled = rt.head_tracking_enabled;
      binaural->set_params(params);
    }
    main_limiter.set_parameter(kLimiterCeilingParam, rt.limiter_ceiling_db);
    lfe_limiter.set_parameter(kLimiterCeilingParam, rt.limiter_ceiling_db);
    limiter_enabled = rt.limiter_enabled;
  }

  void adopt() noexcept {
    publisher.acquire();
    const RealtimeConfig* current = publisher.current();
    if (current != nullptr && current != adopted) {
      adopted = current;
      apply(*current, false);
    }
  }

  void switch_to(ChannelLayout layout) noexcept {
    const int base = layout_index(layout) * kInstancesPerLayout;
    if (fading >= 0) {
      front_ends[static_cast<size_t>(fading)]->reset();
      fading = -1;
    }
    if (draining >= 0) {
      front_ends[static_cast<size_t>(draining)]->start_fade_out(fade_frames);
      fading = draining;
      draining = -1;
      truncated_drains.fetch_add(1, std::memory_order_relaxed);
    }
    int next = base;
    for (int k = 0; k < kInstancesPerLayout; ++k) {
      if (!busy(base + k)) {
        next = base + k;
        break;
      }
    }
    FrontEnd& from = *front_ends[static_cast<size_t>(active)];
    FrontEnd& to = *front_ends[static_cast<size_t>(next)];
    to.accept_handover(from.handover());
    from.set_draining(true);
    draining = active;
    drain_left = from.drain_frames();
    active = next;
    active_index.store(next, std::memory_order_relaxed);
    layout_switches.fetch_add(1, std::memory_order_relaxed);
  }

  /// Renders `frames` of the staged input into out_ptrs.
  void render(int in_channels, int frames) noexcept {
    rt::ScopedNoDenormals no_denormals;
    adopt();
    if (auto_layout &&
        in_channels != front_ends[static_cast<size_t>(active)]->input_channel_count()) {
      ChannelLayout layout = ChannelLayout::Stereo;
      layout_for_channel_count(in_channels, &layout);
      switch_to(layout);
    }

    const int bus_count = bus.channel_count();
    for (int p = 0; p < bus_count; ++p) {
      std::fill(bus_ptrs[static_cast<size_t>(p)], bus_ptrs[static_cast<size_t>(p)] + frames, 0.0f);
    }
    front_ends[static_cast<size_t>(active)]->process(staging_ptrs.data(), bus_ptrs.data(), frames);
    if (draining >= 0) {
      FrontEnd& fe = *front_ends[static_cast<size_t>(draining)];
      fe.process(nullptr, bus_ptrs.data(), frames);
      drain_left -= frames;
      if (drain_left <= 0) {
        fe.reset();
        draining = -1;
      }
    }
    if (fading >= 0) {
      FrontEnd& fe = *front_ends[static_cast<size_t>(fading)];
      fe.process(nullptr, bus_ptrs.data(), frames);
      if (fe.fade_out_done()) {
        fe.reset();
        fading = -1;
      }
    }

    if (speakers) {
      speakers->process(bus_ptrs.data(), frames);
    } else {
      binaural->process(bus_ptrs.data(), out_ptrs[0], out_ptrs[1], frames);
    }

    for (int p = 0; p < out_count; ++p) {
      const float* from = out_ptrs[static_cast<size_t>(p)];
      float* to = bypass_planes[static_cast<size_t>(p)].data();
      rt::DelayLine& delay = bypass[static_cast<size_t>(p)];
      for (int i = 0; i < frames; ++i) to[i] = delay.process(from[i]);
    }
    main_limiter.process(main_ptrs.data(), main_count, frames);
    if (lfe_plane >= 0) lfe_limiter.process(&out_ptrs[static_cast<size_t>(lfe_plane)], 1, frames);
    const float gain_reduction = lfe_plane >= 0 ? std::min(main_limiter.last_gain_reduction_db(),
                                                           lfe_limiter.last_gain_reduction_db())
                                                : main_limiter.last_gain_reduction_db();
    limiter_gain_reduction_db.store(gain_reduction, std::memory_order_relaxed);
  }

  /// Output plane p of the last render (limited or bypassed).
  const float* output_plane(int p) const noexcept {
    return limiter_enabled ? out_ptrs[static_cast<size_t>(p)]
                           : bypass_planes[static_cast<size_t>(p)].data();
  }

  bool accepts_input(int in_channels) const noexcept {
    if (auto_layout) {
      ChannelLayout layout = ChannelLayout::Stereo;
      return layout_for_channel_count(in_channels, &layout);
    }
    return in_channels == front_ends[0]->input_channel_count();
  }
};

std::string diagnostics_to_json(const RendererDiagnostics& diagnostics) {
  const auto number = [](double value) { return util::format_general(value, 6); };
  std::string out = "{\"active_input_layout\":\"";
  out += layout_text(diagnostics.active_input_layout);
  out += "\",\"layout_switches\":" + std::to_string(diagnostics.layout_switches);
  out += ",\"truncated_drains\":" + std::to_string(diagnostics.truncated_drains);
  out += ",\"inactive_stages\":[";
  bool first = true;
  for (int s = 0; s < kStageCount; ++s) {
    if ((diagnostics.inactive_stages & (1u << static_cast<unsigned>(s))) == 0) continue;
    if (!first) out += ',';
    first = false;
    out += '"';
    out += stage_name(static_cast<Stage>(s));
    out += '"';
  }
  out += "],\"latency\":{\"samples\":" + std::to_string(diagnostics.latency_samples);
  out += ",\"stages\":{";
  for (int s = 0; s < kStageCount; ++s) {
    const int q8 = diagnostics.stage_latency_q8[static_cast<size_t>(s)];
    if (s > 0) out += ',';
    out += '"';
    out += stage_name(static_cast<Stage>(s));
    out += "\":{\"samples\":" +
           std::to_string(static_cast<long>(std::lround(static_cast<double>(q8) / kQ8)));
    out += ",\"q8\":" + std::to_string(q8) + "}";
  }
  out += "}},\"loudness_gain_db\":" + number(diagnostics.loudness_gain_db);
  out += ",\"loudness_gain_clamped\":";
  out += diagnostics.loudness_gain_clamped ? "true" : "false";
  out += ",\"hrtf_ignored\":";
  out += diagnostics.hrtf_ignored ? "true" : "false";
  out += ",\"limiter_gain_reduction_db\":" + number(diagnostics.limiter_gain_reduction_db);
  out += ",\"non_finite_discards\":" + std::to_string(diagnostics.non_finite_discards);
  out += "}";
  return out;
}

PlaybackRenderer::PlaybackRenderer(const RendererConfig& config, const HrtfSet* hrtf,
                                   int sample_rate, int max_block_size)
    : impl_(std::make_unique<Impl>()) {
  SONARE_CHECK_RANGE("sample_rate", sample_rate, kMinSampleRate, kMaxSampleRate);
  SONARE_CHECK_RANGE("max_block_size", max_block_size, 1, kMaxBlockFrames);
  validate_renderer_config(config);

  Impl& m = *impl_;
  m.config = config;
  m.sample_rate = sample_rate;
  m.max_block = max_block_size;
  const PrepareConfig& prepare = config.prepare;
  m.auto_layout = prepare.input_layout == InputLayout::Auto;
  m.bus = make_output_bus(prepare);
  m.out_count = ::sonare::playback::output_channel_count(prepare);
  const auto rate = static_cast<double>(sample_rate);

  // Back end.
  if (prepare.target_kind == TargetKind::Speakers) {
    m.hrtf_ignored = hrtf != nullptr;
    m.speakers = std::make_unique<SpeakerStage>();
    m.speakers->prepare(rate, max_block_size, prepare.target_layout, prepare.speakers,
                        prepare.bass_management);
  } else {
    m.binaural = std::make_unique<BinauralRenderer>();
#ifdef __EMSCRIPTEN__
    SONARE_CHECK_MSG(hrtf != nullptr, ErrorCode::InvalidParameter, "hrtf required");
    m.binaural->prepare(rate, max_block_size, *hrtf, m.bus.slots, prepare.room_preset);
#else
    if (hrtf != nullptr) {
      m.binaural->prepare(rate, max_block_size, *hrtf, m.bus.slots, prepare.room_preset);
    } else {
      m.binaural->prepare(rate, max_block_size, HrtfSet::builtin_default(), m.bus.slots,
                          prepare.room_preset);
    }
#endif
  }

  // Front ends.
  if (m.auto_layout) {
    for (ChannelLayout layout : kLayouts) {
      for (int k = 0; k < kInstancesPerLayout; ++k) {
        auto fe = std::make_unique<FrontEnd>();
        fe->prepare(rate, max_block_size, layout, prepare, m.bus);
        m.front_ends.push_back(std::move(fe));
      }
    }
    m.active = layout_index(ChannelLayout::Stereo) * kInstancesPerLayout;
  } else {
    auto fe = std::make_unique<FrontEnd>();
    fe->prepare(rate, max_block_size, to_channel_layout(prepare.input_layout), prepare, m.bus);
    m.front_ends.push_back(std::move(fe));
    m.active = 0;
  }
  m.active_index.store(m.active, std::memory_order_relaxed);
  m.fade_frames =
      std::max(1, static_cast<int>(std::lround(kTruncatedDrainFadeMs * kSecondsPerMs * rate)));

  // Planes.
  m.staging = make_planes(kMaxInputPlanes, max_block_size);
  for (int p = 0; p < kMaxInputPlanes; ++p) {
    m.staging_ptrs[static_cast<size_t>(p)] = m.staging[static_cast<size_t>(p)].data();
  }
  m.bus_planes = make_planes(m.bus.channel_count(), max_block_size);
  for (int p = 0; p < m.bus.channel_count(); ++p) {
    m.bus_ptrs[static_cast<size_t>(p)] = m.bus_planes[static_cast<size_t>(p)].data();
  }
  if (m.speakers) {
    for (int p = 0; p < m.out_count; ++p) m.out_ptrs[static_cast<size_t>(p)] = m.bus_ptrs[p];
    m.lfe_plane = lfe_index(prepare.target_layout);
  } else {
    m.ear_planes = make_planes(2, max_block_size);
    for (int p = 0; p < 2; ++p) {
      m.out_ptrs[static_cast<size_t>(p)] = m.ear_planes[static_cast<size_t>(p)].data();
    }
  }
  m.main_count = 0;
  for (int p = 0; p < m.out_count; ++p) {
    if (p != m.lfe_plane) m.main_ptrs[static_cast<size_t>(m.main_count++)] = m.out_ptrs[p];
  }

  // Output limiter, and the equal delay used while it is disabled.
  mastering::dynamics::BrickwallLimiterConfig limiter_config;
  limiter_config.ceiling_db = config.realtime.limiter_ceiling_db;
  limiter_config.lookahead_ms = kOutputLimiterLookaheadMs;
  m.main_limiter.set_config(limiter_config);
  m.lfe_limiter.set_config(limiter_config);
  m.main_limiter.prepare(rate, max_block_size);
  m.lfe_limiter.prepare(rate, max_block_size);
  const int limiter_latency = m.main_limiter.latency_samples();
  for (auto& delay : m.bypass) delay.prepare(static_cast<size_t>(limiter_latency));
  m.bypass_planes = make_planes(m.out_count, max_block_size);

  // Latency: front end + distance compensation + limiter.
  const FrontEnd& fe = *m.front_ends[static_cast<size_t>(m.active)];
  m.stage_q8 = fe.stage_latency_q8();
  if (m.speakers) {
    m.stage_q8[static_cast<size_t>(Stage::SpeakerCalibration)] = m.speakers->latency_samples_q8();
  }
  m.stage_q8[static_cast<size_t>(Stage::OutputLimiter)] = limiter_latency * kQ8;
  m.latency = fe.latency() + (m.speakers ? m.speakers->latency_samples() : 0) + limiter_latency;

  m.apply(config.realtime, true);
  m.publisher.publish(std::make_shared<const RealtimeConfig>(config.realtime));
  m.publisher.acquire();
  m.adopted = m.publisher.current();
}

PlaybackRenderer::~PlaybackRenderer() = default;

void PlaybackRenderer::set_config(const RendererConfig& config) {
  validate_renderer_config(config);
  Impl& m = *impl_;
  const std::string key = first_prepare_difference(m.config.prepare, config.prepare);
  SONARE_CHECK_MSG(key.empty(), ErrorCode::InvalidParameter, "requires a new renderer: " + key);
  m.publisher.publish(std::make_shared<const RealtimeConfig>(config.realtime));
  m.config.realtime = config.realtime;
}

RendererConfig PlaybackRenderer::config() const { return impl_->config; }

void PlaybackRenderer::set_head_orientation(float yaw_deg, float pitch_deg,
                                            float roll_deg) noexcept {
  if (!impl_->binaural) return;
  if (!std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) || !std::isfinite(roll_deg)) return;
  HeadPose pose;
  pose.yaw_deg = yaw_deg;
  pose.pitch_deg = pitch_deg;
  pose.roll_deg = roll_deg;
  impl_->binaural->set_head_pose(pose);
}

bool PlaybackRenderer::process_planar(const float* const* in, int in_channels, float* const* out,
                                      int out_channels, int frames) noexcept {
  Impl& m = *impl_;
  if (frames == 0) return true;
  if (frames < 0 || frames > m.max_block || in == nullptr || out == nullptr) return false;
  if (out_channels != m.out_count || !m.accepts_input(in_channels)) return false;
  for (int c = 0; c < in_channels; ++c) {
    if (in[c] == nullptr) return false;
  }
  for (int c = 0; c < out_channels; ++c) {
    if (out[c] == nullptr) return false;
  }

  uint32_t replaced = 0;
  for (int c = 0; c < in_channels; ++c) {
    float* to = m.staging[static_cast<size_t>(c)].data();
    std::copy(in[c], in[c] + frames, to);
    replaced += static_cast<uint32_t>(resolve_non_finite_run(SampleDestination::kIrreversibleOutput,
                                                             to, static_cast<size_t>(frames)));
  }
  if (replaced > 0) m.non_finite.add(replaced);

  m.render(in_channels, frames);
  for (int c = 0; c < out_channels; ++c) {
    const float* from = m.output_plane(c);
    std::copy(from, from + frames, out[c]);
  }
  return true;
}

bool PlaybackRenderer::process_interleaved(const float* in, int in_channels, float* out,
                                           int out_channels, int frames) noexcept {
  Impl& m = *impl_;
  if (frames == 0) return true;
  if (frames < 0 || frames > m.max_block || in == nullptr || out == nullptr) return false;
  if (out_channels != m.out_count || !m.accepts_input(in_channels)) return false;
  const auto in_end = in + static_cast<size_t>(frames) * static_cast<size_t>(in_channels);
  const auto out_end = out + static_cast<size_t>(frames) * static_cast<size_t>(out_channels);
  const std::less<const void*> before;
  if (before(in, out_end) && before(out, in_end)) return false;

  uint32_t replaced = 0;
  for (int c = 0; c < in_channels; ++c) {
    float* to = m.staging[static_cast<size_t>(c)].data();
    for (int i = 0; i < frames; ++i) {
      float sample =
          in[static_cast<size_t>(i) * static_cast<size_t>(in_channels) + static_cast<size_t>(c)];
      if (resolve_non_finite(SampleDestination::kIrreversibleOutput, sample)) ++replaced;
      to[i] = sample;
    }
  }
  if (replaced > 0) m.non_finite.add(replaced);

  m.render(in_channels, frames);
  for (int c = 0; c < out_channels; ++c) {
    const float* from = m.output_plane(c);
    for (int i = 0; i < frames; ++i) {
      out[static_cast<size_t>(i) * static_cast<size_t>(out_channels) + static_cast<size_t>(c)] =
          from[i];
    }
  }
  return true;
}

void PlaybackRenderer::reset() noexcept {
  Impl& m = *impl_;
  for (auto& fe : m.front_ends) fe->reset();
  m.draining = -1;
  m.fading = -1;
  m.drain_left = 0;
  if (m.speakers) m.speakers->reset();
  if (m.binaural) m.binaural->reset();
  m.main_limiter.reset();
  m.lfe_limiter.reset();
  for (auto& delay : m.bypass) delay.reset();
}

int PlaybackRenderer::latency_samples() const noexcept { return impl_->latency; }

int PlaybackRenderer::input_channel_count() const noexcept {
  const Impl& m = *impl_;
  return m.front_ends[static_cast<size_t>(m.active_index.load(std::memory_order_relaxed))]
      ->input_channel_count();
}

int PlaybackRenderer::output_channel_count() const noexcept { return impl_->out_count; }

int PlaybackRenderer::sample_rate() const noexcept { return impl_->sample_rate; }

int PlaybackRenderer::max_block_size() const noexcept { return impl_->max_block; }

RendererDiagnostics PlaybackRenderer::diagnostics() const {
  const Impl& m = *impl_;
  const RealtimeConfig& rt = m.config.realtime;
  RendererDiagnostics d;
  const FrontEnd& fe =
      *m.front_ends[static_cast<size_t>(m.active_index.load(std::memory_order_relaxed))];
  d.active_input_layout = fe.input_layout();
  d.layout_switches = m.layout_switches.load(std::memory_order_relaxed);
  d.truncated_drains = m.truncated_drains.load(std::memory_order_relaxed);

  uint32_t inactive = fe.inactive_stages(rt);
  if (m.speakers) {
    inactive |=
        stage_bit(Stage::Binaural) | stage_bit(Stage::RoomEarly) | stage_bit(Stage::RoomLate);
    bool trimmed = false;
    for (float trim : rt.trim_db) trimmed = trimmed || trim != 0.0f;
    if (m.speakers->latency_samples_q8() == 0 && !trimmed) {
      inactive |= stage_bit(Stage::SpeakerCalibration);
    }
    if (!m.config.prepare.bass_management.enabled) inactive |= stage_bit(Stage::BassManagement);
  } else {
    inactive |= stage_bit(Stage::SpeakerCalibration) | stage_bit(Stage::BassManagement);
    if (m.config.prepare.room_preset == RoomPreset::None || !rt.room_enabled) {
      inactive |= stage_bit(Stage::RoomEarly) | stage_bit(Stage::RoomLate);
    }
  }
  if (!rt.limiter_enabled) inactive |= stage_bit(Stage::OutputLimiter);
  d.inactive_stages = inactive;

  d.stage_latency_q8 = m.stage_q8;
  d.latency_samples = m.latency;
  const LoudnessGain gain = compute_loudness_gain(rt);
  d.loudness_gain_db = gain.gain_db;
  d.loudness_gain_clamped = gain.clamped;
  d.hrtf_ignored = m.hrtf_ignored;
  d.limiter_gain_reduction_db = m.limiter_gain_reduction_db.load(std::memory_order_relaxed);
  d.non_finite_discards = m.non_finite.load();
  return d;
}

uint32_t PlaybackRenderer::non_finite_discard_count() const noexcept {
  return impl_->non_finite.load();
}

std::vector<float> render_interleaved(const float* in, size_t frames, int in_channels,
                                      int sample_rate, const RendererConfig& config,
                                      const HrtfSet* hrtf, int* out_channels) {
  // Offline entry point: unlike process_interleaved below, the caller does
  // not own the finite check here.
  SONARE_CHECK_MSG(frames != 0, ErrorCode::InvalidParameter, "input samples must not be empty");
  SONARE_CHECK_MSG(in != nullptr, ErrorCode::InvalidParameter, "input samples is null");
  SONARE_CHECK_MSG(in_channels > 0, ErrorCode::InvalidParameter, "in_channels must be positive");
  SONARE_CHECK_MSG(numeric::all_finite(in, frames * static_cast<size_t>(in_channels)),
                   ErrorCode::InvalidParameter, "input samples contains a non-finite sample");
  PlaybackRenderer renderer(config, hrtf, sample_rate, kOfflineRenderBlockFrames);
  const bool auto_layout = config.prepare.input_layout == InputLayout::Auto;
  ChannelLayout layout = ChannelLayout::Stereo;
  const bool valid =
      auto_layout ? layout_for_channel_count(in_channels, &layout)
                  : in_channels == channel_count(to_channel_layout(config.prepare.input_layout));
  SONARE_CHECK_MSG(
      valid, ErrorCode::InvalidParameter,
      "in_channels " + std::to_string(in_channels) + " does not match the input layout");

  const int channels = renderer.output_channel_count();
  const auto latency = static_cast<size_t>(renderer.latency_samples());
  const size_t total = frames + latency;
  const auto in_ch = static_cast<size_t>(in_channels);
  const auto out_ch = static_cast<size_t>(channels);
  std::vector<float> result(frames * out_ch, 0.0f);
  std::vector<float> in_block(static_cast<size_t>(kOfflineRenderBlockFrames) * in_ch, 0.0f);
  std::vector<float> out_block(static_cast<size_t>(kOfflineRenderBlockFrames) * out_ch, 0.0f);

  for (size_t start = 0; start < total; start += static_cast<size_t>(kOfflineRenderBlockFrames)) {
    const size_t count = std::min(static_cast<size_t>(kOfflineRenderBlockFrames), total - start);
    std::fill(in_block.begin(), in_block.end(), 0.0f);
    if (start < frames) {
      const size_t available = std::min(count, frames - start);
      std::copy(in + start * in_ch, in + (start + available) * in_ch, in_block.begin());
    }
    const bool ok = renderer.process_interleaved(in_block.data(), in_channels, out_block.data(),
                                                 channels, static_cast<int>(count));
    SONARE_CHECK_MSG(ok, ErrorCode::InvalidParameter, "playback render rejected a block");
    for (size_t i = 0; i < count; ++i) {
      const size_t t = start + i;
      if (t < latency) continue;
      std::copy(out_block.begin() + static_cast<std::ptrdiff_t>(i * out_ch),
                out_block.begin() + static_cast<std::ptrdiff_t>((i + 1) * out_ch),
                result.begin() + static_cast<std::ptrdiff_t>((t - latency) * out_ch));
    }
  }
  if (out_channels != nullptr) *out_channels = channels;
  return result;
}

}  // namespace sonare::playback
