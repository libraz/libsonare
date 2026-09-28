#include "playback/layout_convert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "mixing/downmix.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::playback {

namespace {

constexpr float kLfeCutoffHz = 120.0f;
/// -60 dB in amplitude: the decay horizon reported by decay_frames().
constexpr float kDecayThreshold = 1e-3f;
/// Longest fold-down impulse response simulated when measuring the decay.
constexpr double kDecaySimulationSeconds = 2.0;

// Canonical WAVE_FORMAT_EXTENSIBLE plane indices (SpeakerRole enum order).
constexpr int kPlaneL = static_cast<int>(SpeakerRole::L);
constexpr int kPlaneR = static_cast<int>(SpeakerRole::R);
constexpr int kPlaneC = static_cast<int>(SpeakerRole::C);
constexpr int kPlaneLfe = static_cast<int>(SpeakerRole::LFE);
constexpr int kPlaneLs = static_cast<int>(SpeakerRole::Ls);
constexpr int kPlaneRs = static_cast<int>(SpeakerRole::Rs);
constexpr int kPlaneLss = static_cast<int>(SpeakerRole::Lss);
constexpr int kPlaneRss = static_cast<int>(SpeakerRole::Rss);

mastering::multiband::CrossoverConfig fold_crossover_config() {
  mastering::multiband::CrossoverConfig config;
  config.cutoffs_hz = {kLfeCutoffHz};
  config.slope = mastering::multiband::CrossoverSlope::LR4;
  config.mode = mastering::multiband::CrossoverMode::LinkwitzRiley;
  return config;
}

/// Last index at which |h| exceeds -60 dB of its peak, plus one.
int decay_length(const std::vector<float>& h) {
  float peak = 0.0f;
  for (float v : h) peak = std::max(peak, std::abs(v));
  const float threshold = peak * kDecayThreshold;
  for (int i = static_cast<int>(h.size()) - 1; i >= 0; --i) {
    if (std::abs(h[static_cast<size_t>(i)]) > threshold) return i + 1;
  }
  return 0;
}

}  // namespace

OutputBus make_output_bus(const PrepareConfig& config) noexcept {
  OutputBus bus;
  bus.kind = config.target_kind;
  if (config.target_kind == TargetKind::Speakers) {
    bus.speaker_layout = config.target_layout;
  } else {
    bus.slots = headphone_slots(config.input_layout);
  }
  return bus;
}

void validate_channel_map(ChannelLayout layout, const SpeakerRole* map, int length) {
  const int expected = channel_count(layout);
  SONARE_CHECK_MSG(length == expected, ErrorCode::InvalidParameter,
                   "input.channel_map length " + std::to_string(length) +
                       " does not match layout channel count " + std::to_string(expected));

  std::array<bool, kSpeakerRoleCount> required{};
  const SpeakerRole* layout_roles = speaker_roles(layout);
  for (int i = 0; i < expected; ++i) required[static_cast<size_t>(layout_roles[i])] = true;

  std::array<int, kSpeakerRoleCount> seen{};
  for (int i = 0; i < length; ++i) {
    const auto role_index = static_cast<size_t>(map[i]);
    SONARE_CHECK_MSG(role_index < kSpeakerRoleCount, ErrorCode::InvalidParameter,
                     "input.channel_map: unrecognized role at index " + std::to_string(i));
    SONARE_CHECK_MSG(required[role_index], ErrorCode::InvalidParameter,
                     std::string("input.channel_map: role ") + speaker_role_name(map[i]) +
                         " is not part of the layout");
    ++seen[role_index];
    SONARE_CHECK_MSG(seen[role_index] == 1, ErrorCode::InvalidParameter,
                     std::string("input.channel_map: duplicate role ") + speaker_role_name(map[i]));
  }
  for (int r = 0; r < kSpeakerRoleCount; ++r) {
    SONARE_CHECK_MSG(!required[static_cast<size_t>(r)] || seen[static_cast<size_t>(r)] == 1,
                     ErrorCode::InvalidParameter,
                     std::string("input.channel_map: missing role ") +
                         speaker_role_name(static_cast<SpeakerRole>(r)));
  }
}

ChannelLayout converted_source_layout(ChannelLayout input, const OutputBus& bus) noexcept {
  const bool stereo_speakers =
      bus.kind == TargetKind::Speakers && bus.speaker_layout == ChannelLayout::Stereo;
  if (input != ChannelLayout::Stereo || stereo_speakers) return input;
  return bus.kind == TargetKind::Speakers && bus.speaker_layout == ChannelLayout::SevenPointOne
             ? ChannelLayout::SevenPointOne
             : ChannelLayout::FivePointOne;
}

