#include <cstring>
#include <optional>
#include <string>

#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

const char* SectionTypeName(SonareSectionType type) {
  switch (type) {
    case SONARE_SECTION_INTRO:
      return "Intro";
    case SONARE_SECTION_VERSE:
      return "Verse";
    case SONARE_SECTION_PRE_CHORUS:
      return "Pre-Chorus";
    case SONARE_SECTION_CHORUS:
      return "Chorus";
    case SONARE_SECTION_BRIDGE:
      return "Bridge";
    case SONARE_SECTION_INSTRUMENTAL:
      return "Instrumental";
    case SONARE_SECTION_OUTRO:
      return "Outro";
    case SONARE_SECTION_UNKNOWN:
    default:
      return "Unknown";
  }
}

}  // namespace

Napi::Value SonareWrap::AnalyzeWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsFunction() || (info.Length() > 3 && !info[3].IsFunction())) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, onProgress, cancel?, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Lending the TypedArray's pointer across the progress/cancel callbacks is
  // safe here, and deliberately not defended against a second time: the C ABI
  // takes its own copy before a callback can ever fire. run_offline
  // (src/c_api/sonare_c_internal.h:285) builds the Audio with
  // Audio::from_buffer, which copies, and only the MusicAnalyzer constructed
  // from that Audio receives the progress callback. Nothing dereferences this
  // pointer after the call below returns, so a callback that transfers or
  // detaches the caller's ArrayBuffer cannot reach freed memory. Duplicating
  // the buffer here would cost a second full copy of a whole recording for a
  // hazard that the layer underneath already closes. That copy is pinned by the
  // "copies the input before the first progress callback" section of the
  // sonare_analyze_json case in tests/api/sonare_c_core_test.cpp.
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sample_rate = node_narrow_int(env, info[1], "sampleRate");
  Napi::Function js_cb = info[2].As<Napi::Function>();
  // Absent or non-object options read as the defaults, as analyze reads them.
  SonareMusicAnalyzeOptions options{};
  ReadMusicAnalyzeOptions(info.Length() > 4 ? info[4] : env.Undefined(), &options);
  if (env.IsExceptionPending()) return env.Undefined();

  // The C-ABI progress callback cannot hold a Napi reference (it is called
  // synchronously on the same thread, so the stack is still valid).
  struct ProgressCtx {
    Napi::Env env;
    Napi::Function cb;
    std::optional<Napi::Function> cancel;
  } ctx{env, js_cb,
        info.Length() > 3 ? std::optional<Napi::Function>(info[3].As<Napi::Function>())
                          : std::nullopt};

  // A JS callback that throws leaves an exception pending; every further N-API
  // call must be skipped, and the analysis is cancelled so the throw surfaces
  // promptly instead of after the whole pipeline has run.
  auto c_progress = [](float progress, const char* stage, void* user_data) {
    auto* c = static_cast<ProgressCtx*>(user_data);
    if (c->env.IsExceptionPending()) return;
    c->cb.Call({Napi::Number::New(c->env, static_cast<double>(progress)),
                Napi::String::New(c->env, stage != nullptr ? stage : "")});
  };
  auto c_cancel = [](void* user_data) {
    auto* c = static_cast<ProgressCtx*>(user_data);
    if (c->env.IsExceptionPending()) return 1;
    if (!c->cancel) return 0;
    const Napi::Value result = c->cancel->Call({});
    if (c->env.IsExceptionPending()) return 1;
    return result.IsBoolean() && result.As<Napi::Boolean>().Value() ? 1 : 0;
  };

  char* json_str = nullptr;
  SonareError err = sonare_analyze_json_ex_with_progress(
      data, length, sample_rate, &options, c_progress, &ctx, &json_str, c_cancel, &ctx);
  // The pending-exception check comes first: throwing a SonareError on top of a
  // callback's exception would abort the process under NAPI_DISABLE_CPP_EXCEPTIONS.
  if (env.IsExceptionPending()) {
    sonare_free_string(json_str);
    return env.Undefined();
  }
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  // Parse JSON and inject beatTimes on the main thread.
  Napi::Object json_global = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function json_parse = json_global.Get("parse").As<Napi::Function>();
  Napi::Value parsed =
      json_parse.Call({Napi::String::New(env, json_str != nullptr ? json_str : "")});
  sonare_free_string(json_str);

  if (env.IsExceptionPending() || !parsed.IsObject()) {
    if (!env.IsExceptionPending()) {
      Napi::Error::New(env, "Failed to parse analysis JSON").ThrowAsJavaScriptException();
    }
    return env.Undefined();
  }

  Napi::Object result = parsed.As<Napi::Object>();
  Napi::Error enrich_error;
  if (!sonare_node::EnrichFullAnalysisObject(env, result, &enrich_error)) {
    enrich_error.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeSections(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_arg_int(info, 1, 22050);
  const int n_fft = node_arg_int(info, 2, 2048);
  const int hop_length = node_arg_int(info, 3, 512);
  const float min_section_sec = node_arg_finite_float(info, 4, 4.0f);

  SonareSectionResult result{};
  SonareError err = sonare_analyze_sections(typed.Data(), typed.ElementLength(), sample_rate, n_fft,
                                            hop_length, min_section_sec, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Array sections = Napi::Array::New(env, result.section_count);
  for (size_t i = 0; i < result.section_count; ++i) {
    const SonareSection& section = result.sections[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("type", Napi::Number::New(env, static_cast<int>(section.type)));
    obj.Set("name", Napi::String::New(env, SectionTypeName(section.type)));
    obj.Set("start", Napi::Number::New(env, section.start));
    obj.Set("end", Napi::Number::New(env, section.end));
    obj.Set("energyLevel", Napi::Number::New(env, section.energy_level));
    obj.Set("confidence", Napi::Number::New(env, section.confidence));
    sections.Set(static_cast<uint32_t>(i), obj);
  }
  sonare_free_section_result(&result);
  return sections;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectBoundaries(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_arg_int(info, 1, 22050);

  // Seeded from the C ABI rather than restated, so the facade cannot drift from
  // the core's BoundaryConfig. No field here takes ZeroIsSentinel: the C entry
  // refuses a zero size and reads a zero threshold as "accept every peak".
  SonareBoundaryOptions options = sonare_boundary_options_default();
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object bag = info[2].As<Napi::Object>();
    options.n_fft = IntProperty(bag, "nFft", options.n_fft);
    options.hop_length = IntProperty(bag, "hopLength", options.hop_length);
    options.kernel_size = IntProperty(bag, "kernelSize", options.kernel_size);
    options.threshold = FiniteFloatProperty(bag, "threshold", options.threshold);
    options.absolute_threshold =
        FiniteFloatProperty(bag, "absoluteThreshold", options.absolute_threshold);
    options.n_mfcc = IntProperty(bag, "nMfcc", options.n_mfcc);
    options.n_chroma = IntProperty(bag, "nChroma", options.n_chroma);
    options.peak_distance = FiniteFloatProperty(bag, "peakDistance", options.peak_distance);
    options.use_mfcc = BoolProperty(bag, "useMfcc", options.use_mfcc != 0) ? 1 : 0;
    options.use_chroma = BoolProperty(bag, "useChroma", options.use_chroma != 0) ? 1 : 0;
  }

  SonareBoundaryResult result{};
  SonareError err =
      sonare_detect_boundaries(typed.Data(), typed.ElementLength(), sample_rate, &options, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Array boundaries = Napi::Array::New(env, result.boundary_count);
  for (size_t i = 0; i < result.boundary_count; ++i) {
    const SonareBoundary& boundary = result.boundaries[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("time", Napi::Number::New(env, boundary.time));
    obj.Set("frame", Napi::Number::New(env, boundary.frame));
    obj.Set("strength", Napi::Number::New(env, boundary.strength));
    boundaries.Set(static_cast<uint32_t>(i), obj);
  }

  auto novelty = Napi::Float32Array::New(env, result.novelty_length);
  if (result.novelty_length > 0 && result.novelty_curve != nullptr) {
    std::memcpy(novelty.Data(), result.novelty_curve, result.novelty_length * sizeof(float));
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("boundaries", boundaries);
  out.Set("noveltyCurve", novelty);
  out.Set("noveltyPeak", Napi::Number::New(env, result.novelty_peak));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("hopLength", Napi::Number::New(env, result.hop_length));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("frameStride", Napi::Number::New(env, result.frame_stride));
  sonare_free_boundary_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeMelody(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_arg_int(info, 1, 22050);
  const float fmin = node_arg_finite_float(info, 2, 65.0f);
  const float fmax = node_arg_finite_float(info, 3, 2093.0f);
  const int frame_length = node_arg_int(info, 4, 2048);
  const int hop_length = node_arg_int(info, 5, 256);
  const float threshold = node_arg_finite_float(info, 6, 0.1f);
  const int use_pyin =
      info.Length() >= 8 && info[7].IsBoolean() && info[7].As<Napi::Boolean>().Value() ? 1 : 0;
  // center defaults to true (matches librosa.pyin(center=True)); only honored
  // when use_pyin is set.
  const int center =
      info.Length() >= 9 && info[8].IsBoolean() ? (info[8].As<Napi::Boolean>().Value() ? 1 : 0) : 1;

  SonareMelodyResult result{};
  SonareError err =
      sonare_analyze_melody_ex(typed.Data(), typed.ElementLength(), sample_rate, fmin, fmax,
                               frame_length, hop_length, threshold, use_pyin, center, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Array points = Napi::Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    const SonareMelodyPoint& point = result.points[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("time", Napi::Number::New(env, point.time));
    obj.Set("frequency", Napi::Number::New(env, point.frequency));
    obj.Set("confidence", Napi::Number::New(env, point.confidence));
    points.Set(static_cast<uint32_t>(i), obj);
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("points", points);
  out.Set("pitchRangeOctaves", Napi::Number::New(env, result.pitch_range_octaves));
  out.Set("pitchStability", Napi::Number::New(env, result.pitch_stability));
  out.Set("meanFrequency", Napi::Number::New(env, result.mean_frequency));
  out.Set("vibratoRate", Napi::Number::New(env, result.vibrato_rate));
  sonare_free_melody_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}
