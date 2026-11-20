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
// f0MinHz, f0MaxHz and minNoteMs are the fields whose C defaults are themselves 0
// sentinels: an omitted key must stay 0 so the C layer can resolve it from the
// selected source. Their presence is therefore carried separately and only present values are
// checked here. fixedVelocity is handled at its read for the same reason. group
// and channel are absent on purpose -- there 0 is a value a caller can mean, and
// that is the distinction deciding the whole set.
bool RefuseOutOfDomain(Napi::Env env, const SonareTranscribeConfig& config, bool has_f0_min,
                       bool has_f0_max, bool has_min_note_ms) {
  struct PositiveField {
    const char* key;
    float value;
    bool present;
  };
  const PositiveField positive[] = {
      {"referenceHz", config.reference_hz, true},
      {"f0MinHz", config.f0_min_hz, has_f0_min},
      {"f0MaxHz", config.f0_max_hz, has_f0_max},
      {"minNoteMs", config.min_note_ms, has_min_note_ms},
      {"segmentationThresholdCents", config.segmentation_threshold_cents, true},
  };
  for (const PositiveField& field : positive) {
    if (!field.present) continue;
    if (!(field.value > 0.0f)) {
      ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                              std::string(field.key) + " must be finite and greater than 0");
      return false;
    }
  }
  if (!(config.velocity_floor_db < 0.0f)) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                            "velocityFloorDb must be finite and less than 0");
    return false;
  }
  return true;
}

// Seeds the C ABI's own defaults, then applies whichever keys the caller wrote.
// Seeding rather than zeroing keeps the documented default of every field in one
// place — sonare_transcribe_config_default() — so this file cannot drift from
// the header a caller reads. f0MinHz/f0MaxHz intentionally remain zero when omitted:
// the C layer resolves those sentinels against the selected tracker source, and
// so does minNoteMs, whose default is the source's.
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
  bool reference_auto = false;
  out->reference_hz =
      AutoFiniteFloatProperty(request, "referenceHz", out->reference_hz, &reference_auto);
  out->reference_auto = reference_auto ? 1 : 0;
  const Napi::Value f0_min = request.Get("f0MinHz");
  const bool wrote_f0_min = !f0_min.IsUndefined() && !f0_min.IsNull();
  out->f0_min_hz = FiniteFloatProperty(request, "f0MinHz", out->f0_min_hz);
  const Napi::Value f0_max = request.Get("f0MaxHz");
  const bool wrote_f0_max = !f0_max.IsUndefined() && !f0_max.IsNull();
  out->f0_max_hz = FiniteFloatProperty(request, "f0MaxHz", out->f0_max_hz);
  const Napi::Value min_note_ms = request.Get("minNoteMs");
  const bool wrote_min_note_ms = !min_note_ms.IsUndefined() && !min_note_ms.IsNull();
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
  // maxPolyphony, minNoteDivision and the two peak ratios carry presence for the same reason as
  // fixedVelocity: the C ABI's 0 means "default", so an omitted key stays 0 while
  // a written 0 is either refused (maxPolyphony) or mapped to the C ABI's spelling
  // of a real zero (the ratios, where a negative value means 0 / no split).
  const Napi::Value max_polyphony = request.Get("maxPolyphony");
  const bool wrote_max_polyphony = !max_polyphony.IsUndefined() && !max_polyphony.IsNull();
  out->max_polyphony = IntProperty(request, "maxPolyphony", 0);
  const Napi::Value min_note_division = request.Get("minNoteDivision");
  const bool wrote_min_note_division =
      !min_note_division.IsUndefined() && !min_note_division.IsNull();
  out->min_note_division = IntProperty(request, "minNoteDivision", 0);
  const Napi::Value frame_ratio = request.Get("minFramePeakRatio");
  const bool wrote_frame_ratio = !frame_ratio.IsUndefined() && !frame_ratio.IsNull();
  out->min_frame_peak_ratio = FiniteFloatProperty(request, "minFramePeakRatio", 0.0f);
  const Napi::Value ridge_ratio = request.Get("minRidgePeakRatio");
  const bool wrote_ridge_ratio = !ridge_ratio.IsUndefined() && !ridge_ratio.IsNull();
  out->min_ridge_peak_ratio = FiniteFloatProperty(request, "minRidgePeakRatio", 0.0f);
  // 0 is the C meaning "no split"; any other value is validated by the C layer.
  out->reattack_ratio = FiniteFloatProperty(request, "reattackRatio", 0.0f);
  // A string name rather than the ordinal: undefined/null keeps the C default
  // (segments), a non-string is a TypeError and an unknown name a RangeError.
  const std::string monophonic_model = StringProperty(request, "monophonicModel", "segments");
  if (monophonic_model == "segments") {
    out->monophonic_model = 0;
  } else if (monophonic_model == "singing") {
    out->monophonic_model = 1;
  } else {
    throw Napi::RangeError::New(
        env, "monophonicModel must be 'segments' or 'singing', got '" + monophonic_model + "'");
  }
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
  if (wrote_max_polyphony && (out->max_polyphony < 1 || out->max_polyphony > 64)) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                            "maxPolyphony must be an integer in [1, 64]");
    return false;
  }
  if (wrote_min_note_division && (out->min_note_division < 1 || out->min_note_division > 128)) {
    ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                            "minNoteDivision must be an integer in [1, 128]");
    return false;
  }
  const struct {
    const char* key;
    float* value;
    bool present;
  } peak_ratios[] = {
      {"minFramePeakRatio", &out->min_frame_peak_ratio, wrote_frame_ratio},
      {"minRidgePeakRatio", &out->min_ridge_peak_ratio, wrote_ridge_ratio},
  };
  for (const auto& ratio : peak_ratios) {
    if (!ratio.present) continue;
    if (!(*ratio.value >= 0.0f && *ratio.value <= 1.0f)) {
      ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                              std::string(ratio.key) + " must be a number in [0, 1]");
      return false;
    }
    // A written 0 is a real zero; the C ABI spells that as a negative value.
    if (*ratio.value == 0.0f) *ratio.value = -1.0f;
  }
  return RefuseOutOfDomain(env, *out, wrote_f0_min, wrote_f0_max, wrote_min_note_ms);
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
  const float tempo_bpm = FiniteFloatProperty(request, "tempoBpm", 0.0f);
  SonareTranscribeConfig config{};
  if (!ReadTranscribeConfig(env, request, &config)) return env.Undefined();

  TranscribeResultGuard result;
  const SonareError error = sonare_transcribe(samples.Data(), samples.ElementLength(), sample_rate,
                                              tempo_bpm, &config, &result.value);
  if (error != SONARE_OK) {
    sonare_node::ThrowSonareErrorFlattening(env, error, "config");
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
      sonare_project_transcribe_to_clip(project_, clip_id, samples.Data(), samples.ElementLength(),
                                        sample_rate, &config, &note_count);
  if (error != SONARE_OK) {
    sonare_node::ThrowSonareErrorFlattening(env, error, "config");
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(note_count));
  SONARE_NODE_CATCH(env)
}
