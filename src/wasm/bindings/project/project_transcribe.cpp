/// @file project_transcribe.cpp
/// @brief Embind binding for audio-to-MIDI transcription: the standalone
/// `transcribe` free function and the project's transcribe-into-a-clip method.
///
/// Both entries are thin marshallers over sonare_c_transcribe.h. The note
/// chains, the tempo fallback and the note-off-before-note-on ordering all stay
/// there, so the two doors cannot answer differently.
///
/// An omitted field keeps what sonare_transcribe_config_init wrote (the C ABI's
/// absence spelling: NaN or an UNSET macro), so no second copy of the defaults
/// lives here.
///
/// It sits beside project_midi.cpp because it is arrangement-gated in exactly
/// the same way (the C-ABI unit reaches WASM only through
/// SONARE_C_PROJECT_SOURCES) and because the events it returns are marshalled
/// by js_midi_event_to_val, which that file owns.
///
/// Nothing here catches -- a refusal is a SonareException the module wrapper
/// turns into a SonareError.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

// What a written value of a float field must be. Omission keeps the value the
// config was initialised with, so a written 0 is a value the caller chose and
// is either out of domain or, for the ratio kind, a real 0.
enum class FloatDomain {
  kPositive,   // > 0
  kNegative,   // < 0
  kUnitRatio,  // [0, 1]
};

// A present value is range-checked here by the domain's own wording and
// otherwise forwarded as given; the C ABI checks it once more. An omitted
// field returns @p absent, the initialised value.
float signedField(val config, const char* key, FloatDomain domain, float absent) {
  if (!hasProperty(config, key)) return absent;
  const float value = floatProperty(config, key, 0.0f);
  const std::string name(key);
  switch (domain) {
    case FloatDomain::kPositive:
      if (!(value > 0.0f)) throw WasmRangeError(name + " must be finite and greater than 0");
      return value;
    case FloatDomain::kNegative:
      if (!(value < 0.0f)) throw WasmRangeError(name + " must be finite and less than 0");
      return value;
    case FloatDomain::kUnitRatio:
      if (!(value >= 0.0f && value <= 1.0f)) {
        throw WasmRangeError(name + " must be a number in [0, 1]");
      }
      return value;
  }
  return value;
}

// The same rule on the fields whose domain is an integer range rather than a
// sign. Omission keeps the initialised UNSET value; a written value is checked
// against [lo, hi], and 0 is in none of [1, 127], [1, 64] and [1, 128].
// Wording is Node's verbatim, so one domain reports one way everywhere.
int rangeField(val config, const char* key, int lo, int hi, int absent) {
  if (!hasProperty(config, key)) return absent;
  const int value = intProperty(config, key, 0);
  if (value < lo || value > hi) {
    throw WasmRangeError(std::string(key) + " must be an integer in [" + std::to_string(lo) + ", " +
                         std::to_string(hi) + "]");
  }
  return value;
}

