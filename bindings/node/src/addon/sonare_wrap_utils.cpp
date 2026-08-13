#include "sonare_wrap_utils.h"

#include <cstring>
#include <string>

#include "c_api/sonare_c_error_mapping.h"

namespace sonare_node {

const char* ErrorMessageForCode(SonareError err) {
  const char* detail = sonare_last_error_message();
  if (detail != nullptr && detail[0] != '\0') {
    return detail;
  }
  return sonare_error_message(err);
}

const char* ErrorCodeName(SonareError err) {
  switch (err) {
    case SONARE_OK:
      return "Ok";
    case SONARE_ERROR_FILE_NOT_FOUND:
      return "FileNotFound";
    case SONARE_ERROR_INVALID_FORMAT:
      return "InvalidFormat";
    case SONARE_ERROR_DECODE_FAILED:
      return "DecodeFailed";
    case SONARE_ERROR_INVALID_PARAMETER:
      return "InvalidParameter";
    case SONARE_ERROR_OUT_OF_MEMORY:
      return "OutOfMemory";
    case SONARE_ERROR_NOT_SUPPORTED:
      return "NotSupported";
    case SONARE_ERROR_INVALID_STATE:
      return "InvalidState";
    case SONARE_ERROR_CANCELLED:
      return "Cancelled";
    case SONARE_ERROR_UNKNOWN:
    default:
      return "Unknown";
  }
}

SonareError CErrorFromException(const sonare::SonareException& e) {
  switch (e.code()) {
    case sonare::ErrorCode::FileNotFound:
      return SONARE_ERROR_FILE_NOT_FOUND;
    case sonare::ErrorCode::InvalidFormat:
      return SONARE_ERROR_INVALID_FORMAT;
    case sonare::ErrorCode::DecodeFailed:
      return SONARE_ERROR_DECODE_FAILED;
    case sonare::ErrorCode::InvalidParameter:
      return SONARE_ERROR_INVALID_PARAMETER;
    case sonare::ErrorCode::OutOfMemory:
      return SONARE_ERROR_OUT_OF_MEMORY;
    case sonare::ErrorCode::NotImplemented:
      return SONARE_ERROR_NOT_SUPPORTED;
    case sonare::ErrorCode::InvalidState:
      return SONARE_ERROR_INVALID_STATE;
    case sonare::ErrorCode::Cancelled:
      return SONARE_ERROR_CANCELLED;
    case sonare::ErrorCode::Ok:
    default:
      return SONARE_ERROR_UNKNOWN;
  }
}

sonare::ErrorCode CodeFromCError(SonareError err) {
  return sonare_c_detail::error_code_from_c_error(err);
}

namespace {

void SetSonareErrorProperties(Napi::Env env, Napi::Object error, SonareError err) {
  error.Set("name", Napi::String::New(env, "SonareError"));
  error.Set("code", Napi::Number::New(env, static_cast<double>(static_cast<int>(err))));
  error.Set("codeName", Napi::String::New(env, ErrorCodeName(err)));
}

void ThrowWithCode(Napi::Env env, SonareError err, const std::string& message) {
  Napi::Error error = Napi::Error::New(env, message);
  SetSonareErrorProperties(env, error.Value(), err);
  error.ThrowAsJavaScriptException();
}

}  // namespace

void ThrowSonareError(Napi::Env env, SonareError err, const std::string& prefix) {
  ThrowWithCode(env, err, prefix + ErrorMessageForCode(err));
}

void ThrowSonareErrorMessage(Napi::Env env, SonareError err, const std::string& message) {
  ThrowWithCode(env, err, message);
}

void DecorateSonareError(Napi::Env env, Napi::Object error, SonareError err) {
  SetSonareErrorProperties(env, error, err);
}

bool ReadMusicAnalyzeOptions(const Napi::Value& value, SonareMusicAnalyzeOptions* options) {
  if (options == nullptr) return false;
  *options = sonare_music_analyze_options_default();
  if (!value.IsObject()) return false;

  const Napi::Object object = value.As<Napi::Object>();
  auto integer = [&](const char* key, int* field) {
    const Napi::Value item = object.Get(key);
    if (item.IsNumber()) *field = item.As<Napi::Number>().Int32Value();
  };
  auto number = [&](const char* key, float* field) {
    const Napi::Value item = object.Get(key);
    if (item.IsNumber()) *field = item.As<Napi::Number>().FloatValue();
  };
  auto boolean = [&](const char* key, int* field) {
    const Napi::Value item = object.Get(key);
    if (item.IsBoolean()) *field = item.As<Napi::Boolean>().Value() ? 1 : 0;
  };
  integer("nFft", &options->n_fft);
  integer("hopLength", &options->hop_length);
  number("bpmMin", &options->bpm_min);
  number("bpmMax", &options->bpm_max);
  number("startBpm", &options->start_bpm);
  boolean("useTriadsOnly", &options->use_triads_only);
  boolean("useHpss", &options->use_hpss);
  number("chromaHighpassHz", &options->chroma_highpass_hz);
  boolean("useBassWeighted", &options->use_bass_weighted);
  integer("chromaHopMultiplier", &options->chroma_hop_multiplier);
  boolean("useChordHmm", &options->use_chord_hmm);
  boolean("useChordKeyContext", &options->use_chord_key_context);
  integer("chordHmmBeamWidth", &options->chord_hmm_beam_width);
  boolean("detectChordInversions", &options->detect_chord_inversions);
  return true;
}

bool IsFloat32Array(const Napi::Value& value) {
  return value.IsTypedArray() &&
         value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array;
}

bool IsUint8Array(const Napi::Value& value) {
  return value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_uint8_array;
}

bool IsInt32Array(const Napi::Value& value) {
  return value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array;
}

bool ValidateMatrixDims(Napi::Env env, const char* fn_name, int rows, int cols, size_t length) {
  if (rows < 0 || cols < 0) {
    Napi::RangeError::New(env, std::string(fn_name) + ": dimensions must be non-negative")
        .ThrowAsJavaScriptException();
    return false;
  }
  const size_t expected = static_cast<size_t>(rows) * static_cast<size_t>(cols);
  if (expected != length) {
    Napi::RangeError::New(env, std::string(fn_name) + ": rows*cols (" + std::to_string(expected) +
                                   ") must equal input length (" + std::to_string(length) + ")")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

const char* PitchClassNameLocal(SonarePitchClass pc) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int idx = static_cast<int>(pc);
  if (idx < 0 || idx > 11) return "C";
  return names[idx];
}

const char* ModeNameLocal(SonareMode mode) {
  switch (mode) {
    case SONARE_MODE_MAJOR:
      return "major";
    case SONARE_MODE_MINOR:
      return "minor";
    case SONARE_MODE_DORIAN:
      return "dorian";
    case SONARE_MODE_PHRYGIAN:
      return "phrygian";
    case SONARE_MODE_LYDIAN:
      return "lydian";
    case SONARE_MODE_MIXOLYDIAN:
      return "mixolydian";
    case SONARE_MODE_LOCRIAN:
      return "locrian";
    default:
      return "unknown";
  }
}

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
    case SONARE_CHORD_ADD9:
      return "add9";
    case SONARE_CHORD_MINOR_ADD9:
      return "minorAdd9";
    case SONARE_CHORD_DIM7:
      return "dim7";
    case SONARE_CHORD_HALF_DIM7:
      return "halfDim7";
    case SONARE_CHORD_MAJOR9:
      return "major9";
    case SONARE_CHORD_DOMINANT9:
      return "dominant9";
    case SONARE_CHORD_SUS2_ADD4:
      return "sus2Add4";
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
  const std::string short_name =
      mode == SONARE_MODE_MAJOR
          ? root_name
          : (mode == SONARE_MODE_MINOR ? root_name + "m" : root_name + " " + mode_name);
  key.Set("shortName", Napi::String::New(env, short_name));
  return key;
}

std::vector<sonare::mastering::api::Param> ParamsFromObject(const Napi::Object& object) {
  std::vector<sonare::mastering::api::Param> params;
  Napi::Env env = object.Env();
  Napi::Array names = object.GetPropertyNames();
  for (uint32_t index = 0; index < names.Length(); ++index) {
    Napi::Value key_value = names.Get(index);
    Napi::Value value = object.Get(key_value);
    const std::string key = key_value.As<Napi::String>().Utf8Value();
    if (value.IsNumber()) {
      params.push_back({key, value.As<Napi::Number>().DoubleValue()});
    } else if (value.IsBoolean()) {
      params.push_back({key, value.As<Napi::Boolean>().Value() ? 1.0 : 0.0});
    } else {
      Napi::TypeError::New(
          env, "Parameter '" + key + "' must be a number or boolean (got an unsupported type)")
          .ThrowAsJavaScriptException();
      return params;
    }
  }
  return params;
}

Napi::Object AnalysisToObject(Napi::Env env, const SonareAnalysisResult& analysis) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, static_cast<double>(analysis.bpm)));
  result.Set("bpmConfidence", Napi::Number::New(env, static_cast<double>(analysis.bpm_confidence)));
  Napi::Array bpm_candidates = Napi::Array::New(env, analysis.bpm_candidate_count);
  for (size_t i = 0; i < analysis.bpm_candidate_count; ++i) {
    Napi::Object candidate = Napi::Object::New(env);
    candidate.Set("value", Napi::Number::New(env, analysis.bpm_candidates[i].value));
    candidate.Set("confidence", Napi::Number::New(env, analysis.bpm_candidates[i].confidence));
    static constexpr const char* kRelations[] = {"primary", "half", "double", "other"};
    const int relation = analysis.bpm_candidates[i].relation;
    candidate.Set(
        "relation",
        Napi::String::New(env, relation >= 0 && relation < 4 ? kRelations[relation] : "other"));
    bpm_candidates.Set(static_cast<uint32_t>(i), candidate);
  }
  result.Set("bpmCandidates", bpm_candidates);
  result.Set("key",
             KeyToObject(env, analysis.key.root, analysis.key.mode, analysis.key.confidence));

  Napi::Object ts = Napi::Object::New(env);
  ts.Set("numerator", Napi::Number::New(env, analysis.time_signature.numerator));
  ts.Set("denominator", Napi::Number::New(env, analysis.time_signature.denominator));
  ts.Set("confidence",
         Napi::Number::New(env, static_cast<double>(analysis.time_signature.confidence)));
  result.Set("timeSignature", ts);
  Napi::Array time_signature_candidates =
      Napi::Array::New(env, analysis.time_signature_candidate_count);
  for (size_t i = 0; i < analysis.time_signature_candidate_count; ++i) {
    const SonareTimeSignature& candidate = analysis.time_signature_candidates[i];
    Napi::Object value = Napi::Object::New(env);
    value.Set("numerator", Napi::Number::New(env, candidate.numerator));
    value.Set("denominator", Napi::Number::New(env, candidate.denominator));
    value.Set("confidence", Napi::Number::New(env, candidate.confidence));
    time_signature_candidates.Set(static_cast<uint32_t>(i), value);
  }
  result.Set("timeSignatureCandidates", time_signature_candidates);

  auto beat_times = Napi::Float32Array::New(env, analysis.beat_count);
  Napi::Array beats = Napi::Array::New(env, analysis.beat_count);
  if (analysis.beat_count > 0 && analysis.beat_times != nullptr) {
    std::memcpy(beat_times.Data(), analysis.beat_times, analysis.beat_count * sizeof(float));
    for (size_t i = 0; i < analysis.beat_count; ++i) {
      Napi::Object beat = Napi::Object::New(env);
      beat.Set("time", Napi::Number::New(env, static_cast<double>(analysis.beat_times[i])));
      beats.Set(static_cast<uint32_t>(i), beat);
    }
  }
  result.Set("beatTimes", beat_times);
  result.Set("beats", beats);
  return result;
}

