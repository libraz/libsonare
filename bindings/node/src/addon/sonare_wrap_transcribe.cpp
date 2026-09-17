#include "sonare_wrap_transcribe.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

// Refuses the values the C ABI structurally CANNOT refuse.
//
// On the C ABI a field's 0 spells "keep the documented default", so a caller who
// writes 0 into one of these gets the default and no error. That sentinel has no
// job on a JS surface, where omitting the key already means the default: a
// written 0 is therefore a value the field's own domain excludes, and letting it
// through returns an answer measured against something the caller never asked
// for -- indistinguishable downstream from a deliberate one. hasProperty-class
// knowledge is what makes this reachable here and not there.
//
// The bound is checked on the resulting value rather than on presence, which is
// sound because every fallback below is the C ABI's own default and all of them
// are in domain: a refusal can only ever name a value the caller wrote.
// fixedVelocity is the exception and is handled at its read instead, because its
// default IS the 0 under discussion. group and channel are absent on purpose --
// there 0 is a value a caller can mean, and that is the distinction deciding the
// whole set.
bool RefuseOutOfDomain(Napi::Env env, const SonareTranscribeConfig& config) {
  struct PositiveField {
    const char* key;
    float value;
  };
  const PositiveField positive[] = {
      {"referenceHz", config.reference_hz},
      {"fmin", config.fmin},
      {"fmax", config.fmax},
      {"minNoteMs", config.min_note_ms},
      {"segmentationThresholdCents", config.segmentation_threshold_cents},
  };
  for (const PositiveField& field : positive) {
    if (!(field.value > 0.0f)) {
      ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                              std::string(field.key) + " must be a positive number");
      return false;
    }
  }
  if (!(config.velocity_floor_db < 0.0f)) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                            "velocityFloorDb must be negative");
    return false;
  }
  return true;
}

// Seeds the C ABI's own defaults, then applies whichever keys the caller wrote.
// Seeding rather than zeroing keeps the documented default of every field in one
// place — sonare_transcribe_config_default() — so this file cannot drift from
// the header a caller reads.
//
// The floats take the finite reader: none of these fields documents an infinity
// or a NaN as selecting anything, so a non-finite value is refused by name here
// rather than reaching the C ABI, which refuses it without saying which field
// carried it.
//
// fixedVelocity is read WITHOUT ZeroIsSentinel, which is the one place that tag
// would be actively misleading: it exists so a refusal can say "its zero selects
// the library default", and that sentence is false for a caller here, whose zero
// is refused. The C ABI still reads the 0 an omitted key sends as "measure";
// what changed is that no caller can spell it.
bool ReadTranscribeConfig(Napi::Env env, const Napi::Object& request, SonareTranscribeConfig* out) {
  *out = sonare_transcribe_config_default();
  out->polyphonic = BoolProperty(request, "polyphonic", out->polyphonic != 0) ? 1 : 0;
  out->reference_hz = FiniteFloatProperty(request, "referenceHz", out->reference_hz);
  out->fmin = FiniteFloatProperty(request, "fmin", out->fmin);
  out->fmax = FiniteFloatProperty(request, "fmax", out->fmax);
  out->min_note_ms = FiniteFloatProperty(request, "minNoteMs", out->min_note_ms);
  out->segmentation_threshold_cents =
      FiniteFloatProperty(request, "segmentationThresholdCents", out->segmentation_threshold_cents);
  out->velocity_floor_db = FiniteFloatProperty(request, "velocityFloorDb", out->velocity_floor_db);
  // Presence, not value: an omitted fixedVelocity has to reach the C ABI as the
  // 0 that means "measure", and a written 0 has to be refused, so the number
  // alone cannot say which happened. Get()/IsUndefined() rather than Has(),
  // which would make an explicit `undefined` diverge from an omitted field.
  const Napi::Value fixed_velocity = request.Get("fixedVelocity");
  const bool wrote_fixed_velocity = !fixed_velocity.IsUndefined() && !fixed_velocity.IsNull();
  out->fixed_velocity = IntProperty(request, "fixedVelocity", 0);
  out->group = IntProperty(request, "group", out->group);
  out->channel = IntProperty(request, "channel", out->channel);
  // Short-circuited: a reader that already refused a key left an exception
  // pending, and a second throw on top of it aborts rather than reaching JS.
  if (env.IsExceptionPending()) return false;
  if (wrote_fixed_velocity && (out->fixed_velocity < 1 || out->fixed_velocity > 127)) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                            "fixedVelocity must be an integer in [1, 127]");
    return false;
  }
  return RefuseOutOfDomain(env, *out);
}