struct LayoutConverter::Impl {
  enum class Mode {
    PassThrough,
    MonoToStereo,
    MonoToCentre,
    FiveToSevenExpand,
    NarrowDownmix,
    Virtualize
  };

  ChannelLayout source = ChannelLayout::Stereo;
  OutputBus bus{};
  Mode mode = Mode::PassThrough;

  // NarrowDownmix: downmix() output scratch, channel_count(bus.speaker_layout) planes.
  std::vector<std::vector<float>> downmix_scratch;
  std::vector<float*> downmix_scratch_ptrs;

  // Virtualize: destination bus plane for each source plane (-1 for LFE, folded instead).
  std::array<int, kSpeakerRoleCount> virtualize_dst{};

  // LFE fold-down rule (bus without an LFE plane: stereo speakers, headphones).
  bool fold_needed = false;
  int fold_dst[2] = {-1, -1};
  float fold_gain = 1.0f;  // linear, from lfe_mix_db
  mastering::multiband::Crossover fold_filter{fold_crossover_config()};
  mastering::multiband::CrossoverScratch fold_scratch;
  std::vector<float> fold_scaled;
  int decay = 0;
};

LayoutConverter::LayoutConverter() : impl_(std::make_unique<Impl>()) {}
LayoutConverter::~LayoutConverter() = default;

void LayoutConverter::prepare(double sample_rate, int max_block_size, ChannelLayout source,
                              const OutputBus& bus) {
  Impl& m = *impl_;
  m.source = source;
  m.bus = bus;

  const bool bus_has_lfe = bus.kind == TargetKind::Speakers && lfe_index(bus.speaker_layout) >= 0;
  m.fold_needed = lfe_index(source) >= 0 && !bus_has_lfe;
  m.fold_dst[0] = bus.kind == TargetKind::Speakers ? kPlaneL : bus.direct_left_index();
  m.fold_dst[1] = bus.kind == TargetKind::Speakers ? kPlaneR : bus.direct_right_index();

  if (bus.kind == TargetKind::Headphones) {
    m.mode = Impl::Mode::Virtualize;
    m.virtualize_dst.fill(-1);
    const SpeakerRole* roles = speaker_roles(source);
    for (int i = 0; i < channel_count(source); ++i) {
      const SpeakerRole role = roles[i];
      if (role == SpeakerRole::LFE) continue;  // handled by the fold-down path below
      VirtualSlot slot = VirtualSlot::C;
      if (!slot_for_role(source, role, &slot)) continue;
      for (int s = 0; s < bus.slots.count; ++s) {
        if (bus.slots.slots[s] == slot) {
          m.virtualize_dst[static_cast<size_t>(i)] = s;
          break;
        }
      }
    }
  } else if (source == bus.speaker_layout) {
    m.mode = Impl::Mode::PassThrough;
  } else if (source == ChannelLayout::Mono) {
    m.mode = bus.speaker_layout == ChannelLayout::Stereo ? Impl::Mode::MonoToStereo
                                                         : Impl::Mode::MonoToCentre;
  } else if (source == ChannelLayout::FivePointOne &&
             bus.speaker_layout == ChannelLayout::SevenPointOne) {
    m.mode = Impl::Mode::FiveToSevenExpand;
  } else {
    // Only remaining table cells: FivePointOne/SevenPointOne -> Stereo, SevenPointOne ->
    // FivePointOne.
    m.mode = Impl::Mode::NarrowDownmix;
    const int ndst = channel_count(bus.speaker_layout);
    m.downmix_scratch.assign(static_cast<size_t>(ndst),
                             std::vector<float>(static_cast<size_t>(max_block_size), 0.0f));
    m.downmix_scratch_ptrs.resize(static_cast<size_t>(ndst));
    for (int c = 0; c < ndst; ++c) {
      m.downmix_scratch_ptrs[static_cast<size_t>(c)] =
          m.downmix_scratch[static_cast<size_t>(c)].data();
    }
  }

  m.fold_filter.prepare(sample_rate, max_block_size, 1);
  m.fold_filter.prepare_scratch(m.fold_scratch, 1, max_block_size);
  m.fold_scaled.assign(static_cast<size_t>(max_block_size), 0.0f);

  // Decay horizon, measured on the prepared filter's own impulse response.
  m.decay = 0;
  if (m.fold_needed) {
    mastering::multiband::Crossover probe(fold_crossover_config());
    probe.prepare(sample_rate, max_block_size, 1);
    mastering::multiband::CrossoverScratch probe_scratch;
    probe.prepare_scratch(probe_scratch, 1, max_block_size);
    const auto sim_len = static_cast<size_t>(kDecaySimulationSeconds * sample_rate);
    std::vector<float> h(sim_len, 0.0f);
    h[0] = 1.0f;
    std::vector<float> response(sim_len, 0.0f);
    for (size_t start = 0; start < sim_len; start += static_cast<size_t>(max_block_size)) {
      const int count =
          static_cast<int>(std::min(static_cast<size_t>(max_block_size), sim_len - start));
      float* ptr = h.data() + start;
      probe.split_into(&ptr, 1, count, probe_scratch);
      std::copy(probe_scratch.band_channels[0][0], probe_scratch.band_channels[0][0] + count,
                response.begin() + static_cast<std::ptrdiff_t>(start));
    }
    m.decay = decay_length(response);
  }
}

