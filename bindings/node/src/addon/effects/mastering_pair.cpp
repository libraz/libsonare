/// @file
/// @brief Node bindings for the reference-pair, stereo-analysis and advisory entry points.

#include <cstddef>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/streaming_preview.h"
#include "metering/basic.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

// The analysis entry points take an interleaved buffer so BS.1770 channel
// summing sees the program rather than a downmix; the JS surface keeps the
// planar left/right shape the rest of the stereo mastering API uses.
bool ReadStereoPair(const Napi::CallbackInfo& info, const char* usage,
                    std::vector<float>* interleaved, size_t* frames, int* sample_rate) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, usage).ThrowAsJavaScriptException();
    return false;
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return false;
  }
  *sample_rate = node_narrow_int(env, info[2], node_arg_label(2).c_str());
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(left.Data(), left.ElementLength(), *sample_rate);
  sonare::validate_offline_audio_input(right.Data(), right.ElementLength(), *sample_rate);

  *frames = left.ElementLength();
  interleaved->resize(*frames * 2);
  for (size_t index = 0; index < *frames; ++index) {
    (*interleaved)[2 * index] = left[index];
    (*interleaved)[2 * index + 1] = right[index];
  }
  return true;
}

// Builds an assistant config from a JS params object. `targetPlatform` is a
// delivery-target NAME on this surface: it is read here, validated against the
// shared table, and kept out of the numeric conversion. The index the C ABI
// carries is a transport detail for callers that cannot pass a string, so a
// number is rejected here rather than silently accepted as an index.
sonare::mastering::assistant::AssistantConfig AssistantConfigFromParams(
    const Napi::CallbackInfo& info, size_t index) {
  static const std::vector<std::string> kPlatformKeys = {"targetPlatform", "target_platform"};
  std::vector<sonare::mastering::api::Param> params;
  std::string platform;
  bool has_platform = false;
  if (info.Length() > index && info[index].IsObject()) {
    Napi::Object object = info[index].As<Napi::Object>();
    for (const std::string& key : kPlatformKeys) {
      if (!object.Has(key)) continue;
      Napi::Value platform_value = object.Get(key);
      if (!platform_value.IsString()) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "'" + key + "' must be one of the delivery-target names: " +
                                          sonare::mastering::assistant::platform_names_joined());
      }
      platform = platform_value.As<Napi::String>().Utf8Value();
      has_platform = true;
    }
    params = ParamsFromObject(object, kPlatformKeys);
  }
  sonare::mastering::assistant::AssistantConfig config =
      sonare::mastering::assistant::assistant_config_from_params(params.data(), params.size());
  if (has_platform) sonare::mastering::assistant::set_target_platform(config, platform);
  return config;
}

}  // namespace