// The request shape both entry points share: mono audio plus its rate. Returns
// false with exactly one pending JS exception, so the caller bails out before
// the C-ABI call.
//
// @p fn_name precedes the object because a (Env, Object, char*) parameter list
// is the shape a key reader has, and a key reader outside sonare_wrap_options.h
// is what the addon's reader-convention guard exists to refuse.
bool ReadTranscribeSource(Napi::Env env, const char* fn_name, const Napi::Object& request,
                          Napi::Float32Array* out_samples, int* out_sample_rate) {
  const Napi::Value samples = request.Get("samples");
  if (!IsFloat32Array(samples)) {
    Napi::TypeError::New(env, std::string(fn_name) + " requires samples as a Float32Array")
        .ThrowAsJavaScriptException();
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
bool RequireRequestObject(Napi::Env env, const char* fn_name, const Napi::CallbackInfo& info,
                          Napi::Object* out) {
  if (info.Length() != 1 || !info[0].IsObject() || info[0].IsArray()) {
    Napi::TypeError::New(env, std::string(fn_name) + " expects one request object")
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
  if (!RequireRequestObject(env, "transcribe", info, &request)) return env.Undefined();
  Napi::Float32Array samples;
  int sample_rate = 0;
  if (!ReadTranscribeSource(env, "transcribe", request, &samples, &sample_rate)) {
    return env.Undefined();
  }
  // A tempo <= 0 is the C ABI's "detect it", which is also what an omitted key
  // means, so the fallback and the sentinel are the same value.
  const float tempo_bpm = FiniteFloatProperty(request, "tempoBpm", 0.0f);
  SonareTranscribeConfig config{};
  if (!ReadTranscribeConfig(env, request, &config)) return env.Undefined();

  TranscribeResultGuard result;
  const SonareError error = sonare_transcribe(samples.Data(), samples.ElementLength(), sample_rate,
                                              tempo_bpm, &config, &result.value);
  if (error != SONARE_OK) {
    ThrowIfError(env, error);
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
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node

Napi::Value ProjectWrap::TranscribeToClip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Object request;
  if (!sonare_node::RequireRequestObject(env, "transcribeToClip", info, &request)) {
    return env.Undefined();
  }
  uint32_t clip_id = 0;
  if (!sonare_node::RequiredUint32Property(env, request, "clipId", &clip_id)) {
    return env.Undefined();
  }
  Napi::Float32Array samples;
  int sample_rate = 0;
  if (!sonare_node::ReadTranscribeSource(env, "transcribeToClip", request, &samples,
                                         &sample_rate)) {
    return env.Undefined();
  }
  // No tempo argument: the grid is the project's own tempo map, which is why the
  // C ABI takes none either.
  SonareTranscribeConfig config{};
  if (!sonare_node::ReadTranscribeConfig(env, request, &config)) return env.Undefined();

  size_t note_count = 0;
  sonare_node::ThrowIfError(env, sonare_project_transcribe_to_clip(
                                     project_, clip_id, samples.Data(), samples.ElementLength(),
                                     sample_rate, &config, &note_count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(note_count));
  SONARE_NODE_CATCH(env)
}