void LayoutConverter::set_lfe_mix_db(float db) noexcept { impl_->fold_gain = db_to_linear(db); }

void LayoutConverter::process(const float* const* source, float* const* bus, int frames) noexcept {
  Impl& m = *impl_;
  using sonare::mixing::downmix_coeff::kMinus3dB;

  switch (m.mode) {
    case Impl::Mode::PassThrough: {
      const int n = channel_count(m.source);
      for (int c = 0; c < n; ++c) {
        for (int i = 0; i < frames; ++i) bus[c][i] += source[c][i];
      }
      break;
    }
    case Impl::Mode::MonoToStereo: {
      for (int i = 0; i < frames; ++i) {
        const float v = kMinus3dB * source[0][i];
        bus[kPlaneL][i] += v;
        bus[kPlaneR][i] += v;
      }
      break;
    }
    case Impl::Mode::MonoToCentre: {
      for (int i = 0; i < frames; ++i) bus[kPlaneC][i] += source[0][i];
      break;
    }
    case Impl::Mode::FiveToSevenExpand: {
      for (int i = 0; i < frames; ++i) {
        bus[kPlaneL][i] += source[kPlaneL][i];
        bus[kPlaneR][i] += source[kPlaneR][i];
        bus[kPlaneC][i] += source[kPlaneC][i];
        bus[kPlaneLfe][i] += source[kPlaneLfe][i];
        bus[kPlaneLss][i] += source[kPlaneLs][i];
        bus[kPlaneRss][i] += source[kPlaneRs][i];
        // bus[kPlaneLs], bus[kPlaneRs] (7.1's own Ls/Rs) stay untouched: zero per the table.
      }
      break;
    }
    case Impl::Mode::NarrowDownmix: {
      sonare::mixing::DownmixOptions options;
      options.include_lfe = false;
      sonare::mixing::downmix(m.source, m.bus.speaker_layout, source, m.downmix_scratch_ptrs.data(),
                              static_cast<size_t>(frames), options);
      const int ndst = channel_count(m.bus.speaker_layout);
      for (int c = 0; c < ndst; ++c) {
        for (int i = 0; i < frames; ++i) {
          bus[c][i] += m.downmix_scratch[static_cast<size_t>(c)][static_cast<size_t>(i)];
        }
      }
      break;
    }
    case Impl::Mode::Virtualize: {
      const int n = channel_count(m.source);
      for (int i = 0; i < n; ++i) {
        const int dst = m.virtualize_dst[static_cast<size_t>(i)];
        if (dst < 0) continue;
        for (int f = 0; f < frames; ++f) bus[dst][f] += source[i][f];
      }
      break;
    }
  }

  if (m.fold_needed) {
    const int lfe_plane = lfe_index(m.source);
    for (int i = 0; i < frames; ++i)
      m.fold_scaled[static_cast<size_t>(i)] = m.fold_gain * source[lfe_plane][i];
    float* ptr = m.fold_scaled.data();
    m.fold_filter.split_into(&ptr, 1, frames, m.fold_scratch);
    const float* low = m.fold_scratch.band_channels[0][0];
    for (int i = 0; i < frames; ++i) {
      const float v = kMinus3dB * low[i];
      bus[m.fold_dst[0]][i] += v;
      bus[m.fold_dst[1]][i] += v;
    }
  }
}

void LayoutConverter::reset() noexcept { impl_->fold_filter.reset(); }

int LayoutConverter::decay_frames() const noexcept { return impl_->decay; }

}  // namespace sonare::playback
