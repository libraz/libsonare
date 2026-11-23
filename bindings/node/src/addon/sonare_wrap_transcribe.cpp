#include "sonare_wrap_transcribe.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "c_api/transcribe_input_domains.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

// Seeds the C ABI's own defaults, then applies whichever keys the caller wrote, so the documented
// defaults live only in sonare_transcribe_config_init(). An omitted key keeps the initialised NaN
// or UNSET value, which the C layer resolves against the selected source; a written value is read
// in the C ABI's domain for its field under the caller's key.
bool ReadTranscribeConfig(Napi::Env env, const Napi::Object& request, SonareTranscribeConfig* out) {
  sonare_transcribe_config_init(out);
  out->polyphonic = BoolProperty(request, "polyphonic", out->polyphonic != 0) ? 1 : 0;
  bool reference_auto = false;
  const Napi::Value reference = request.Get("referenceHz");
  out->reference_hz =
      AutoFiniteFloatProperty(request, "referenceHz", out->reference_hz, &reference_auto);
  if (!reference_auto && !reference.IsUndefined() && !reference.IsNull()) {
    out->reference_hz = node_domain_number<float>(reference, "referenceHz",
                                                  sonare_c_detail::kTranscribeFrequencyHzDomain);
  }
  out->reference_auto = reference_auto ? 1 : 0;
  out->f0_min_hz = DomainProperty<float>(request, "f0MinHz", out->f0_min_hz,
                                         sonare_c_detail::kTranscribeFrequencyHzDomain);
  out->f0_max_hz = DomainProperty<float>(request, "f0MaxHz", out->f0_max_hz,
                                         sonare_c_detail::kTranscribeFrequencyHzDomain);
  out->min_note_ms = DomainProperty<float>(request, "minNoteMs", out->min_note_ms,
                                           sonare_c_detail::kTranscribeMinNoteMsDomain);
  out->segmentation_threshold_cents = DomainProperty<float>(
      request, "segmentationThresholdCents", out->segmentation_threshold_cents,
      sonare_c_detail::kTranscribeSegmentationCentsDomain);
  out->velocity_floor_db = DomainProperty<float>(request, "velocityFloorDb", out->velocity_floor_db,
                                                 sonare_c_detail::kTranscribeVelocityFloorDbDomain);
  // Absence selects the C ABI's 0; a written value is checked against the given-value domain.
  out->fixed_velocity = DomainProperty<int>(request, "fixedVelocity", 0,
                                            sonare_c_detail::kTranscribeFixedVelocityDomain);
  out->max_polyphony = DomainProperty<int>(request, "maxPolyphony", 0,
                                           sonare_c_detail::kTranscribeMaxPolyphonyDomain);
  out->min_note_division = DomainProperty<int>(request, "minNoteDivision", 0,
                                               sonare_c_detail::kTranscribeMinNoteDivisionDomain);
  out->min_frame_peak_ratio =
      DomainProperty<float>(request, "minFramePeakRatio", out->min_frame_peak_ratio,
                            sonare_c_detail::kTranscribePeakRatioDomain);
  out->min_ridge_peak_ratio =
      DomainProperty<float>(request, "minRidgePeakRatio", out->min_ridge_peak_ratio,
                            sonare_c_detail::kTranscribePeakRatioDomain);
  // Absent stays NaN, the C meaning "no split".
  out->reattack_ratio = DomainProperty<float>(request, "reattackRatio", out->reattack_ratio,
                                              sonare_c_detail::kReattackRatioDomain);
  // A string name rather than the ordinal: undefined/null keeps the C default
  // (segments), a non-string is a TypeError and an unknown name a RangeError.
  const std::string monophonic_model = StringProperty(request, "monophonicModel", "segments");
  if (monophonic_model == "segments") {
    out->monophonic_model = 0;
  } else if (monophonic_model == "singing") {
    out->monophonic_model = 1;
  } else {
    throw Napi::RangeError::New(env, "monophonicModel must be one of segments, singing");
  }
  out->group = IntProperty(request, "group", out->group);
  out->channel = IntProperty(request, "channel", out->channel);
  // A reader that already refused a key left an exception pending.
  return !env.IsExceptionPending();
}

