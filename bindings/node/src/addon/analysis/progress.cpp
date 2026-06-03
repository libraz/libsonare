#include <cstring>
#include <string>

#include "sonare_wrap.h"
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
      return "PreChorus";
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

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsFunction()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  Napi::Function js_cb = info[2].As<Napi::Function>();

  // The C-ABI progress callback cannot hold a Napi reference (it is called
  // synchronously on the same thread, so the stack is still valid).
  struct ProgressCtx {
    Napi::Env env;
    Napi::Function cb;
  } ctx{env, js_cb};

  auto c_progress = [](float progress, const char* stage, void* user_data) {
    auto* c = static_cast<ProgressCtx*>(user_data);
    c->cb.Call({Napi::Number::New(c->env, static_cast<double>(progress)),
                Napi::String::New(c->env, stage != nullptr ? stage : "")});
  };

  char* json_str = nullptr;
  SonareError err =
      sonare_analyze_json_with_progress(data, length, sample_rate, c_progress, &ctx, &json_str);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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

  Napi::Value beats_val = result.Get("beats");
  if (beats_val.IsArray()) {
    Napi::Array beats_arr = beats_val.As<Napi::Array>();
    uint32_t n = beats_arr.Length();
    auto beat_times = Napi::Float32Array::New(env, n);
    for (uint32_t i = 0; i < n; ++i) {
      Napi::Value beat = beats_arr.Get(i);
      if (beat.IsObject()) {
        Napi::Value t = beat.As<Napi::Object>().Get("time");
        beat_times[i] =
            t.IsNumber() ? static_cast<float>(t.As<Napi::Number>().DoubleValue()) : 0.0f;
      }
    }
    result.Set("beatTimes", beat_times);
  } else {
    result.Set("beatTimes", Napi::Float32Array::New(env, 0));
  }

  return result;
}

Napi::Value SonareWrap::AnalyzeSections(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  const float min_section_sec =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 4.0f;

  SonareSectionResult result{};
  SonareError err = sonare_analyze_sections(typed.Data(), typed.ElementLength(), sample_rate, n_fft,
                                            hop_length, min_section_sec, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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
}

Napi::Value SonareWrap::AnalyzeMelody(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const float fmin =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 65.0f;
  const float fmax =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 2093.0f;
  const int frame_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 512;
  const float threshold =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.1f;
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
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
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
}