Napi::Value SonareWrap::MasteringPairProcess(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (processorName, source, reference, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto source = info[1].As<Napi::Float32Array>();
  auto reference = info[2].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  // source and reference may have independent lengths, so each is validated at its
  // own length with the shared sample rate.
  const int sr = node_narrow_int(env, info[3], "sr");
  sonare::validate_offline_audio_input(source.Data(), source.ElementLength(), sr);
  sonare::validate_offline_audio_input(reference.Data(), reference.ElementLength(), sr);
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  // source and reference may have independent lengths; the match primitives
  // consume each buffer at its own length.
  auto result = sonare::mastering::api::apply_named_pair_processor(
      info[0].As<Napi::String>().Utf8Value(), source.Data(), reference.Data(),
      source.ElementLength(), reference.ElementLength(), sr, params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  out.Set("nonFiniteSubstitutionCount",
          Napi::Number::New(env, result.non_finite_substitution_count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAbMatchLoudness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (source, reference, sampleRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto source = info[0].As<Napi::Float32Array>();
  auto reference = info[1].As<Napi::Float32Array>();
  // Through the C ABI rather than the core call the pair processors use: the
  // entry point owns the input validation and the match scalars, so this surface
  // reports exactly what the other surfaces do.
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  float* matched = nullptr;
  size_t matched_length = 0;
  SonareLoudnessMatch match{};
  const SonareError err = sonare_mastering_ab_match_loudness(
      source.Data(), source.ElementLength(), reference.Data(), reference.ElementLength(), sr,
      &matched, &matched_length, &match);
  if (err != SONARE_OK) {
    sonare_free_floats(matched);
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Float32Array samples = Napi::Float32Array::New(env, matched_length);
  if (matched_length > 0 && matched != nullptr) {
    std::memcpy(samples.Data(), matched, matched_length * sizeof(float));
  }
  sonare_free_floats(matched);
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", samples);
  out.Set("sampleRate", Napi::Number::New(env, sr));
  out.Set("referenceLufs", Napi::Number::New(env, match.reference_lufs));
  out.Set("sourceLufs", Napi::Number::New(env, match.source_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, match.applied_gain_db));
  out.Set("matchedTruePeakDbtp", Napi::Number::New(env, match.matched_true_peak_dbtp));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPairAnalyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (analysisName, source, reference, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto source = info[1].As<Napi::Float32Array>();
  auto reference = info[2].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  // source and reference may have independent lengths, so each is validated at its
  // own length with the shared sample rate.
  const int sr = node_narrow_int(env, info[3], "sr");
  sonare::validate_offline_audio_input(source.Data(), source.ElementLength(), sr);
  sonare::validate_offline_audio_input(reference.Data(), reference.ElementLength(), sr);
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  // source and reference may have independent lengths.
  auto json = sonare::mastering::api::analyze_named_pair(
      info[0].As<Napi::String>().Utf8Value(), source.Data(), reference.Data(),
      source.ElementLength(), reference.ElementLength(), sr, params);
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStereoAnalyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (analysisName, left, right, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  const int sr = node_narrow_int(env, info[3], "sr");
  sonare::validate_offline_audio_input(left.Data(), left.ElementLength(), sr);
  sonare::validate_offline_audio_input(right.Data(), right.ElementLength(), sr);
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  auto json = sonare::mastering::api::analyze_named_stereo(info[0].As<Napi::String>().Utf8Value(),
                                                           left.Data(), right.Data(),
                                                           left.ElementLength(), sr, params);
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAssistantSuggest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(samples.Data(), samples.ElementLength(),
                                       node_narrow_int(env, info[1], node_arg_label(1).c_str()));
  const sonare::mastering::assistant::AssistantConfig config = AssistantConfigFromParams(info, 2);
  const auto result = sonare::mastering::assistant::suggest_chain(
      samples.Data(), samples.ElementLength(),
      node_narrow_int(env, info[1], node_arg_label(1).c_str()), config);
  return Napi::String::New(env, sonare::mastering::assistant::assistant_result_to_json(result));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAudioProfile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(samples.Data(), samples.ElementLength(),
                                       node_narrow_int(env, info[1], node_arg_label(1).c_str()));
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 3 && info[2].IsObject())
    params = ParamsFromObject(info[2].As<Napi::Object>());
  const sonare::mastering::assistant::AudioProfileConfig config =
      sonare::mastering::assistant::audio_profile_config_from_params(params.data(), params.size());
  const auto profile = sonare::mastering::assistant::analyze_audio_profile(
      samples.Data(), samples.ElementLength(),
      node_narrow_int(env, info[1], node_arg_label(1).c_str()), config);
  return Napi::String::New(env, sonare::mastering::assistant::audio_profile_to_json(profile));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStreamingPreview(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, platforms?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(samples.Data(), samples.ElementLength(),
                                       node_narrow_int(env, info[1], node_arg_label(1).c_str()));
  std::vector<sonare::mastering::maximizer::StreamingPlatform> platforms;
  if (info.Length() >= 3 && info[2].IsArray()) {
    Napi::Array input = info[2].As<Napi::Array>();
    platforms.reserve(input.Length());
    for (uint32_t index = 0; index < input.Length(); ++index) {
      Napi::Value value = input.Get(index);
      if (!value.IsObject()) {
        Napi::TypeError::New(env, "platforms entries must be objects").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object object = value.As<Napi::Object>();
      if (!object.Get("name").IsString() || !object.Get("targetLufs").IsNumber() ||
          !object.Get("ceilingDb").IsNumber()) {
        Napi::TypeError::New(env, "platforms entries require name, targetLufs, ceilingDb")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      platforms.push_back({object.Get("name").As<Napi::String>().Utf8Value(),
                           node_narrow_finite_float(env, object.Get("targetLufs"), "targetLufs"),
                           node_narrow_finite_float(env, object.Get("ceilingDb"), "ceilingDb")});
    }
  }
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.Data(), samples.ElementLength(),
                                 node_narrow_int(env, info[1], node_arg_label(1).c_str()));
  const auto results = platforms.empty()
                           ? sonare::mastering::maximizer::streaming_preview(audio)
                           : sonare::mastering::maximizer::streaming_preview(audio, platforms);
  return Napi::String::New(env, sonare::mastering::maximizer::streaming_preview_to_json(results));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAssistantSuggestStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<float> interleaved;
  size_t frames = 0;
  int sample_rate = 0;
  if (!ReadStereoPair(info, "Expected (left, right, sampleRate, params?)", &interleaved, &frames,
                      &sample_rate)) {
    return env.Undefined();
  }
  const sonare::mastering::assistant::AssistantConfig config = AssistantConfigFromParams(info, 3);
  const auto result = sonare::mastering::assistant::suggest_chain_interleaved(
      interleaved.data(), frames, 2, sample_rate, config);
  return Napi::String::New(env, sonare::mastering::assistant::assistant_result_to_json(result));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAudioProfileStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<float> interleaved;
  size_t frames = 0;
  int sample_rate = 0;
  if (!ReadStereoPair(info, "Expected (left, right, sampleRate, params?)", &interleaved, &frames,
                      &sample_rate)) {
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 4 && info[3].IsObject())
    params = ParamsFromObject(info[3].As<Napi::Object>());
  const sonare::mastering::assistant::AudioProfileConfig config =
      sonare::mastering::assistant::audio_profile_config_from_params(params.data(), params.size());
  const auto profile = sonare::mastering::assistant::analyze_audio_profile_interleaved(
      interleaved.data(), frames, 2, sample_rate, config);
  return Napi::String::New(env, sonare::mastering::assistant::audio_profile_to_json(profile));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStreamingPreviewStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<float> interleaved;
  size_t frames = 0;
  int sample_rate = 0;
  if (!ReadStereoPair(info, "Expected (left, right, sampleRate, platforms?)", &interleaved, &frames,
                      &sample_rate)) {
    return env.Undefined();
  }
  std::vector<sonare::mastering::maximizer::StreamingPlatform> platforms;
  if (info.Length() >= 4 && info[3].IsArray()) {
    Napi::Array input = info[3].As<Napi::Array>();
    platforms.reserve(input.Length());
    for (uint32_t index = 0; index < input.Length(); ++index) {
      Napi::Value value = input.Get(index);
      if (!value.IsObject()) {
        Napi::TypeError::New(env, "platforms entries must be objects").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object object = value.As<Napi::Object>();
      if (!object.Get("name").IsString() || !object.Get("targetLufs").IsNumber() ||
          !object.Get("ceilingDb").IsNumber()) {
        Napi::TypeError::New(env, "platforms entries require name, targetLufs, ceilingDb")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      platforms.push_back({object.Get("name").As<Napi::String>().Utf8Value(),
                           node_narrow_finite_float(env, object.Get("targetLufs"), "targetLufs"),
                           node_narrow_finite_float(env, object.Get("ceilingDb"), "ceilingDb")});
    }
  }
  const auto results = platforms.empty()
                           ? sonare::mastering::maximizer::streaming_preview_interleaved(
                                 interleaved.data(), frames, 2, sample_rate)
                           : sonare::mastering::maximizer::streaming_preview_interleaved(
                                 interleaved.data(), frames, 2, sample_rate, platforms);
  return Napi::String::New(env, sonare::mastering::maximizer::streaming_preview_to_json(results));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MeteringCrestFactorDbStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::vector<float> interleaved;
  size_t frames = 0;
  int sample_rate = 0;
  if (!ReadStereoPair(info, "Expected (left, right, sampleRate)", &interleaved, &frames,
                      &sample_rate)) {
    return env.Undefined();
  }
  return Napi::Number::New(
      env, sonare::metering::crest_factor_db_interleaved(interleaved.data(), frames, 2));
  SONARE_NODE_CATCH(env)
}
