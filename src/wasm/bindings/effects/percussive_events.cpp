/// @file percussive_events.cpp
/// @brief Embind bindings for percussive event extraction and rendering.

#ifdef __EMSCRIPTEN__

#include <cmath>

#include "editing/event_model/event_extractor.h"
#include "editing/event_model/event_renderer.h"
#include "wasm/bindings/common/common.h"

namespace {

using editing::event_model::PercussiveEvent;

// The separation both percussive-event calls take, with every field defaulting
// at 0 or absent (SonarePercussiveEventConfig / SonarePercussiveRenderConfig).
// Whether the framing overlap-adds is left to the core, which rejects it on
// both calls.
editing::event_model::PercussiveSeparationConfig percussiveSeparationFromVal(
    val options, const char* entry_point) {
  editing::event_model::PercussiveSeparationConfig separation;
  const int n_fft = intProperty(options, "nFft", 0);
  const int hop_length = intProperty(options, "hopLength", 0);
  const int kernel_harmonic = intProperty(options, "hpssKernelHarmonic", 0);
  const int kernel_percussive = intProperty(options, "hpssKernelPercussive", 0);
  // Rejected before the sentinel promotion: a negative value must not be
  // swallowed by the "0 keeps the default" rule that follows.
  if (n_fft < 0 || hop_length < 0 || kernel_harmonic < 0 || kernel_percussive < 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(entry_point) + ": the framing and kernel sizes must not be negative");
  }
  if (n_fft != 0) separation.n_fft = n_fft;
  if (hop_length != 0) separation.hop_length = hop_length;
  if (kernel_harmonic != 0) separation.hpss.kernel_size_harmonic = kernel_harmonic;
  if (kernel_percussive != 0) separation.hpss.kernel_size_percussive = kernel_percussive;
  return separation;
}

// One event as a JS object. Field names mirror the C ABI
// (sonare_extract_percussive_events); there is no per-frame curve to carry, so
// unlike a note object an event crosses as scalars alone.
val percussiveEventToVal(const PercussiveEvent& event) {
  val row = val::object();
  // Sample positions cross as plain JS numbers rather than BigInt, matching
  // every other int64 field on this surface.
  row.set("onsetSample", static_cast<double>(event.onset_sample));
  row.set("offsetSample", static_cast<double>(event.offset_sample));
  row.set("strength", event.strength);
  row.set("peakAmplitude", event.peak_amplitude);
  row.set("percussiveRatio", event.percussive_ratio);
  val edit = val::object();
  edit.set("timeOffsetSamples", static_cast<double>(event.edit.time_offset_samples));
  edit.set("gainDb", event.edit.gain_db);
  edit.set("muted", event.edit.muted);
  row.set("edit", edit);
  return row;
}

// One event as the renderer reads it: the span and the pending edit. The three
// measured figures are deliberately not read, so extraction's own output can be
// handed back unchanged (sonare_render_percussive_events). An absent edit field
// is its own identity spelling.
PercussiveEvent renderablePercussiveEventFromVal(const val& row) {
  PercussiveEvent event;
  event.onset_sample = static_cast<int64_t>(
      requireNumberProperty(row, "onsetSample", "renderPercussiveEvents event"));
  event.offset_sample = static_cast<int64_t>(
      requireNumberProperty(row, "offsetSample", "renderPercussiveEvents event"));
  const val edit = objectProperty(row, "edit");
  if (hasProperty(edit, "timeOffsetSamples")) {
    event.edit.time_offset_samples = static_cast<int64_t>(
        requireNumberProperty(edit, "timeOffsetSamples", "renderPercussiveEvents event.edit"));
  }
  event.edit.gain_db = floatProperty(edit, "gainDb", 0.0f);
  event.edit.muted = boolProperty(edit, "muted", false);
  return event;
}

}  // namespace

// Percussive events: struck sounds located in time, and the render pass that
// writes an edited set back over the audio they were measured against. Both
// mirror the C ABI's validation even though they call the core directly.
val js_extract_percussive_events(val samples, const val& sample_rate, val options) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  editing::event_model::PercussiveEventExtractorConfig config;
  config.separation = percussiveSeparationFromVal(options, "extractPercussiveEvents");

  const int onset_wait = intProperty(options, "onsetWait", 0);
  const float onset_delta = floatProperty(options, "onsetDelta", 0.0f);
  const float max_event_ms = floatProperty(options, "maxEventMs", 0.0f);
  const float min_percussive_ratio = floatProperty(options, "minPercussiveRatio", 0.0f);
  if (!std::isfinite(onset_delta) || !std::isfinite(max_event_ms) ||
      !std::isfinite(min_percussive_ratio) || onset_wait < 0 || max_event_ms < 0.0f ||
      min_percussive_ratio < 0.0f || min_percussive_ratio > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "extractPercussiveEvents: onsetDelta, maxEventMs and "
                          "minPercussiveRatio must be finite, onsetWait and maxEventMs must not "
                          "be negative, and minPercussiveRatio must be in [0, 1]");
  }
  // config.onset keeps percussive_onset_defaults(), so backtracking stays on:
  // a span that opened after its own transient would report the next hit's peak
  // and leave its attack behind when muted. Only the two peak-picking knobs the
  // C ABI exposes are overwritten.
  if (onset_wait != 0) config.onset.wait = onset_wait;
  if (onset_delta != 0.0f) config.onset.delta = onset_delta;
  if (max_event_ms != 0.0f) config.max_event_ms = max_event_ms;
  // Unlike the rest, 0 is this field's own meaning as well as its default, so
  // it is assigned unconditionally rather than read as "leave the default".
  config.min_percussive_ratio = min_percussive_ratio;

  Audio audio = loadValidatedAudio(samples, rate);
  val out = val::array();
  for (const PercussiveEvent& event :
       editing::event_model::extract_percussive_events(audio, config)) {
    out.call<void>("push", percussiveEventToVal(event));
  }
  return out;
}

val js_render_percussive_events(val samples, const val& sample_rate, val events, val options) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  editing::event_model::PercussiveEventRenderConfig config;
  config.separation = percussiveSeparationFromVal(options, "renderPercussiveEvents");
  const float fade_ms = floatProperty(options, "fadeMs", 0.0f);
  if (!std::isfinite(fade_ms) || fade_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "renderPercussiveEvents: fadeMs must be finite and non-negative");
  }
  if (fade_ms != 0.0f) config.fade_ms = fade_ms;

  const std::size_t count = wasmArrayLikeLength(events, "renderPercussiveEvents events");
  std::vector<PercussiveEvent> core_events;
  core_events.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    core_events.push_back(renderablePercussiveEventFromVal(events[i]));
  }

  Audio audio = loadValidatedAudio(samples, rate);
  Audio result = editing::event_model::render_percussive_events(audio, core_events, config);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

void registerEffectsPercussiveEventBindings() {
  function("extractPercussiveEvents", &js_extract_percussive_events);
  function("renderPercussiveEvents", &js_render_percussive_events);
}

#endif  // __EMSCRIPTEN__
