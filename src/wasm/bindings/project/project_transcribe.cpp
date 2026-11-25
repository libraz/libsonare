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

#include <cmath>
#include <limits>

#include "c_api/transcribe_input_domains.h"
#include "project_wasm.h"
#include "transport/tempo_map.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

// Omission keeps the initialised NaN or UNSET value; a written value is read in the C ABI's domain
// for the field under the caller's key.
template <typename T>
T domainField(val config, const char* key, const sonare::util::check::NumericDomain& domain,
              T absent) {
  return optionalDomainValue<T>(objectProperty(config, key), key, domain).value_or(absent);
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
      throw WasmTypeError("referenceHz must be a number or 'auto'");
    }
    out.reference_auto = 1;
  } else {
    out.reference_hz = domainField<float>(
        config, "referenceHz", sonare_c_detail::kTranscribeFrequencyHzDomain, out.reference_hz);
  }
  out.f0_min_hz = domainField<float>(config, "f0MinHz",
                                     sonare_c_detail::kTranscribeFrequencyHzDomain, out.f0_min_hz);
  out.f0_max_hz = domainField<float>(config, "f0MaxHz",
                                     sonare_c_detail::kTranscribeFrequencyHzDomain, out.f0_max_hz);
  out.min_note_ms = domainField<float>(
      config, "minNoteMs", sonare_c_detail::kTranscribeMinNoteMsDomain, out.min_note_ms);
  // Both spellings written is forwarded; the C ABI refuses it naming min_note_division.
  out.min_note_division =
      domainField<int>(config, "minNoteDivision", sonare_c_detail::kTranscribeMinNoteDivisionDomain,
                       SONARE_NOTE_DIVISION_UNSET);
  out.segmentation_threshold_cents = domainField<float>(
      config, "segmentationThresholdCents", sonare_c_detail::kTranscribeSegmentationCentsDomain,
      out.segmentation_threshold_cents);
  out.velocity_floor_db =
      domainField<float>(config, "velocityFloorDb",
                         sonare_c_detail::kTranscribeVelocityFloorDbDomain, out.velocity_floor_db);
  out.fixed_velocity =
      domainField<int>(config, "fixedVelocity", sonare_c_detail::kTranscribeFixedVelocityDomain,
                       SONARE_FIXED_VELOCITY_UNSET);
  // Polyphonic only; a monophonic call with any of these written is refused by the C ABI.
  out.max_polyphony =
      domainField<int>(config, "maxPolyphony", sonare_c_detail::kTranscribeMaxPolyphonyDomain,
                       SONARE_MAX_POLYPHONY_UNSET);
  out.min_frame_peak_ratio =
      domainField<float>(config, "minFramePeakRatio", sonare_c_detail::kTranscribePeakRatioDomain,
                         out.min_frame_peak_ratio);
  out.min_ridge_peak_ratio =
      domainField<float>(config, "minRidgePeakRatio", sonare_c_detail::kTranscribePeakRatioDomain,
                         out.min_ridge_peak_ratio);
  // Absent never splits.
  out.reattack_ratio = domainField<float>(
      config, "reattackRatio", sonare_c_detail::kReattackRatioDomain, out.reattack_ratio);
  // Absence is the segments model; the C ABI refuses a non-segments model on the polyphonic path.
  const std::string model = stringProperty(config, "monophonicModel", "segments");
  if (model == "singing") {
    out.monophonic_model = 1;
  } else if (model != "segments") {
    throw WasmRangeError("monophonicModel must be one of segments, singing");
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
  // The C ABI's zero means detection; only an absent value selects it here.
  const bool detect_tempo = tempo_bpm_val.isUndefined() || tempo_bpm_val.isNull();
  const double supplied = detect_tempo ? 0.0 : numberFromVal(tempo_bpm_val, "tempoBpm");
  const SonareTranscribeConfig resolved = transcribeConfigFromVal(config);
  float tempo_bpm = 0.0f;
  if (!detect_tempo) {
    tempo_bpm = sonare::util::check::saturated_float(supplied);
    // The C ABI uses zero for detection, so keep a written zero or underflow distinct from absence.
    if (tempo_bpm == 0.0f) {
      tempo_bpm = std::copysign(std::numeric_limits<float>::min(), supplied);
    }
    if (supplied >= sonare::transport::kMinPublicTempoBpm &&
        static_cast<double>(tempo_bpm) < sonare::transport::kMinPublicTempoBpm) {
      tempo_bpm = std::nextafterf(static_cast<float>(sonare::transport::kMinPublicTempoBpm),
                                  std::numeric_limits<float>::infinity());
    }
  }
  const std::vector<float> buffer = float32ArrayToVector(samples, "samples");

  SonareTranscribeResult result{};
  const SonareError err = sonare_transcribe(buffer.empty() ? nullptr : buffer.data(), buffer.size(),
                                            sample_rate, tempo_bpm, &resolved, &result);
  if (err != SONARE_OK) {
    throwCodedCError(err);
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
  const std::vector<float> buffer = float32ArrayToVector(samples, "samples");

  size_t note_count = 0;
  const SonareError err = sonare_project_transcribe_to_clip(
      project_.get(), clip_id, buffer.empty() ? nullptr : buffer.data(), buffer.size(), sample_rate,
      &resolved, &note_count);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<uint32_t>(note_count);
}

void registerProjectTranscribe(class_<ProjectWasm>& cls) {
  cls.function("transcribeToClip", &ProjectWasm::transcribeToClip);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
