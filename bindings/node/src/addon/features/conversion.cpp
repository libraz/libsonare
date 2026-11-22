#include <string>

#include "core/convert.h"
#include "features/common.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;
using namespace sonare_node::features;

// ============================================================================
// Core - Conversion
// ============================================================================

Napi::Value SonareWrap::HzToMel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "hz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = node_float_as_c_abi(info[0]);
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_mel(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "mel must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float mel = node_float_as_c_abi(info[0]);
  return Napi::Number::New(env, static_cast<double>(sonare::mel_to_hz(mel)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "hz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = node_float_as_c_abi(info[0]);
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_midi(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MidiToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "midi must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float midi = node_float_as_c_abi(info[0]);
  return Napi::Number::New(env, static_cast<double>(sonare::midi_to_hz(midi)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "hz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = node_float_as_c_abi(info[0]);
  return Napi::String::New(env, sonare::hz_to_note(hz));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "note must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  std::string note = node_narrow_string(env, info[0], "note");
  return Napi::Number::New(env, static_cast<double>(sonare::note_to_hz(note)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FramesToTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireNumberValue(env, info[0], "frames")) return env.Undefined();

  SONARE_NODE_TRY
  int frames = node_narrow_int(env, info[0], "frames");
  int sr{};
  if (!RequiredDomainArg(env, info, 1, "sampleRate", sonare::kFrameTimeSampleRateDomain, &sr)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!RequiredDomainArg(env, info, 2, "hopLength", sonare::kFrameTimeHopDomain, &hop_length)) {
    return env.Undefined();
  }

  return Napi::Number::New(env,
                           static_cast<double>(sonare::frames_to_time(frames, sr, hop_length)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireNumberValue(env, info[0], "time")) return env.Undefined();

  SONARE_NODE_TRY
  float time = node_narrow_float(env, info[0], "time");
  int sr{};
  if (!RequiredDomainArg(env, info, 1, "sampleRate", sonare::kFrameTimeSampleRateDomain, &sr)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!RequiredDomainArg(env, info, 2, "hopLength", sonare::kFrameTimeHopDomain, &hop_length)) {
    return env.Undefined();
  }

  return Napi::Number::New(env, sonare::time_to_frames(time, sr, hop_length));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FramesToSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireNumberValue(env, info[0], "frames")) return env.Undefined();
  int frames = node_narrow_int(env, info[0], "frames");
  int hop{};
  if (!RequiredIntArg(env, info, 1, "hopLength", &hop)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 0, &n_fft)) return env.Undefined();
  return Napi::Number::New(env, sonare_frames_to_samples(frames, hop, n_fft));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SamplesToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireNumberValue(env, info[0], "samples")) return env.Undefined();
  int samples = node_narrow_int(env, info[0], "samples");
  int hop{};
  if (!RequiredIntArg(env, info, 1, "hopLength", &hop)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 0, &n_fft)) return env.Undefined();
  return Napi::Number::New(env, sonare_samples_to_frames(samples, hop, n_fft));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PowerToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref{};
  if (!RequiredFloatArg(env, info, 1, "ref", &ref)) return env.Undefined();
  float amin{};
  if (!RequiredFloatArg(env, info, 2, "amin", &amin)) return env.Undefined();
  float top_db{};
  if (!RequiredDomainFloatArg(env, info, 3, "topDb", &top_db)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_power_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AmplitudeToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref{};
  if (!RequiredFloatArg(env, info, 1, "ref", &ref)) return env.Undefined();
  float amin{};
  if (!RequiredFloatArg(env, info, 2, "amin", &amin)) return env.Undefined();
  float top_db{};
  if (!RequiredDomainFloatArg(env, info, 3, "topDb", &top_db)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_amplitude_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DbToPower(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref{};
  if (!OptionalFloatArg(env, info, 1, "ref", 1.0f, &ref)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_power(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DbToAmplitude(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "values must be a Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref{};
  if (!OptionalFloatArg(env, info, 1, "ref", 1.0f, &ref)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_amplitude(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, count);
  SONARE_NODE_CATCH(env)
}
