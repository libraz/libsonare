/// @file percussive_events.cpp
/// @brief Embind bindings for percussive event extraction and rendering.

#ifdef __EMSCRIPTEN__

#include "c_api/daw_config_domains.h"
#include "core/spectrum.h"
#include "editing/event_model/event_extractor.h"
#include "editing/event_model/event_renderer.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"

namespace {

using editing::event_model::PercussiveEvent;

// The separation both percussive-event calls take. An absent key keeps the core
// default (an absent hop derives it from nFft); a given value is read literally
// and checked as the C ABI checks it. Whether the framing overlap-adds is left to
// the core, which rejects it on both calls.
editing::event_model::PercussiveSeparationConfig percussiveSeparationFromVal(val options) {
  editing::event_model::PercussiveSeparationConfig separation;
  if (hasProperty(options, "nFft")) {
    separation.n_fft = domainNumberFromVal<int>(options["nFft"], "nFft", sonare::kColaNFftDomain);
  }
  if (hasProperty(options, "hopLength")) {
    separation.hop_length =
        domainNumberFromVal<int>(options["hopLength"], "hopLength", sonare::kStftHopDomain);
  }
  if (hasProperty(options, "hpssKernelHarmonic")) {
    separation.hpss.kernel_size_harmonic =
        domainNumberFromVal<int>(options["hpssKernelHarmonic"], "hpssKernelHarmonic",
                                 sonare_c_detail::kPercussiveHpssKernelDomain);
  }
  if (hasProperty(options, "hpssKernelPercussive")) {
    separation.hpss.kernel_size_percussive =
        domainNumberFromVal<int>(options["hpssKernelPercussive"], "hpssKernelPercussive",
                                 sonare_c_detail::kPercussiveHpssKernelDomain);
  }
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
  event.onset_sample = requireInt64Property(row, "onsetSample", "event");
  event.offset_sample = requireInt64Property(row, "offsetSample", "event");
  const val edit = objectProperty(row, "edit");
  if (hasProperty(edit, "timeOffsetSamples")) {
    event.edit.time_offset_samples = requireInt64Property(edit, "timeOffsetSamples", "event.edit");
  }
  event.edit.gain_db = floatProperty(edit, "gainDb", event.edit.gain_db);
  event.edit.muted = boolProperty(edit, "muted", event.edit.muted);
  return event;
}

}  // namespace

// Percussive events: struck sounds located in time, and the render pass that
// writes an edited set back over the audio they were measured against. Both
// mirror the C ABI's validation even though they call the core directly.
val js_extract_percussive_events(val samples, const val& sample_rate, val options) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  editing::event_model::PercussiveEventExtractorConfig config;
  config.separation = percussiveSeparationFromVal(options);

  // config.onset keeps percussive_onset_defaults(), so backtracking stays on:
  // a span that opened after its own transient would report the next hit's peak
  // and leave its attack behind when muted. Only the two peak-picking knobs the
  // C ABI exposes are read.
  config.onset.wait = intProperty(options, "onsetWait", config.onset.wait);
  config.onset.delta = floatProperty(options, "onsetDelta", config.onset.delta);
  config.max_event_ms = floatProperty(options, "maxEventMs", config.max_event_ms);
  config.min_percussive_ratio =
      floatProperty(options, "minPercussiveRatio", config.min_percussive_ratio);
  sonare::util::check::greater_than(config.max_event_ms, 0.0, "maxEventMs");
  editing::event_model::check_percussive_extract_controls(
      config.onset.wait, config.onset.delta, config.max_event_ms, config.min_percussive_ratio);

  Audio audio = loadValidatedAudio("samples", samples, rate);
  val out = val::array();
  const std::vector<PercussiveEvent> events = sonare::util::check::with_camel_paths(
      [&] { return editing::event_model::extract_percussive_events(audio, config); },
      kPercussiveSeparationKeys);
  for (const PercussiveEvent& event : events) {
    out.call<void>("push", percussiveEventToVal(event));
  }
  return out;
}

val js_render_percussive_events(val samples, const val& sample_rate, val events, val options) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  editing::event_model::PercussiveEventRenderConfig config;
  config.separation = percussiveSeparationFromVal(options);
  config.fade_ms = floatProperty(options, "fadeMs", config.fade_ms);
  sonare::util::check::greater_than(config.fade_ms, 0.0, "fadeMs");

  const std::size_t count = wasmArrayLength(events, "events");
  std::vector<PercussiveEvent> core_events;
  core_events.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    core_events.push_back(renderablePercussiveEventFromVal(events[i]));
  }

  Audio audio = loadValidatedAudio("samples", samples, rate);
  Audio result = sonare::util::check::with_camel_paths(
      [&] { return editing::event_model::render_percussive_events(audio, core_events, config); },
      kPercussiveSeparationKeys);
  return vectorToFloat32Array(result.data(), result.size());
}

void registerEffectsPercussiveEventBindings() {
  function("extractPercussiveEvents", &js_extract_percussive_events);
  function("renderPercussiveEvents", &js_render_percussive_events);
}

#endif  // __EMSCRIPTEN__
