#include "sonare_wrap_utils.h"

#include <cstring>
#include <string>

namespace sonare_node {

const char* ErrorMessageForCode(SonareError err) {
  const char* detail = sonare_last_error_message();
  if (detail != nullptr && detail[0] != '\0') {
    return detail;
  }
  return sonare_error_message(err);
}

bool IsFloat32Array(const Napi::Value& value) {
  return value.IsTypedArray() &&
         value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array;
}

bool IsUint8Array(const Napi::Value& value) {
  return value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_uint8_array;
}

const char* PitchClassNameLocal(SonarePitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int idx = static_cast<int>(pc);
  if (idx < 0 || idx > 11) return "C";
  return names[idx];
}

const char* ModeNameLocal(SonareMode mode) { return mode == SONARE_MODE_MAJOR ? "major" : "minor"; }

const char* ChordQualityName(SonareChordQuality quality) {
  switch (quality) {
    case SONARE_CHORD_MAJOR:
      return "major";
    case SONARE_CHORD_MINOR:
      return "minor";
    case SONARE_CHORD_DIMINISHED:
      return "diminished";
    case SONARE_CHORD_AUGMENTED:
      return "augmented";
    case SONARE_CHORD_DOMINANT7:
      return "dominant7";
    case SONARE_CHORD_MAJOR7:
      return "major7";
    case SONARE_CHORD_MINOR7:
      return "minor7";
    case SONARE_CHORD_SUS2:
      return "sus2";
    case SONARE_CHORD_SUS4:
      return "sus4";
    case SONARE_CHORD_UNKNOWN:
      return "unknown";
  }
  return "unknown";
}

Napi::Object KeyToObject(Napi::Env env, SonarePitchClass root, SonareMode mode, float confidence) {
  Napi::Object key = Napi::Object::New(env);
  std::string root_name = PitchClassNameLocal(root);
  std::string mode_name = ModeNameLocal(mode);
  key.Set("root", Napi::String::New(env, root_name));
  key.Set("mode", Napi::String::New(env, mode_name));
  key.Set("confidence", Napi::Number::New(env, static_cast<double>(confidence)));
  key.Set("name", Napi::String::New(env, root_name + " " + mode_name));
  key.Set("shortName", Napi::String::New(env, root_name + (mode == SONARE_MODE_MAJOR ? "" : "m")));
  return key;
}

Napi::Object AnalysisToObject(Napi::Env env, const SonareAnalysisResult& analysis) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, static_cast<double>(analysis.bpm)));
  result.Set("bpmConfidence", Napi::Number::New(env, static_cast<double>(analysis.bpm_confidence)));
  result.Set("key",
             KeyToObject(env, analysis.key.root, analysis.key.mode, analysis.key.confidence));

  Napi::Object ts = Napi::Object::New(env);
  ts.Set("numerator", Napi::Number::New(env, analysis.time_signature.numerator));
  ts.Set("denominator", Napi::Number::New(env, analysis.time_signature.denominator));
  ts.Set("confidence",
         Napi::Number::New(env, static_cast<double>(analysis.time_signature.confidence)));
  result.Set("timeSignature", ts);

  auto beat_times = Napi::Float32Array::New(env, analysis.beat_count);
  Napi::Array beats = Napi::Array::New(env, analysis.beat_count);
  if (analysis.beat_count > 0 && analysis.beat_times != nullptr) {
    std::memcpy(beat_times.Data(), analysis.beat_times, analysis.beat_count * sizeof(float));
    for (size_t i = 0; i < analysis.beat_count; ++i) {
      Napi::Object beat = Napi::Object::New(env);
      beat.Set("time", Napi::Number::New(env, static_cast<double>(analysis.beat_times[i])));
      beat.Set("strength", env.Undefined());
      beats.Set(static_cast<uint32_t>(i), beat);
    }
  }
  result.Set("beatTimes", beat_times);
  result.Set("beats", beats);
  return result;
}

}  // namespace sonare_node