bool EnrichFullAnalysisObject(Napi::Env env, Napi::Object result, Napi::Error* error) {
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

  Napi::Value key_val = result.Get("key");
  if (key_val.IsObject()) {
    Napi::Object key_obj = key_val.As<Napi::Object>();
    Napi::Value root = key_obj.Get("root");
    Napi::Value mode = key_obj.Get("mode");
    Napi::Value confidence = key_obj.Get("confidence");
    if (root.IsNumber() && mode.IsNumber()) {
      result.Set(
          "key",
          KeyToObject(env, static_cast<SonarePitchClass>(root.As<Napi::Number>().Int32Value()),
                      static_cast<SonareMode>(mode.As<Napi::Number>().Int32Value()),
                      confidence.IsNumber()
                          ? static_cast<float>(confidence.As<Napi::Number>().DoubleValue())
                          : 0.0f));
    }
  }

  if (env.IsExceptionPending()) {
    if (error != nullptr) *error = env.GetAndClearPendingException();
    return false;
  }
  return true;
}

Napi::Value FullAnalysisJsonToObject(Napi::Env env, const float* data, size_t length,
                                     int sample_rate, const SonareMusicAnalyzeOptions* options) {
  char* json_str = nullptr;
  SonareError err = options ? sonare_analyze_json_ex(data, length, sample_rate, options, &json_str)
                            : sonare_analyze_json(data, length, sample_rate, &json_str);
  if (err != SONARE_OK) {
    // A C-ABI failure must carry the SonareError code so the synchronous and
    // async analyze paths classify the same input identically.
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  // Parse the JSON on the main thread using the JS engine's built-in parser.
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
  if (!EnrichFullAnalysisObject(env, result, &enrich_error)) {
    enrich_error.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return result;
}

}  // namespace sonare_node
