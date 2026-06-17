#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "automation/parameter.h"
#include "core/audio.h"
#include "core/channel_layout.h"
#include "core/resample.h"
#include "engine/realtime_engine.h"
#include "engine/tempo_sync.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#if defined(SONARE_WITH_MIXING)
#include "c_api/eq_band_json.h"
#include "c_api/mixing_internal.h"
#include "mixing/api/scene.h"
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/midi_fx_json.h"
#include "c_api/synth_patch_common.h"
#include "midi/builtin_synth.h"
#include "midi/midi_clip.h"
#include "midi/midi_fx.h"
#include "midi/synth/sf2_player.h"
#include "util/json.h"
#endif
#if defined(SONARE_WITH_MASTERING)
#include "mastering/final/dither.h"
#endif
#include <sonare/sonare_c.h>

#include "rt/command.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_GRAPH)
#include "graph/connection.h"
#include "graph/graph.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
namespace json = sonare::util::json;
#endif

namespace {

class CClipPageProvider final : public engine::ClipPagedAudioProvider {
 public:
  CClipPageProvider(int channels, int64_t samples, int64_t page_frames)
      : channels_(channels),
        samples_(samples),
        page_frames_(page_frames),
        pages_(static_cast<size_t>((samples + page_frames - 1) / page_frames)),
        page_ptrs_(pages_.size()) {
    for (auto& page_ptr : page_ptrs_) {
      page_ptr.store(nullptr, std::memory_order_relaxed);
    }
  }

  int num_channels() const noexcept override { return channels_; }
  int64_t num_samples() const noexcept override { return samples_; }
  int64_t page_frames() const noexcept override { return page_frames_; }
  int64_t page_count() const noexcept { return static_cast<int64_t>(pages_.size()); }

  bool sample_at(int channel, int64_t sample, float* out) const noexcept override {
    if (!out || channel < 0 || channel >= channels_ || sample < 0 || sample >= samples_) {
      return false;
    }
    const int64_t page_index = sample / page_frames_;
    const int64_t offset = sample % page_frames_;
    if (page_index < 0 || page_index >= page_count()) return false;
    const Page* page = page_ptrs_[static_cast<size_t>(page_index)].load(std::memory_order_acquire);
    if (!page || channel >= static_cast<int>(page->channels.size()) || offset < 0 ||
        offset >= page->frames) {
      return false;
    }
    *out = page->channels[static_cast<size_t>(channel)][static_cast<size_t>(offset)];
    return true;
  }

  bool supply(int64_t page_index, const float* const* channels, int channel_count, int64_t frames) {
    if (page_index < 0 || page_index >= page_count() || !channels || channel_count != channels_ ||
        frames <= 0 || frames > page_frames_) {
      return false;
    }
    const int64_t page_start = page_index * page_frames_;
    if (page_start >= samples_) return false;
    const int64_t max_frames = std::min<int64_t>(page_frames_, samples_ - page_start);
    if (frames != max_frames) return false;
    auto page = std::make_shared<Page>();
    page->frames = frames;
    page->channels.reserve(static_cast<size_t>(channels_));
    for (int ch = 0; ch < channels_; ++ch) {
      if (!channels[ch]) return false;
      page->channels.emplace_back(channels[ch], channels[ch] + frames);
    }
    const size_t index = static_cast<size_t>(page_index);
    retire_page(std::move(pages_[index]));
    pages_[index] = std::move(page);
    page_ptrs_[index].store(pages_[index].get(), std::memory_order_release);
    return true;
  }

  bool clear(int64_t page_index) {
    if (page_index < 0 || page_index >= page_count()) return false;
    const size_t index = static_cast<size_t>(page_index);
    page_ptrs_[index].store(nullptr, std::memory_order_release);
    retire_page(std::move(pages_[index]));
    return true;
  }

 private:
  struct Page {
    int64_t frames = 0;
    std::vector<std::vector<float>> channels;
  };

  void retire_page(std::shared_ptr<const Page> page) {
    if (page) {
      retired_pages_.push_back(std::move(page));
    }
  }

  int channels_ = 0;
  int64_t samples_ = 0;
  int64_t page_frames_ = 0;
  std::vector<std::shared_ptr<const Page>> pages_;
  mutable std::vector<std::atomic<const Page*>> page_ptrs_;
  std::vector<std::shared_ptr<const Page>> retired_pages_;
};

}  // namespace

struct SonareClipPageProvider {
  std::shared_ptr<CClipPageProvider> provider;
};

namespace {

size_t rounded_nonnegative_sample(double sample) noexcept {
  if (!std::isfinite(sample) || sample <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sample));
}

bool tempo_sync_segments_for_clip(const SonareEngineClip& clip,
                                  std::vector<engine::TempoSyncWarpSegment>* out) {
  if (!out || clip.length_samples <= 0 || clip.clip_offset_samples < 0 ||
      clip.clip_offset_samples >= clip.num_samples) {
    return false;
  }
  out->clear();
  const size_t base_offset = static_cast<size_t>(clip.clip_offset_samples);
  if (clip.warp_anchor_count >= 2 && clip.warp_anchors) {
    out->reserve(clip.warp_anchor_count - 1);
    for (size_t anchor_index = 1; anchor_index < clip.warp_anchor_count; ++anchor_index) {
      const auto& prev = clip.warp_anchors[anchor_index - 1];
      const auto& next = clip.warp_anchors[anchor_index];
      const size_t source_start = rounded_nonnegative_sample(prev.source_sample);
      const size_t source_end = rounded_nonnegative_sample(next.source_sample);
      const size_t target_start = rounded_nonnegative_sample(prev.warp_sample);
      const size_t target_end = rounded_nonnegative_sample(next.warp_sample);
      engine::TempoSyncWarpSegment segment;
      segment.source_offset = base_offset + source_start;
      segment.source_samples = source_end > source_start ? source_end - source_start : 0;
      segment.target_samples = target_end > target_start ? target_end - target_start : 0;
      if (segment.source_offset > static_cast<size_t>(clip.num_samples) ||
          segment.source_samples > static_cast<size_t>(clip.num_samples) - segment.source_offset ||
          segment.source_samples == 0 || segment.target_samples == 0) {
        return false;
      }
      out->push_back(segment);
    }
    return !out->empty();
  }
  engine::TempoSyncWarpSegment segment;
  segment.source_offset = base_offset;
  segment.source_samples = static_cast<size_t>(clip.num_samples) - base_offset;
  segment.target_samples = static_cast<size_t>(clip.length_samples);
  if (segment.source_samples == 0 || segment.target_samples == 0) return false;
  out->push_back(segment);
  return true;
}

}  // namespace

#if defined(SONARE_WITH_GRAPH)
namespace {

std::unique_ptr<rt::ProcessorBase> make_graph_processor(const SonareEngineGraphNode& node) {
  switch (node.type) {
    case 0:
      return std::make_unique<rt::PassProcessor>();
    case 1:
      return std::make_unique<rt::GainProcessor>(node.gain_db);
    default:
      return nullptr;
  }
}

}  // namespace
#endif