SonareTranscribeConfig transcribeConfigFromVal(val config) {
  SonareTranscribeConfig out;
  sonare_transcribe_config_init(&out);
  if (config.isUndefined() || config.isNull()) return out;
  out.polyphonic = boolProperty(config, "polyphonic", out.polyphonic != 0) ? 1 : 0;
  // `referenceHz` is a positive number in Hz, or "auto" to measure it from the audio.
  const val reference = config["referenceHz"];
  if (reference.isString()) {
    if (reference.as<std::string>() != "auto") {
      throw WasmRangeError("referenceHz must be finite and greater than 0, or 'auto'");
    }
    out.reference_auto = 1;
  } else {
    out.reference_hz = signedField(config, "referenceHz", FloatDomain::kPositive, out.reference_hz);
  }
  out.f0_min_hz = signedField(config, "f0MinHz", FloatDomain::kPositive, out.f0_min_hz);
  out.f0_max_hz = signedField(config, "f0MaxHz", FloatDomain::kPositive, out.f0_max_hz);
  out.min_note_ms = signedField(config, "minNoteMs", FloatDomain::kPositive, out.min_note_ms);
  // Both spellings written is forwarded; the C ABI refuses it naming min_note_division.
  out.min_note_division = rangeField(config, "minNoteDivision", 1, 128, SONARE_NOTE_DIVISION_UNSET);
  out.segmentation_threshold_cents =
      signedField(config, "segmentationThresholdCents", FloatDomain::kPositive,
                  out.segmentation_threshold_cents);
  out.velocity_floor_db =
      signedField(config, "velocityFloorDb", FloatDomain::kNegative, out.velocity_floor_db);
  out.fixed_velocity = rangeField(config, "fixedVelocity", 1, 127, SONARE_FIXED_VELOCITY_UNSET);
  // Polyphonic only; a monophonic call with any of these written is refused by the C ABI.
  out.max_polyphony = rangeField(config, "maxPolyphony", 1, 64, SONARE_MAX_POLYPHONY_UNSET);
  out.min_frame_peak_ratio =
      signedField(config, "minFramePeakRatio", FloatDomain::kUnitRatio, out.min_frame_peak_ratio);
  out.min_ridge_peak_ratio =
      signedField(config, "minRidgePeakRatio", FloatDomain::kUnitRatio, out.min_ridge_peak_ratio);
  // Absent never splits; a written value is forwarded and the C ABI refuses an invalid one.
  out.reattack_ratio = floatProperty(config, "reattackRatio", out.reattack_ratio);
  // Absence is the segments model; the C ABI refuses a non-segments model on the polyphonic path.
  const std::string model = stringProperty(config, "monophonicModel", "segments");
  if (model == "singing") {
    out.monophonic_model = 1;
  } else if (model != "segments") {
    throw WasmRangeError("monophonicModel must be 'segments' or 'singing'");
  }
  // Not the same family: 0 is IN domain on these two -- group 0 and channel 0
  // are values a caller can mean -- so there is no sentinel to separate.
  out.group = intProperty(config, "group", 0);
  out.channel = intProperty(config, "channel", 0);
  return out;
}

// Copies the events out and releases the result before anything JS-facing is
// built, so a throw while marshalling cannot leak the heap block.
std::vector<SonareMidiEventPod> takeTranscribeEvents(SonareTranscribeResult* result) {
  std::vector<SonareMidiEventPod> out;
  if (result->events != nullptr && result->count != 0) {
    out.assign(result->events, result->events + result->count);
  }
  sonare_free_transcribe_result(result);
  return out;
}

}  // namespace

val js_transcribe(val samples, const val& sample_rate_val, const val& tempo_bpm_val, val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  // Omitted is the C ABI's "detect", which it spells as a non-positive tempo.
  const float tempo_bpm = tempo_bpm_val.isUndefined() || tempo_bpm_val.isNull()
                              ? 0.0f
                              : checkedFloatFromVal(tempo_bpm_val, "tempoBpm");
  const SonareTranscribeConfig resolved = transcribeConfigFromVal(config);
  const std::vector<float> buffer = float32ArrayToVector(samples);

  SonareTranscribeResult result{};
  const SonareError err = sonare_transcribe(buffer.empty() ? nullptr : buffer.data(), buffer.size(),
                                            sample_rate, tempo_bpm, &resolved, &result);
  if (err != SONARE_OK) {
    throwCodedCErrorFlattening(err, "config");
  }
  const double note_count = static_cast<double>(result.note_count);
  const float tempo = result.tempo_bpm;
  const float tuning_cents = result.tuning_cents;
  const std::vector<SonareMidiEventPod> events = takeTranscribeEvents(&result);

  val list = val::array();
  for (size_t i = 0; i < events.size(); ++i) {
    list.set(static_cast<unsigned>(i), js_midi_event_to_val(events[i]));
  }
  val out = val::object();
  out.set("events", list);
  out.set("noteCount", note_count);
  out.set("tempoBpm", tempo);
  out.set("tuningCents", tuning_cents);
  return out;
}

uint32_t ProjectWasm::transcribeToClip(const val& clip_id_val, val samples,
                                       const val& sample_rate_val, val config) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const SonareTranscribeConfig resolved = transcribeConfigFromVal(config);
  const std::vector<float> buffer = float32ArrayToVector(samples);

  size_t note_count = 0;
  const SonareError err = sonare_project_transcribe_to_clip(
      project_.get(), clip_id, buffer.empty() ? nullptr : buffer.data(), buffer.size(), sample_rate,
      &resolved, &note_count);
  if (err != SONARE_OK) {
    throwCodedCErrorFlattening(err, "config");
  }
  return static_cast<uint32_t>(note_count);
}

void registerProjectTranscribe(class_<ProjectWasm>& cls) {
  cls.function("transcribeToClip", &ProjectWasm::transcribeToClip);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