// The request shape both entry points share: mono audio plus its rate. Returns
// false with exactly one pending JS exception, so the caller bails out before
// the C-ABI call.
bool ReadTranscribeSource(Napi::Env env, const Napi::Object& request,
                          Napi::Float32Array* out_samples, int* out_sample_rate) {
  const Napi::Value samples = request.Get("samples");
  if (!IsFloat32Array(samples)) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return false;
  }
  if (!RequiredIntProperty(env, request, "sampleRate", out_sample_rate)) return false;
  *out_samples = samples.As<Napi::Float32Array>();
  return true;
}

// Releases the heap-owned event array however the marshalling below leaves, so
// a JS error raised part-way through it cannot leak the result.
struct TranscribeResultGuard {
  SonareTranscribeResult value{};

  ~TranscribeResultGuard() { sonare_free_transcribe_result(&value); }
};

// Rejects anything but a plain request object, so a positional call is reported
// rather than read as an empty bag whose every field took its default.
bool RequireRequestObject(Napi::Env env, const Napi::CallbackInfo& info, Napi::Object* out) {
  if (info.Length() != 1 || !info[0].IsObject() || info[0].IsArray()) {
    Napi::TypeError::New(env, "request must be a single object argument")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = info[0].As<Napi::Object>();
  return true;
}

}  // namespace

Napi::Value Transcribe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Object request;
  if (!RequireRequestObject(env, info, &request)) return env.Undefined();
  Napi::Float32Array samples;
  int sample_rate = 0;
  if (!ReadTranscribeSource(env, request, &samples, &sample_rate)) {
    return env.Undefined();
  }
  // A tempo <= 0 is the C ABI's "detect it", which is also what an omitted key
  // means, so the fallback and the sentinel are the same value.
  const float tempo_bpm = FloatProperty(request, "tempoBpm", 0.0f);
  SonareTranscribeConfig config{};
  if (!ReadTranscribeConfig(env, request, &config)) return env.Undefined();

  TranscribeResultGuard result;
  const SonareError error =
      sonare_transcribe(sonare_node::ElementData(samples), samples.ElementLength(), sample_rate,
                        tempo_bpm, &config, &result.value);
  if (error != SONARE_OK) {
    sonare_node::ThrowSonareError(env, error);
    return env.Undefined();
  }

  Napi::Array events = Napi::Array::New(env, result.value.count);
  for (size_t i = 0; i < result.value.count; ++i) {
    events.Set(static_cast<uint32_t>(i), MidiEventToObject(env, result.value.events[i]));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("events", events);
  out.Set("noteCount", Napi::Number::New(env, static_cast<double>(result.value.note_count)));
  out.Set("tempoBpm", Napi::Number::New(env, result.value.tempo_bpm));
  out.Set("tuningCents", Napi::Number::New(env, result.value.tuning_cents));
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node

Napi::Value ProjectWrap::TranscribeToClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  Napi::Object request;
  if (!sonare_node::RequireRequestObject(env, info, &request)) {
    return env.Undefined();
  }
  uint32_t clip_id = 0;
  if (!sonare_node::RequiredUint32Property(env, request, "clipId", &clip_id)) {
    return env.Undefined();
  }
  Napi::Float32Array samples;
  int sample_rate = 0;
  if (!sonare_node::ReadTranscribeSource(env, request, &samples, &sample_rate)) {
    return env.Undefined();
  }
  // No tempo argument: the grid is the project's own tempo map, which is why the
  // C ABI takes none either.
  SonareTranscribeConfig config{};
  if (!sonare_node::ReadTranscribeConfig(env, request, &config)) return env.Undefined();

  size_t note_count = 0;
  const SonareError error =
      sonare_project_transcribe_to_clip(project_, clip_id, sonare_node::ElementData(samples),
                                        samples.ElementLength(), sample_rate, &config, &note_count);
  if (error != SONARE_OK) {
    sonare_node::ThrowSonareError(env, error);
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(note_count));
  SONARE_NODE_CATCH(env)
}