namespace {

// Pin the automation curve ordinal mapping used by
// sonare_engine_set_automation_lane (via SonareAutomationPoint.curve_to_next).
// As of the AutomationCurve unification this scheme matches the sample-accurate
// mixer API (sonare_strip_schedule_*_automation), so a single canonical
// ordinal set covers both subsystems and all four bindings.
static_assert(static_cast<int>(automation::CurveType::Linear) == 0,
              "automation::CurveType::Linear must be ordinal 0 to keep "
              "SonareAutomationPoint.curve_to_next ABI stable");
static_assert(static_cast<int>(automation::CurveType::Exponential) == 1,
              "automation::CurveType::Exponential must be ordinal 1");
static_assert(static_cast<int>(automation::CurveType::Hold) == 2,
              "automation::CurveType::Hold must be ordinal 2");
static_assert(static_cast<int>(automation::CurveType::SCurve) == 3,
              "automation::CurveType::SCurve must be ordinal 3");

automation::CurveType curve_from_int(int curve) {
  switch (curve) {
    case 1:
      return automation::CurveType::Exponential;
    case 2:
      return automation::CurveType::Hold;
    case 3:
      return automation::CurveType::SCurve;
    case 0:
    default:
      return automation::CurveType::Linear;
  }
}

int curve_to_int(automation::CurveType curve) { return static_cast<int>(curve); }

void copy_text(char* dest, size_t capacity, const char* src) {
  if (!dest || capacity == 0) return;
  const char* text = src ? src : "";
  std::strncpy(dest, text, capacity - 1);
  dest[capacity - 1] = '\0';
}

std::string fixed_text(const char* src, size_t capacity) {
  const char* end = std::find(src, src + capacity, '\0');
  return std::string(src, end);
}

std::vector<float> interleave_channels(const std::vector<std::vector<float>>& channels) {
  if (channels.empty()) return {};
  const size_t frames = channels[0].size();
  std::vector<float> interleaved(frames * channels.size(), 0.0f);
  for (size_t frame = 0; frame < frames; ++frame) {
    for (size_t ch = 0; ch < channels.size(); ++ch) {
      interleaved[frame * channels.size() + ch] = channels[ch][frame];
    }
  }
  return interleaved;
}

std::vector<std::vector<float>> resample_channels(const std::vector<std::vector<float>>& channels,
                                                  int source_rate, int target_rate) {
  if (source_rate == target_rate) return channels;
  std::vector<std::vector<float>> out;
  out.reserve(channels.size());
  for (const auto& channel : channels) {
    out.push_back(resample(channel.data(), channel.size(), source_rate, target_rate));
  }
  return out;
}

#if defined(SONARE_WITH_MASTERING)
mastering::final::DitherType dither_type_from_int(int value) {
  switch (value) {
    case 1:
      return mastering::final::DitherType::Rpdf;
    case 2:
      return mastering::final::DitherType::Tpdf;
    case 3:
      return mastering::final::DitherType::NoiseShaped;
    case 0:
    default:
      return mastering::final::DitherType::None;
  }
}
#endif

void fill_c_parameter(const automation::ParameterInfo& info, SonareParameterInfo* out) {
  out->id = info.id;
  copy_text(out->name, sizeof(out->name), info.name);
  copy_text(out->unit, sizeof(out->unit), info.unit);
  out->min_value = info.min_value;
  out->max_value = info.max_value;
  out->default_value = info.default_value;
  out->rt_safe = info.rt_safe ? 1 : 0;
  out->default_curve = curve_to_int(info.default_curve);
}

std::vector<automation::ParameterInfo> parameter_metadata_snapshot(
    const automation::ParameterRegistry& registry) {
  std::vector<automation::ParameterInfo> parameters;
  parameters.reserve(registry.parameter_count());
  for (size_t i = 0; i < registry.parameter_count(); ++i) {
    automation::ParameterInfo info{};
    if (registry.parameter_info_by_index(i, &info)) {
      parameters.push_back(info);
    }
  }
  return parameters;
}

void publish_parameter_metadata(SonareRealtimeEngine* engine) {
  engine->engine.automation().set_parameter_metadata(
      parameter_metadata_snapshot(engine->parameters));
}

bool registered_parameter_rejects_realtime(const SonareRealtimeEngine* engine, uint32_t param_id) {
  automation::ParameterInfo info{};
  return engine->parameters.parameter_info(param_id, &info) && !info.rt_safe;
}

void fill_c_marker(const transport::Marker& marker, SonareEngineMarker* out) {
  out->id = marker.id;
  out->ppq = marker.ppq;
  out->kind = marker.kind;
  out->key_fifths = marker.key_fifths;
  out->key_minor = marker.key_minor ? 1 : 0;
  copy_text(out->name, sizeof(out->name), marker.name);
}

SonareEngineMetronomeConfig metronome_to_c(const engine::MetronomeConfig& config) {
  return {config.enabled ? 1 : 0, config.beat_gain, config.accent_gain, config.click_samples,
          config.click_seconds};
}

engine::MetronomeConfig metronome_from_c(const SonareEngineMetronomeConfig& config) {
  engine::MetronomeConfig out;
  out.enabled = config.enabled != 0;
  out.beat_gain = config.beat_gain;
  out.accent_gain = config.accent_gain;
  // When click_samples is 0 the engine derives the length from click_seconds and
  // the prepared sample rate; a positive click_samples overrides that. Treat a
  // non-positive click_seconds as "use the default" so older callers that leave
  // the field zero-initialized keep the 2 ms behavior.
  out.click_samples = config.click_samples;
  if (config.click_seconds > 0.0) {
    out.click_seconds = config.click_seconds;
  }
  return out;
}

#if defined(SONARE_WITH_ARRANGEMENT)
sonare::midi::BuiltinSynthConfig engine_synth_config_from_c(
    const SonareEngineBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

// Binds (or replaces) an engine-owned instrument on a destination, keeping the
// ownership table and the engine's instrument rack in sync. Shared by the
// built-in synth and SF2 instrument entries.
SonareError bind_engine_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                   std::unique_ptr<sonare::midi::MidiInstrument> instrument) {
  for (auto& entry : engine->builtin_instruments) {
    if (entry.first == destination_id) {
      sonare::midi::MidiInstrument* raw = instrument.get();
      if (!engine->engine.set_midi_instrument(destination_id, raw)) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      entry.second = std::move(instrument);
      return SONARE_OK;
    }
  }
  engine->builtin_instruments.emplace_back(destination_id, std::move(instrument));
  sonare::midi::MidiInstrument* raw = engine->builtin_instruments.back().second.get();
  if (!engine->engine.set_midi_instrument(destination_id, raw)) {
    engine->builtin_instruments.pop_back();
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

uint64_t pack_midi_note(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return static_cast<uint64_t>(velocity) | (static_cast<uint64_t>(note) << 8) |
         (static_cast<uint64_t>(channel) << 16) | (static_cast<uint64_t>(group) << 24);
}

bool valid_midi_note_args(uint8_t group, uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  return group <= 15 && channel <= 15 && note <= 127 && velocity <= 127;
}

uint8_t infer_ump_word_count(const SonareEngineMidiEvent& event) noexcept {
  if (event.word_count >= 1 && event.word_count <= 4) return event.word_count;
  if (event.word3 != 0) return 4;
  if (event.word2 != 0) return 3;
  if (event.word1 != 0) return 2;
  return 1;
}

bool midi_event_from_c(const SonareEngineMidiEvent& src, midi::MidiEvent* out) noexcept {
  if (!out || src.group > 15 || src.reserved != 0) return false;
  midi::Ump ump{};
  ump.words[0] = src.word0;
  ump.words[1] = src.word1;
  ump.words[2] = src.word2;
  ump.words[3] = src.word3;
  ump.word_count = infer_ump_word_count(src);
  ump.group = src.group;
  ump.sysex_handle = src.sysex_handle;
  out->render_frame = src.render_frame;
  out->ump = ump;
  out->sysex_payload = nullptr;
  out->sysex_payload_size = 0;
  return true;
}

#endif

}  // namespace

SonareError sonare_engine_create(SonareRealtimeEngine** out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out = new SonareRealtimeEngine{};
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_engine_destroy(SonareRealtimeEngine* engine) { delete engine; }

SonareError sonare_engine_prepare(SonareRealtimeEngine* engine, double sample_rate,
                                  int max_block_size, size_t command_capacity,
                                  size_t telemetry_capacity) {
  if (!engine || sample_rate <= 0.0 || max_block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_play(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_stop(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_sample(SonareRealtimeEngine* engine, int64_t timeline_sample,
                                      int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_seek_ppq(SonareRealtimeEngine* engine, double ppq, int64_t render_frame) {
  if (!engine || !std::isfinite(ppq) || ppq < 0.0) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  command.arg.d = ppq;  // full double precision; engine applies without truncation
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_settle_parameters(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.settle_parameters();
  return SONARE_OK;
}

SonareError sonare_engine_set_tempo(SonareRealtimeEngine* engine, double bpm) {
  if (!engine || !std::isfinite(bpm) || bpm <= 0.0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_tempo(bpm);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_set_time_signature(SonareRealtimeEngine* engine, int numerator,
                                             int denominator) {
  if (!engine || numerator <= 0 || denominator <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->engine.set_time_signature(numerator, denominator);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_sample_at_ppq(SonareRealtimeEngine* engine, double ppq,
                                        int64_t* out_sample) {
  if (!engine || !out_sample || !std::isfinite(ppq) || ppq < 0.0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_sample = engine->engine.sample_at_ppq(ppq);
  return SONARE_OK;
}

SonareError sonare_engine_set_loop(SonareRealtimeEngine* engine, double start_ppq, double end_ppq,
                                   int enabled) {
  if (!engine || !std::isfinite(start_ppq) || !std::isfinite(end_ppq) || start_ppq < 0.0 ||
      end_ppq < 0.0 || (enabled && end_ppq <= start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_loop(start_ppq, end_ppq, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_add_parameter(SonareRealtimeEngine* engine,
                                        const SonareParameterInfo* info) {
  if (!engine || !info || info->max_value < info->min_value) return SONARE_ERROR_INVALID_PARAMETER;
  if (sonare::engine::RealtimeEngine::parameter_target_reserved(info->id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Stage the name/unit strings in the deque. std::deque keeps pointers to its
  // existing elements valid across push_back/pop_back at the ends, so the raw
  // const char* the registry holds for previously-added parameters are not
  // invalidated, and we can pop_back our two stagings if the registry rejects
  // the entry (duplicate id) — avoiding the previous unbounded leak on re-add.
  engine->parameter_strings.push_back(fixed_text(info->name, sizeof(info->name)));
  const char* name = engine->parameter_strings.back().c_str();
  engine->parameter_strings.push_back(fixed_text(info->unit, sizeof(info->unit)));
  const char* unit = engine->parameter_strings.back().c_str();
  const bool added = engine->parameters.add({info->id, name, unit, info->min_value, info->max_value,
                                             info->default_value, info->rt_safe != 0,
                                             curve_from_int(info->default_curve)});
  if (!added) {
    // Reclaim the two strings we just staged; the registry kept no reference to
    // them. The order matters: pop the most recent (unit) first.
    engine->parameter_strings.pop_back();
    engine->parameter_strings.pop_back();
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  publish_parameter_metadata(engine);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clear_parameters(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  // Clear the registry first (it holds raw pointers into parameter_strings),
  // then release the backing strings so no dangling pointer ever exists.
  engine->parameters.clear();
  engine->parameter_strings.clear();
  publish_parameter_metadata(engine);
  return SONARE_OK;
}

SonareError sonare_engine_parameter_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->parameters.parameter_count();
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info_by_index(SonareRealtimeEngine* engine, size_t index,
                                                  SonareParameterInfo* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  automation::ParameterInfo info{};
  if (!engine->parameters.parameter_info_by_index(index, &info)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_parameter(info, out);
  return SONARE_OK;
}

SonareError sonare_engine_parameter_info(SonareRealtimeEngine* engine, uint32_t id,
                                         SonareParameterInfo* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  automation::ParameterInfo info{};
  if (!engine->parameters.parameter_info(id, &info)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_parameter(info, out);
  return SONARE_OK;
}

SonareError sonare_engine_set_automation_lane(SonareRealtimeEngine* engine, uint32_t param_id,
                                              const SonareAutomationPoint* points,
                                              size_t point_count) {
  if (!engine || (point_count > 0 && !points)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<automation::Breakpoint> breakpoints;
  breakpoints.reserve(point_count);
  for (size_t i = 0; i < point_count; ++i) {
    // Reject non-finite breakpoints (both axes): a NaN/Inf ppq or value would
    // poison the lane's interpolation and propagate through every parameter the
    // lane drives.
    if (!std::isfinite(points[i].ppq) || !std::isfinite(points[i].value)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    breakpoints.push_back(
        {points[i].ppq, points[i].value, curve_from_int(points[i].curve_to_next)});
  }

  automation::AutomationLane lane(param_id);
  lane.set_points(std::move(breakpoints));
  auto found = std::find_if(
      engine->automation_lanes.begin(), engine->automation_lanes.end(),
      [&](const automation::AutomationLane& item) { return item.target_param_id() == param_id; });
  if (found == engine->automation_lanes.end()) {
    engine->automation_lanes.push_back(std::move(lane));
  } else {
    *found = std::move(lane);
  }
  engine->engine.automation().set_lanes(engine->automation_lanes);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_automation_lane_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.automation().lane_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_markers(SonareRealtimeEngine* engine,
                                      const SonareEngineMarker* markers, size_t marker_count) {
  if (!engine || (marker_count > 0 && !markers)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  engine->marker_strings.clear();
  std::vector<transport::Marker> prepared;
  prepared.reserve(marker_count);
  for (size_t i = 0; i < marker_count; ++i) {
    if (!std::isfinite(markers[i].ppq) || markers[i].ppq < 0.0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    engine->marker_strings.push_back(fixed_text(markers[i].name, sizeof(markers[i].name)));
    transport::Marker prepared_marker;
    prepared_marker.ppq = markers[i].ppq;
    prepared_marker.id = markers[i].id;
    prepared_marker.name = engine->marker_strings.back().c_str();
    prepared_marker.kind = markers[i].kind;
    prepared_marker.key_fifths = markers[i].key_fifths;
    prepared_marker.key_minor = markers[i].key_minor != 0;
    prepared.push_back(prepared_marker);
  }
  engine->engine.set_markers(std::move(prepared));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_marker_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.marker_count();
  return SONARE_OK;
}

SonareError sonare_engine_marker_by_index(SonareRealtimeEngine* engine, size_t index,
                                          SonareEngineMarker* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  transport::Marker marker{};
  if (!engine->engine.marker_by_index(index, &marker)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
}

SonareError sonare_engine_marker(SonareRealtimeEngine* engine, uint32_t id,
                                 SonareEngineMarker* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  transport::Marker marker{};
  if (!engine->engine.marker_by_id(id, &marker)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  fill_c_marker(marker, out);
  return SONARE_OK;
}

SonareError sonare_engine_seek_marker(SonareRealtimeEngine* engine, uint32_t marker_id,
                                      int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kSeekMarker;
  command.target_id = marker_id;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_loop_from_markers(SonareRealtimeEngine* engine,
                                                uint32_t start_marker_id, uint32_t end_marker_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  return engine->engine.set_loop_from_markers(start_marker_id, end_marker_id)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
}

SonareError sonare_engine_set_metronome(SonareRealtimeEngine* engine,
                                        const SonareEngineMetronomeConfig* config) {
  // click_samples == 0 is the documented "use the sample-rate-derived default"
  // sentinel (the engine derives it from click_seconds and the sample rate).
  // Only a negative explicit length is invalid.
  if (!engine || !config || config->beat_gain < 0.0f || config->accent_gain < 0.0f ||
      config->click_samples < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_metronome_config(metronome_from_c(*config));
  return SONARE_OK;
}

SonareError sonare_engine_metronome(SonareRealtimeEngine* engine,
                                    SonareEngineMetronomeConfig* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = metronome_to_c(engine->engine.metronome_config());
  return SONARE_OK;
}

SonareError sonare_engine_count_in_end_sample(SonareRealtimeEngine* engine, int64_t start_sample,
                                              int bars, int64_t* out_sample) {
  if (!engine || !out_sample || start_sample < 0 || bars <= 0)
    return SONARE_ERROR_INVALID_PARAMETER;
  *out_sample = engine->engine.count_in_end_sample(start_sample, bars);
  return SONARE_OK;
}

SonareError sonare_engine_set_clips(SonareRealtimeEngine* engine, const SonareEngineClip* clips,
                                    size_t clip_count) {
  if (!engine || (clip_count > 0 && !clips)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<std::shared_ptr<engine::ClipAudioStorage>> clip_storage;
  clip_storage.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    const bool paged = clip.page_provider != nullptr;
    const int source_channels =
        paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
    const int64_t source_samples =
        paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
    const int64_t effective_length =
        clip.length_samples > 0 ? clip.length_samples : source_samples - clip.clip_offset_samples;
    if ((!paged && !clip.channels) || (paged && !clip.page_provider->provider) ||
        source_channels <= 0 || source_samples <= 0 || !std::isfinite(clip.start_ppq) ||
        clip.start_ppq < 0.0 || clip.clip_offset_samples < 0 ||
        clip.clip_offset_samples >= source_samples || effective_length <= 0 ||
        !(std::isfinite(clip.gain) && clip.gain >= 0.0f) || clip.fade_in_samples < 0 ||
        clip.fade_out_samples < 0 ||
        (clip.warp_mode != SONARE_ENGINE_WARP_MODE_OFF &&
         clip.warp_mode != SONARE_ENGINE_WARP_MODE_REPITCH &&
         clip.warp_mode != SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) ||
        (paged && clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) ||
        (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC && clip.loop != 0) ||
        (clip.warp_mode == SONARE_ENGINE_WARP_MODE_REPITCH && clip.loop != 0 &&
         clip.warp_anchor_count >= 2) ||
        (clip.warp_anchor_count > 0 && !clip.warp_anchors)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    for (size_t anchor_index = 0; anchor_index < clip.warp_anchor_count; ++anchor_index) {
      const SonareEngineWarpAnchor& anchor = clip.warp_anchors[anchor_index];
      if (!std::isfinite(anchor.warp_sample) || !std::isfinite(anchor.source_sample) ||
          anchor.warp_sample < 0.0 || anchor.source_sample < 0.0) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (anchor_index > 0) {
        const SonareEngineWarpAnchor& prev = clip.warp_anchors[anchor_index - 1];
        if (!(anchor.warp_sample > prev.warp_sample && anchor.source_sample > prev.source_sample)) {
          return SONARE_ERROR_INVALID_PARAMETER;
        }
      }
    }
    auto owned = std::make_shared<engine::ClipAudioStorage>();
    owned->channels.reserve(static_cast<size_t>(source_channels));
    owned->channel_ptrs.reserve(static_cast<size_t>(source_channels));
    if (paged) {
      // Paged providers are retained by shared_ptr on the ClipSchedule; no audio
      // is copied into ClipAudioStorage.
    } else if (clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) {
      std::vector<engine::TempoSyncWarpSegment> segments;
      if (!tempo_sync_segments_for_clip(clip, &segments)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      engine::TempoSyncWarpBakeConfig bake_config;
      bake_config.sample_rate = static_cast<int>(std::lround(engine->engine.sample_rate()));
      std::vector<const float*> source_channel_ptrs;
      source_channel_ptrs.reserve(static_cast<size_t>(clip.num_channels));
      for (int ch = 0; ch < clip.num_channels; ++ch) {
        if (!clip.channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
        source_channel_ptrs.push_back(clip.channels[ch]);
      }
      owned->channels = engine::bake_tempo_sync_warp_channels(
          source_channel_ptrs, static_cast<size_t>(clip.num_samples), segments, bake_config);
    } else {
      for (int ch = 0; ch < clip.num_channels; ++ch) {
        if (!clip.channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
        owned->channels.emplace_back(clip.channels[ch], clip.channels[ch] + clip.num_samples);
      }
    }
    clip_storage.push_back(std::move(owned));
  }

  std::vector<engine::ClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineClip& clip = clips[i];
    const bool paged = clip.page_provider != nullptr;
    const int source_channels =
        paged ? clip.page_provider->provider->num_channels() : clip.num_channels;
    const int64_t source_samples =
        paged ? clip.page_provider->provider->num_samples() : clip.num_samples;
    const int64_t effective_length =
        clip.length_samples > 0 ? clip.length_samples : source_samples - clip.clip_offset_samples;
    auto& owned = clip_storage[i];
    owned->channel_ptrs.clear();
    for (const auto& channel : owned->channels) {
      owned->channel_ptrs.push_back(channel.data());
    }
    engine::ClipSchedule schedule{};
    schedule.id = clip.id;
    schedule.track_id = clip.track_id;
    schedule.buffer =
        paged ? engine::ClipAudioBuffer{}
              : engine::ClipAudioBuffer{
                    owned->channel_ptrs.data(), source_channels,
                    static_cast<int64_t>(owned->channels.empty() ? 0 : owned->channels[0].size())};
    schedule.storage = owned;
    if (paged) schedule.page_provider = clip.page_provider->provider;
    schedule.start_ppq = clip.start_ppq;
    schedule.clip_offset_samples =
        clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC ? 0 : clip.clip_offset_samples;
    schedule.length_samples = effective_length;
    schedule.loop = clip.warp_mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC ? false : clip.loop != 0;
    schedule.gain = clip.gain;
    schedule.fade_in_samples = clip.fade_in_samples;
    schedule.fade_out_samples = clip.fade_out_samples;
    schedule.warp_mode = clip.warp_mode == SONARE_ENGINE_WARP_MODE_REPITCH
                             ? engine::WarpMode::kRepitch
                             : engine::WarpMode::kOff;
    if (schedule.warp_mode == engine::WarpMode::kRepitch && clip.warp_anchor_count >= 2) {
      auto anchors = std::make_shared<std::vector<engine::WarpAnchor>>();
      anchors->reserve(clip.warp_anchor_count);
      for (size_t anchor_index = 0; anchor_index < clip.warp_anchor_count; ++anchor_index) {
        anchors->push_back({clip.warp_anchors[anchor_index].warp_sample,
                            clip.warp_anchors[anchor_index].source_sample});
      }
      schedule.warp_anchors = std::move(anchors);
    }
    schedules.push_back(schedule);
  }
  engine->engine.set_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_clip_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = engine->engine.clip_count();
  return SONARE_OK;
}

SonareError sonare_engine_set_track_lanes(SonareRealtimeEngine* engine,
                                          const SonareEngineTrackLane* lanes, size_t lane_count) {
  if (!engine || (lane_count > 0 && !lanes)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)lanes;
  (void)lane_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  std::vector<engine::TrackLaneConfig> configs;
  SONARE_C_TRY
  configs.reserve(lane_count);
  for (size_t i = 0; i < lane_count; ++i) {
    if (lanes[i].send_count > 0 && lanes[i].sends == nullptr) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!is_valid_channel_layout(lanes[i].source_channel_layout)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    engine::TrackLaneConfig lane{lanes[i].track_id};
    lane.output_bus_id = lanes[i].output_bus_id;
    lane.source_layout = static_cast<ChannelLayout>(lanes[i].source_channel_layout);
    lane.sends.reserve(lanes[i].send_count);
    for (size_t send_index = 0; send_index < lanes[i].send_count; ++send_index) {
      const SonareEngineTrackSend& send = lanes[i].sends[send_index];
      lane.sends.push_back({send.bus_id, send.level_db, send.enabled != 0,
                            sonare_c_mixing_detail::to_send_timing(send.send_timing)});
    }
    configs.push_back(std::move(lane));
  }
  return engine->engine.set_track_lanes(std::move(configs)) ? SONARE_OK
                                                            : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_lane_sidechain(SonareRealtimeEngine* engine, uint32_t track_id,
                                             unsigned int insert_index, uint32_t source_track_id) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)source_track_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.set_lane_sidechain(track_id, insert_index, source_track_id)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
#endif
}

SonareError sonare_engine_set_track_buses(SonareRealtimeEngine* engine,
                                          const SonareEngineBus* buses, size_t bus_count) {
  if (!engine || (bus_count > 0 && !buses)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)buses;
  (void)bus_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  std::vector<engine::TrackBusConfig> configs;
  SONARE_C_TRY
  configs.reserve(bus_count);
  for (size_t i = 0; i < bus_count; ++i) {
    if (!is_valid_channel_layout(buses[i].channel_layout)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    configs.push_back(
        {buses[i].bus_id, buses[i].gain_db, static_cast<ChannelLayout>(buses[i].channel_layout)});
  }
  return engine->engine.set_track_buses(std::move(configs)) ? SONARE_OK
                                                            : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_json(SonareRealtimeEngine* engine, uint32_t bus_id,
                                             const char* scene_json) {
  if (!engine || bus_id == 0 || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  try {
    scene = mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (scene.buses.empty()) return SONARE_ERROR_INVALID_PARAMETER;
  return engine->engine.set_bus_strip(bus_id, scene.buses.front()) ? SONARE_OK
                                                                   : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_json(SonareRealtimeEngine* engine, uint32_t track_id,
                                               const char* scene_json) {
  if (!engine || track_id == 0 || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  try {
    scene = mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (scene.strips.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->engine.set_track_strip(track_id, scene.strips.front())
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                       uint32_t track_id, int band_index,
                                                       const char* band_json) {
  if (!engine || track_id == 0 || band_index < 0 || !band_json) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)band_index;
  (void)band_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_eq_band(track_id, static_cast<size_t>(band_index),
                                          sonare::c_api::parse_eq_band_json(band_json))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                          uint32_t track_id,
                                                          unsigned int insert_index, int bypassed,
                                                          int reset_on_bypass) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_insert_bypassed(track_id, insert_index, bypassed != 0,
                                                  reset_on_bypass != 0)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_json(SonareRealtimeEngine* engine,
                                                const char* scene_json) {
  if (!engine || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  try {
    scene = mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  if (scene.strips.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->engine.set_master_strip(scene.strips.front()) ? SONARE_OK
                                                               : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                        int band_index, const char* band_json) {
  if (!engine || band_index < 0 || !band_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)band_index;
  (void)band_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_eq_band(static_cast<size_t>(band_index),
                                           sonare::c_api::parse_eq_band_json(band_json))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                           unsigned int insert_index, int bypassed,
                                                           int reset_on_bypass) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_insert_bypassed(insert_index, bypassed != 0,
                                                   reset_on_bypass != 0)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                               uint32_t track_id,
                                                               unsigned int insert_index,
                                                               const char* param_name,
                                                               float value) {
  if (!engine || track_id == 0 || !param_name || param_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)insert_index;
  (void)value;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_insert_param(track_id, insert_index, param_name, value)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                                unsigned int insert_index,
                                                                const char* param_name,
                                                                float value) {
  if (!engine || !param_name || param_name[0] == '\0') return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)value;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_insert_param(insert_index, param_name, value)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                              float pan) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan(track_id, pan) ? SONARE_OK : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_law(SonareRealtimeEngine* engine, uint32_t track_id,
                                                  int pan_law) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan_law;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan_law(track_id, sonare_c_mixing_detail::to_pan_law(pan_law))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_mode(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   int pan_mode) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan_mode;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan_mode(track_id, sonare_c_mixing_detail::to_pan_mode(pan_mode))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_dual_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   float left_pan, float right_pan) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_dual_pan(track_id, left_pan, right_pan)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_channel_delay_samples(SonareRealtimeEngine* engine,
                                                                uint32_t track_id,
                                                                int delay_samples) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)delay_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_channel_delay_samples(track_id, delay_samples)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_clip_page_provider_create(int num_channels, int64_t num_samples,
                                             int64_t page_frames,
                                             SonareClipPageProvider** out_provider) {
  if (!out_provider || num_channels <= 0 || num_samples <= 0 || page_frames <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto handle = std::make_unique<SonareClipPageProvider>();
  handle->provider = std::make_shared<CClipPageProvider>(num_channels, num_samples, page_frames);
  *out_provider = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_clip_page_provider_destroy(SonareClipPageProvider* provider) { delete provider; }

SonareError sonare_clip_page_provider_supply(SonareClipPageProvider* provider, int64_t page_index,
                                             const float* const* channels, int num_channels,
                                             int64_t frames) {
  SONARE_C_TRY
  if (!provider || !provider->provider ||
      !provider->provider->supply(page_index, channels, num_channels, frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_clip_page_provider_clear(SonareClipPageProvider* provider, int64_t page_index) {
  SONARE_C_TRY
  if (!provider || !provider->provider || !provider->provider->clear(page_index)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_pop_clip_page_request(SonareRealtimeEngine* engine,
                                                SonareClipPageRequest* out_request,
                                                int* out_has_request) {
  if (!engine || !out_request || !out_has_request) return SONARE_ERROR_INVALID_PARAMETER;
  engine::ClipPageRequest request{};
  if (engine->engine.pop_clip_page_request(request)) {
    out_request->clip_id = request.clip_id;
    out_request->channel = request.channel;
    out_request->sample = request.sample;
    *out_has_request = 1;
  } else {
    *out_request = {};
    *out_has_request = 0;
  }
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer) {
  if (!engine || !buffer || !buffer->channels || buffer->num_channels <= 0 ||
      buffer->capacity_frames <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (int ch = 0; ch < buffer->num_channels; ++ch) {
    if (!buffer->channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_segment(
      {buffer->channels, buffer->num_channels, buffer->capacity_frames});
  return SONARE_OK;
}

SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_capture_armed(armed != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled) {
  if (!engine || start_sample < 0 || end_sample < start_sample) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_punch(start_sample, end_sample, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_source(SonareRealtimeEngine* engine,
                                             SonareEngineCaptureSource source) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  switch (source) {
    case SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kOutput);
      return SONARE_OK;
    case SONARE_ENGINE_CAPTURE_SOURCE_INPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kInput);
      return SONARE_OK;
    default:
      return SONARE_ERROR_INVALID_PARAMETER;
  }
}

SonareError sonare_engine_set_record_offset_samples(SonareRealtimeEngine* engine,
                                                    int64_t offset_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_record_offset_samples(offset_samples);
  return SONARE_OK;
}

SonareError sonare_engine_set_input_monitor(SonareRealtimeEngine* engine, int enabled, float gain) {
  if (!engine || !std::isfinite(gain)) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_input_monitor(enabled != 0, gain);
  return SONARE_OK;
}

SonareError sonare_engine_reset_capture(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.reset_capture();
  return SONARE_OK;
}

SonareError sonare_engine_capture_status(SonareRealtimeEngine* engine,
                                         SonareEngineCaptureStatus* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  out->captured_frames = engine->engine.captured_frames();
  out->overflow_count = engine->engine.capture_overflow_count();
  out->armed = engine->engine.capture_armed() ? 1 : 0;
  out->punch_enabled = engine->engine.capture_punch_enabled() ? 1 : 0;
  out->source = engine->engine.capture_source() == engine::CaptureSource::kInput
                    ? SONARE_ENGINE_CAPTURE_SOURCE_INPUT
                    : SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
  out->record_offset_samples = engine->engine.record_offset_samples();
  return SONARE_OK;
}

SonareError sonare_engine_set_graph(SonareRealtimeEngine* engine,
                                    const SonareEngineGraphSpec* spec) {
  if (!engine || !spec || !spec->nodes || spec->node_count == 0 || spec->num_channels <= 0 ||
      (spec->connection_count > 0 && !spec->connections) ||
      (spec->parameter_binding_count > 0 && !spec->parameter_bindings)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  SONARE_C_TRY
  auto graph = std::make_unique<graph::Graph>();
  for (size_t i = 0; i < spec->node_count; ++i) {
    const SonareEngineGraphNode& node = spec->nodes[i];
    const int ports = node.num_ports > 0 ? node.num_ports : spec->num_channels;
    auto processor = make_graph_processor(node);
    if (!processor ||
        !graph->add_node(fixed_text(node.id, sizeof(node.id)), std::move(processor), ports)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  for (size_t i = 0; i < spec->connection_count; ++i) {
    const SonareEngineGraphConnection& connection = spec->connections[i];
    graph::Connection graph_connection{};
    graph_connection.source_node =
        fixed_text(connection.source_node, sizeof(connection.source_node));
    graph_connection.source_port = connection.source_port;
    graph_connection.dest_node = fixed_text(connection.dest_node, sizeof(connection.dest_node));
    graph_connection.dest_port = connection.dest_port;
    graph_connection.mix =
        connection.mix == 0 ? graph::Connection::Mix::Replace : graph::Connection::Mix::Add;
    if (!graph->connect(std::move(graph_connection))) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  if (!graph->compile()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto state = engine->engine.transport().snapshot_control();
  graph->prepare(state.sample_rate, engine->engine.max_block_size());
  const std::string input_node = fixed_text(spec->input_node, sizeof(spec->input_node));
  const std::string output_node = fixed_text(spec->output_node, sizeof(spec->output_node));
  if (!engine->engine.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(),
                                 spec->num_channels)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < spec->parameter_binding_count; ++i) {
    const SonareEngineGraphParameterBinding& binding = spec->parameter_bindings[i];
    const std::string node_id = fixed_text(binding.node_id, sizeof(binding.node_id));
    if (!engine->engine.bind_graph_parameter(binding.param_id, node_id.c_str())) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_node_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_node_count();
#else
  *out_count = 0;
#endif
  return SONARE_OK;
}

SonareError sonare_engine_graph_connection_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_connection_count();
#else
  *out_count = 0;
#endif
  return SONARE_OK;
}

SonareError sonare_engine_process(SonareRealtimeEngine* engine, float* const* channels,
                                  int num_channels, int num_frames) {
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process(channels, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_process_with_monitor(SonareRealtimeEngine* engine, float* const* channels,
                                               float* const* monitor_out, int num_channels,
                                               int num_frames) {
  if (!engine || num_channels < 0 || num_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.process_with_monitor(channels, monitor_out, num_channels, num_frames);
  return SONARE_OK;
}

SonareError sonare_engine_render_offline(SonareRealtimeEngine* engine, float* const* out,
                                         int num_channels, int64_t total_frames, int block_size) {
  if (!engine || !out || num_channels <= 0 || total_frames < 0 || block_size <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  engine->engine.render_offline(out, num_channels, total_frames, block_size);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_bounce_options_default(SonareEngineBounceOptions* options) {
  if (!options) return SONARE_ERROR_INVALID_PARAMETER;
  *options = SonareEngineBounceOptions{};
  options->block_size = 128;
  options->num_channels = 2;
  options->target_sample_rate = 48000;
  options->source_sample_rate = 48000;
  options->normalize_lufs = 0;
  options->target_lufs = SONARE_DEFAULT_BOUNCE_TARGET_LUFS;
  options->dither = 0;
  options->dither_bits = 16;
  options->dither_seed = 0;
  return SONARE_OK;
}

SonareError sonare_engine_bounce_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineBounceOptions* options,
                                         SonareEngineBounceResult* out) {
  // Zero the owned out-pointer/lengths BEFORE any validation early-return so a
  // failed validation always leaves a NULL owned pointer (matching the analysis
  // wrappers in sonare_c.cpp). Otherwise the standard
  // sonare_free_bounce_result(&r) idiom would delete[] an uninitialised pointer.
  if (out) {
    *out = {};
  }
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || options->target_sample_rate <= 0 ||
      options->source_sample_rate <= 0 || options->dither_bits < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // The bounce width must map to a supported speaker layout (1 mono, 2 stereo,
  // 6 = 5.1, 8 = 7.1). Counts like 3/4/5/7 have no layout and would silently
  // leave their extra planes unpanned, so reject them up front.
  if (sonare::channel_count(sonare::layout_from_channel_count(options->num_channels)) !=
      options->num_channels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<std::vector<float>> channels(
      static_cast<size_t>(options->num_channels),
      std::vector<float>(static_cast<size_t>(options->total_frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels.size());
  for (auto& channel : channels) {
    ptrs.push_back(channel.data());
  }
  engine->engine.render_offline(ptrs.data(), options->num_channels, options->total_frames,
                                options->block_size);

  channels = resample_channels(channels, options->source_sample_rate, options->target_sample_rate);
  std::vector<float> interleaved = interleave_channels(channels);
  if (options->normalize_lufs) {
    // target_lufs == 0.0f is the documented "use default" sentinel; promote it
    // to the canonical SONARE_DEFAULT_BOUNCE_TARGET_LUFS so a zero-initialised
    // SonareEngineBounceOptions normalises to the same target regardless of
    // which binding (C, Node, Python, WASM) populated the struct. The
    // static_assert below pins the macro to the value the WASM wrapper used
    // to hardcode at the embind layer (see src/wasm/bindings.cpp::bounceOffline).
    static_assert(SONARE_DEFAULT_BOUNCE_TARGET_LUFS == -14.0f,
                  "SONARE_DEFAULT_BOUNCE_TARGET_LUFS must match the WASM/Node "
                  "facade default to keep cross-binding bounce behaviour identical");
    // Non-finite targets (NaN/Inf) also fall back to the default so a
    // garbage float can never propagate into the normalisation gain. Note the
    // 0.0f sentinel makes an exact 0 LUFS target unrepresentable; that is an
    // accepted trade-off documented on SonareEngineBounceOptions::target_lufs.
    const float effective_target_lufs =
        (options->target_lufs == 0.0f || !std::isfinite(options->target_lufs))
            ? SONARE_DEFAULT_BOUNCE_TARGET_LUFS
            : options->target_lufs;
    metering::normalize_interleaved_to_lufs(interleaved, channels[0].size(), options->num_channels,
                                            options->target_sample_rate, effective_target_lufs);
  }
  if (options->dither != 0) {
#if defined(SONARE_WITH_MASTERING)
    mastering::final::DitherConfig config{};
    config.type = dither_type_from_int(options->dither);
    config.target_bits = options->dither_bits > 0 ? options->dither_bits : 16;
    config.seed = options->dither_seed == 0 ? config.seed : options->dither_seed;
    Audio dithered = mastering::final::dither(
        Audio::from_buffer(interleaved.data(), interleaved.size(), options->target_sample_rate),
        config);
    interleaved.assign(dithered.data(), dithered.data() + dithered.size());
#else
    return SONARE_ERROR_NOT_SUPPORTED;
#endif
  }
  const auto loudness = metering::lufs_interleaved(
      interleaved.data(), channels[0].size(), options->num_channels, options->target_sample_rate);
  out->sample_count = interleaved.size();
  out->frames = static_cast<int64_t>(channels[0].size());
  out->num_channels = options->num_channels;
  out->sample_rate = options->target_sample_rate;
  out->integrated_lufs = loudness.integrated_lufs;
  out->interleaved = new float[interleaved.size()];
  std::memcpy(out->interleaved, interleaved.data(), interleaved.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_freeze_offline(SonareRealtimeEngine* engine,
                                         const SonareEngineFreezeOptions* options,
                                         SonareEngineFreezeResult* out) {
  if (!engine || !options || !out || options->total_frames <= 0 || options->block_size <= 0 ||
      options->num_channels <= 0 || !std::isfinite(options->start_ppq) ||
      options->start_ppq < 0.0 || !(std::isfinite(options->gain) && options->gain >= 0.0f)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  *out = {};
  auto owned = std::make_shared<engine::ClipAudioStorage>();
  owned->channels.assign(static_cast<size_t>(options->num_channels),
                         std::vector<float>(static_cast<size_t>(options->total_frames), 0.0f));
  std::vector<float*> render_ptrs;
  render_ptrs.reserve(owned->channels.size());
  for (auto& channel : owned->channels) {
    render_ptrs.push_back(channel.data());
  }
  engine->engine.render_offline(render_ptrs.data(), options->num_channels, options->total_frames,
                                options->block_size);

  owned->channel_ptrs.clear();
  owned->channel_ptrs.reserve(owned->channels.size());
  for (const auto& channel : owned->channels) {
    owned->channel_ptrs.push_back(channel.data());
  }
  engine::ClipSchedule schedule{};
  schedule.id = options->clip_id == 0 ? 1 : options->clip_id;
  schedule.buffer = {owned->channel_ptrs.data(), options->num_channels, options->total_frames};
  schedule.storage = owned;
  schedule.start_ppq = options->start_ppq;
  schedule.clip_offset_samples = 0;
  schedule.length_samples = options->total_frames;
  schedule.loop = false;
  schedule.gain = options->gain;
  schedule.fade_in_samples = 0;
  schedule.fade_out_samples = 0;

  engine->engine.set_clips({schedule});
  out->clip_id = schedule.id;
  out->frames = options->total_frames;
  out->num_channels = options->num_channels;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_drain_telemetry(SonareRealtimeEngine* engine, SonareEngineTelemetry* out,
                                          size_t max_records, size_t* written) {
  if (!engine || !written || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  size_t count = 0;
  engine::Telemetry telemetry{};
  while (count < max_records && engine->engine.pop_telemetry(telemetry)) {
    out[count] = {static_cast<int>(telemetry.type),
                  static_cast<int>(telemetry.error),
                  telemetry.render_frame,
                  telemetry.timeline_sample,
                  telemetry.audible_timeline_sample,
                  telemetry.graph_latency_samples_q8,
                  telemetry.value};
    ++count;
  }
  *written = count;
  return SONARE_OK;
}

SonareError sonare_engine_drain_meter_telemetry(SonareRealtimeEngine* engine,
                                                SonareMeterTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::MeterTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_meter_telemetry(record)) {
    out[count].target_id = record.target_id;
    out[count].render_frame = record.render_frame;
    out[count].seq = record.seq;
    out[count].peak_db_l = record.peak_db[0];
    out[count].peak_db_r = record.peak_db[1];
    out[count].rms_db_l = record.rms_db[0];
    out[count].rms_db_r = record.rms_db[1];
    out[count].true_peak_db_l = record.true_peak_db[0];
    out[count].true_peak_db_r = record.true_peak_db[1];
    out[count].max_true_peak_db = record.max_true_peak_db;
    out[count].correlation = record.correlation;
    out[count].mono_compat_width = record.mono_compat_width;
    out[count].momentary_lufs = record.momentary_lufs;
    out[count].short_term_lufs = record.short_term_lufs;
    out[count].integrated_lufs = record.integrated_lufs;
    out[count].gain_reduction_db = record.gain_reduction_db;
    out[count].dropped_records = record.dropped_records;
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_configure_scope_telemetry(SonareRealtimeEngine* engine,
                                                    int interval_frames, unsigned int band_count,
                                                    unsigned int* out_band_count) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_MIXING)
  const uint32_t applied =
      engine->engine.configure_scope_telemetry(interval_frames, static_cast<uint32_t>(band_count));
  if (out_band_count) *out_band_count = applied;
  return SONARE_OK;
#else
  (void)interval_frames;
  (void)band_count;
  if (out_band_count) *out_band_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_drain_scope_telemetry(SonareRealtimeEngine* engine,
                                                SonareScopeTelemetryRecord* out, size_t max_records,
                                                size_t* out_count) {
  if (!engine || !out_count || (max_records > 0 && !out)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

#if defined(SONARE_WITH_MIXING)
  size_t count = 0;
  engine::ScopeTelemetryRecord record{};
  while (count < max_records && engine->engine.pop_scope_telemetry(record)) {
    out[count].target_id = record.target_id;
    out[count].render_frame = record.render_frame;
    out[count].seq = record.seq;
    out[count].dropped_records = record.dropped_records;
    out[count].band_count = record.band_count;
    for (uint32_t b = 0; b < record.band_count && b < SONARE_SCOPE_MAX_BANDS; ++b) {
      out[count].bands[b] = record.bands[b];
    }
    out[count].point_count = record.point_count;
    for (uint32_t p = 0; p < record.point_count && p < SONARE_SCOPE_MAX_POINTS; ++p) {
      out[count].points[2 * p] = record.points[p].left;
      out[count].points[2 * p + 1] = record.points[p].right;
    }
    ++count;
  }
  *out_count = count;
  return SONARE_OK;
#else
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_set_parameter(SonareRealtimeEngine* engine, uint32_t param_id,
                                        float value, int64_t render_frame) {
  if (!engine || !std::isfinite(value)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParam;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_parameter_smoothed(SonareRealtimeEngine* engine, uint32_t param_id,
                                                 float value, int64_t render_frame) {
  if (!engine || !std::isfinite(value)) return SONARE_ERROR_INVALID_PARAMETER;
  if (registered_parameter_rejects_realtime(engine, param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  rt::Command command{};
  command.type = rt::CommandType::kSetParamSmoothed;
  command.target_id = param_id;
  command.sample_time = render_frame;
  command.arg.f = value;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError sonare_engine_set_solo_mute(SonareRealtimeEngine* engine, uint32_t lane_index, int solo,
                                        int mute, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)lane_index;
  (void)solo;
  (void)mute;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kSetSoloMute;
  command.target_id = lane_index;
  command.sample_time = render_frame;
  command.arg.i = (mute ? 0x1 : 0x0) | (solo ? 0x2 : 0x0);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_set_midi_clips(SonareRealtimeEngine* engine,
                                         const SonareEngineMidiClipSchedule* clips,
                                         size_t clip_count) {
  if (!engine || (clip_count > 0 && clips == nullptr)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)clips;
  (void)clip_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  std::vector<midi::MidiClipSchedule> schedules;
  schedules.reserve(clip_count);
  for (size_t i = 0; i < clip_count; ++i) {
    const SonareEngineMidiClipSchedule& src = clips[i];
    if (!std::isfinite(src.start_ppq) || src.loop < 0 ||
        (src.event_count > 0 && src.events == nullptr)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    midi::MidiClipSchedule dst;
    dst.id = src.id;
    dst.track_id = src.track_id;
    dst.start_sample = src.start_sample;
    dst.start_ppq = src.start_ppq;
    dst.length_samples = src.length_samples;
    dst.loop_mode = src.loop ? midi::MidiLoopMode::kLoop : midi::MidiLoopMode::kOneShot;
    dst.loop_length_samples = src.loop_length_samples;
    dst.destination_id = src.destination_id;
    dst.events.reserve(src.event_count);
    for (size_t j = 0; j < src.event_count; ++j) {
      midi::MidiEvent event;
      if (!midi_event_from_c(src.events[j], &event)) return SONARE_ERROR_INVALID_PARAMETER;
      dst.events.push_back(event);
    }
    std::sort(dst.events.begin(), dst.events.end(),
              [](const midi::MidiEvent& a, const midi::MidiEvent& b) {
                return a.render_frame < b.render_frame;
              });
    schedules.push_back(std::move(dst));
  }
  engine->engine.set_midi_clips(std::move(schedules));
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_builtin_instrument(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id,
                                                 const SonareEngineBuiltinSynthConfig* config) {
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  auto synth = std::make_unique<sonare::midi::BuiltinSynth>(engine_synth_config_from_c(*config));
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_synth_instrument(SonareRealtimeEngine* engine,
                                               uint32_t destination_id,
                                               const SonareSynthPatch* patch) {
  if (!engine || !patch) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  if (!sonare_c_detail::synth_config_from_patch_c(*patch, &cfg, &error)) {
    set_last_error(error != nullptr ? error : "invalid synth patch");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto synth = std::make_unique<sonare::midi::synth::NativeSynth>(cfg);
  return bind_engine_instrument(engine, destination_id, std::move(synth));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_load_soundfont(SonareRealtimeEngine* engine, const uint8_t* data,
                                         size_t size) {
  if (!engine || !data || size == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  auto soundfont = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  engine->soundfont = std::move(soundfont);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_sf2_instrument(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             const SonareEngineSf2InstrumentConfig* config) {
  if (!engine || !config) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (config->struct_version > 1) return SONARE_ERROR_INVALID_PARAMETER;
  // A missing SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor, so live MIDI stays audible with zero data.
  SONARE_C_TRY
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (config->gain > 0.0f) cfg.gain = config->gain;
  if (config->polyphony > 0) cfg.polyphony = config->polyphony;
  auto player = std::make_unique<sonare::midi::synth::Sf2Player>(cfg);
  player->set_soundfont(engine->soundfont);
  return bind_engine_instrument(engine, destination_id, std::move(player));
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_instrument(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_instrument(destination_id, nullptr);
  engine->builtin_instruments.erase(
      std::remove_if(engine->builtin_instruments.begin(), engine->builtin_instruments.end(),
                     [&](const auto& entry) { return entry.first == destination_id; }),
      engine->builtin_instruments.end());
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_instrument_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_instrument_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_bind_midi_cc(SonareRealtimeEngine* engine, uint8_t channel,
                                       uint8_t controller, uint32_t param_id, float min_value,
                                       float max_value) {
  if (!engine || channel > 15 || controller > 127 || param_id == 0 || !std::isfinite(min_value) ||
      !std::isfinite(max_value) || max_value < min_value) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (sonare::engine::RealtimeEngine::parameter_target_reserved(param_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.bind_midi_cc(controller, channel, param_id, min_value, max_value)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_clear_midi_cc_bindings(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_cc_bindings();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_cc_binding_count(SonareRealtimeEngine* engine, size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->engine.midi_cc_binding_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id,
                                      const char* config_json) {
  if (!engine || !config_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  const SonareError parse_err = midi_fx_chain_from_json(config_json, &chain);
  if (parse_err != SONARE_OK) return parse_err;
  return engine->engine.set_midi_fx(destination_id, chain) ? SONARE_OK : SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_midi_fx(SonareRealtimeEngine* engine, uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.clear_midi_fx(destination_id);
  return SONARE_OK;
#endif
}

SonareError sonare_engine_set_midi_input_source(SonareRealtimeEngine* engine,
                                                uint32_t destination_id) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(&engine->midi_input_source, destination_id);
  engine->midi_input_source_enabled = true;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_clear_midi_input_source(SonareRealtimeEngine* engine) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine->engine.set_midi_input_source(nullptr, 0);
  engine->midi_input_source_enabled = false;
  return SONARE_OK;
#endif
}

SonareError sonare_engine_midi_input_pending_count(SonareRealtimeEngine* engine,
                                                   size_t* out_count) {
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_count = engine->midi_input_source.pending_count();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_push_midi_input_note_on(SonareRealtimeEngine* engine, uint8_t group,
                                                  uint8_t channel, uint8_t note, uint8_t velocity,
                                                  int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_on(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_note_off(SonareRealtimeEngine* engine, uint8_t group,
                                                   uint8_t channel, uint8_t note, uint8_t velocity,
                                                   int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || !valid_midi_note_args(group, channel, note, velocity)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_note_off(group, channel, note, velocity), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_input_cc(SonareRealtimeEngine* engine, uint8_t group,
                                             uint8_t channel, uint8_t controller, uint8_t value,
                                             int64_t port_time_samples) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)group;
  (void)channel;
  (void)controller;
  (void)value;
  (void)port_time_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!engine->midi_input_source_enabled || group > 15 || channel > 15 || controller > 127 ||
      value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->midi_input_source.push_event(
             midi::make_midi1_control_change(group, channel, controller, value), port_time_samples)
             ? SONARE_OK
             : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_on(SonareRealtimeEngine* engine, uint32_t destination_id,
                                            uint8_t group, uint8_t channel, uint8_t note,
                                            uint8_t velocity, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOnImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_note_off(SonareRealtimeEngine* engine, uint32_t destination_id,
                                             uint8_t group, uint8_t channel, uint8_t note,
                                             uint8_t velocity, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)group;
  (void)channel;
  (void)note;
  (void)velocity;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  if (!valid_midi_note_args(group, channel, note, velocity)) return SONARE_ERROR_INVALID_PARAMETER;
  rt::Command command{};
  command.type = rt::CommandType::kMidiNoteOffImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(pack_midi_note(group, channel, note, velocity));
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_cc(SonareRealtimeEngine* engine, uint32_t destination_id,
                                       uint8_t group, uint8_t channel, uint8_t controller,
                                       uint8_t value, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  if (group > 15 || channel > 15 || controller > 127 || value > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  // Pack the scalar MIDI fields into arg.i using the encoding documented in
  // rt/command.h and decoded by RealtimeEngine::apply_command.
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  rt::Command command{};
  command.type = rt::CommandType::kMidiCcImmediate;
  command.target_id = destination_id;
  command.sample_time = render_frame;
  command.arg.i = static_cast<int64_t>(packed);
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_push_midi_panic(SonareRealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)render_frame;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  rt::Command command{};
  command.type = rt::CommandType::kMidiAllNotesOff;
  command.target_id = 0;
  command.sample_time = render_frame;
  return engine->engine.push_command(command) ? SONARE_OK : SONARE_ERROR_OUT_OF_MEMORY;
#endif
}

SonareError sonare_engine_get_transport_state(SonareRealtimeEngine* engine,
                                              SonareTransportState* out) {
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const transport::TransportState state = engine->engine.transport_state_control();
  out->playing = state.playing ? 1 : 0;
  out->looping = state.looping ? 1 : 0;
  out->render_frame = state.render_frame;
  out->sample_position = state.sample_position;
  out->ppq_position = state.ppq_position;
  out->bpm = state.bpm;
  out->loop_start_ppq = state.loop_start_ppq;
  out->loop_end_ppq = state.loop_end_ppq;
  out->sample_rate = state.sample_rate;
  out->bar_start_ppq = state.bar_start_ppq;
  out->bar_count = state.bar_count;
  out->time_signature.numerator = state.time_sig.numerator;
  out->time_signature.denominator = state.time_sig.denominator;
  out->time_signature.confidence = 1.0f;
  return SONARE_OK;
}
