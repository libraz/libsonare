/// @file project_transcribe.cpp
/// @brief Embind binding for audio-to-MIDI transcription: the standalone
/// `transcribe` free function and the project's transcribe-into-a-clip method.
///
/// Both entries are thin marshallers over sonare_c_transcribe.h. The note
/// chains, the tempo fallback and the note-off-before-note-on ordering all stay
/// there, so the two doors cannot answer differently.
///
/// The config's "0 keeps the documented default" rule is the header's, so an
/// omitted field is left zero rather than seeded with a second copy of the
/// defaults -- unlike project_midi.cpp's tempo options, whose zero values are
/// not their defaults.
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

// A field whose domain excludes 0 spells "use the documented default" as 0 in
// the C ABI, which has no way to tell that from a caller who wrote it. Omission
// already spells the default here, so a present 0 is a value the caller chose
// and it is out of domain -- answering with the default would hand back a
// number they did not ask for, indistinguishable downstream from one they did.
// A present value is otherwise forwarded as given and range-checked once, by
// the C ABI.
float signedField(val config, const char* key, bool want_positive) {
  const float value = floatProperty(config, key, 0.0f);
  if (hasProperty(config, key) && (want_positive ? !(value > 0.0f) : !(value < 0.0f))) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(key) + (want_positive ? " must be a positive number" : " must be negative"));
  }
  return value;
}

// The same rule on the one field whose domain is a range rather than a sign.
// 0 is the C ABI's "measure the level instead", and omitting the field already
// says that here, so a written 0 is a value -- and 0 is not a MIDI velocity.
//
// Presence is the only thing that can decide this one. The other six have an
// out-of-domain sentinel, so their written 0 is separable by value; this
// field's sentinel IS its default, and an omitted key still has to reach the C
// ABI as 0, so a value check would either refuse omission or accept a written
// zero. Wording is Node's verbatim, so one domain reports one way everywhere.
int velocityField(val config, const char* key) {
  const int value = intProperty(config, key, 0);
  if (hasProperty(config, key) && (value < 1 || value > 127)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " must be an integer in [1, 127]");
  }
  return value;
}

SonareTranscribeConfig transcribeConfigFromVal(val config) {
  SonareTranscribeConfig out = {};
  out.struct_version = 1;
  if (config.isUndefined() || config.isNull()) return out;
  out.polyphonic = boolProperty(config, "polyphonic", false) ? 1 : 0;
  out.reference_hz = signedField(config, "referenceHz", true);
  out.fmin = signedField(config, "fmin", true);
  out.fmax = signedField(config, "fmax", true);
  out.min_note_ms = signedField(config, "minNoteMs", true);
  out.segmentation_threshold_cents = signedField(config, "segmentationThresholdCents", true);
  out.velocity_floor_db = signedField(config, "velocityFloorDb", false);
  out.fixed_velocity = velocityField(config, "fixedVelocity");
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
    throwCError(err, "failed to transcribe audio");
  }
  const double note_count = static_cast<double>(result.note_count);
  const float tempo = result.tempo_bpm;
  const std::vector<SonareMidiEventPod> events = takeTranscribeEvents(&result);

  val list = val::array();
  for (size_t i = 0; i < events.size(); ++i) {
    list.set(static_cast<unsigned>(i), js_midi_event_to_val(events[i]));
  }
  val out = val::object();
  out.set("events", list);
  out.set("noteCount", note_count);
  out.set("tempoBpm", tempo);
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
    throwCError(err, "failed to transcribe into MIDI clip");
  }
  return static_cast<uint32_t>(note_count);
}

void registerProjectTranscribe(class_<ProjectWasm>& cls) {
  cls.function("transcribeToClip", &ProjectWasm::transcribeToClip);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
