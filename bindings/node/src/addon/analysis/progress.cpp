#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
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
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  Napi::Function js_cb = info[2].As<Napi::Function>();

  try {
    // analyze() is synchronous, so js_cb (referenced by info[2]) outlives the call.
    sonare::Audio audio =
        sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate);
    sonare::MusicAnalyzer analyzer(audio);
    analyzer.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
      js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
    });
    sonare::AnalysisResult analysis = analyzer.analyze();

    // Reuse the C-ABI result shape so the output matches analyze().
    std::vector<float> beat_times;
    beat_times.reserve(analysis.beats.size());
    for (const auto& beat : analysis.beats) {
      beat_times.push_back(beat.time);
    }

    SonareAnalysisResult c_result{};
    c_result.bpm = analysis.bpm;
    c_result.bpm_confidence = analysis.bpm_confidence;
    c_result.key.root = static_cast<SonarePitchClass>(static_cast<int>(analysis.key.root));
    c_result.key.mode = static_cast<SonareMode>(static_cast<int>(analysis.key.mode));
    c_result.key.confidence = analysis.key.confidence;
    c_result.time_signature.numerator = analysis.time_signature.numerator;
    c_result.time_signature.denominator = analysis.time_signature.denominator;
    c_result.time_signature.confidence = analysis.time_signature.confidence;
    c_result.beat_times = beat_times.empty() ? nullptr : beat_times.data();
    c_result.beat_count = beat_times.size();

    return AnalysisToObject(env, c_result);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
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

  SonareMelodyResult result{};
  SonareError err = sonare_analyze_melody(typed.Data(), typed.ElementLength(), sample_rate, fmin,
                                          fmax, frame_length, hop_length, threshold, &result);
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
