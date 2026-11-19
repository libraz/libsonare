#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/parameter_checks.h"

using namespace sonare_node;

Napi::Value SonareWrap::Hpss(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  HpssArguments args;
  if (!ReadHpssArguments(env, info, &args)) return env.Undefined();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);

  sonare::HpssConfig config;
  config.kernel_size_harmonic = args.kernel_harmonic;
  config.kernel_size_percussive = args.kernel_percussive;
  config.use_soft_mask = !args.hard_mask;

  sonare::StftConfig stft_config;
  stft_config.n_fft = args.n_fft;
  stft_config.hop_length = args.hop_length;

  sonare::HpssAudioResult result = sonare::hpss(audio, config, stft_config);

  Napi::Object out = Napi::Object::New(env);

  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  out.Set("harmonic", VecToFloat32(env, harmonic_vec));

  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  out.Set("percussive", VecToFloat32(env, percussive_vec));

  out.Set("sampleRate", Napi::Number::New(env, result.harmonic.sample_rate()));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Harmonic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Percussive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "rate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float rate = node_narrow_finite_float(env, info[2], "rate");
  int n_fft{};
  if (!OptionalIntArg(env, info, 3, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  // An absent hop stays 0, which the core runs as a quarter of n_fft.
  if (!OptionalIntArg(env, info, 4, "hopLength", 0, &hop_length)) return env.Undefined();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::TimeStretchConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = sonare::StretchBackend::NativeSpectral;
  sonare::Audio result = sonare::time_stretch(audio, rate, config);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchShift(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "semitones must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float semitones = node_narrow_finite_float(env, info[2], "semitones");
  int n_fft{};
  if (!OptionalIntArg(env, info, 3, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  // An absent hop stays 0, which the core runs as a quarter of n_fft.
  if (!OptionalIntArg(env, info, 4, "hopLength", 0, &hop_length)) return env.Undefined();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::PitchShiftPlan plan;
  if (!sonare::make_pitch_shift_plan(length, sr, semitones, &plan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unsupported pitch-shift expansion");
  }
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::PitchShiftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.backend = sonare::StretchBackend::NativeSpectral;
  sonare::Audio result = sonare::pitch_shift(audio, semitones, config);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchCorrectToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "currentMidi must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "targetMidi must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float current_midi = node_narrow_finite_float(env, info[2], "currentMidi");
  float target_midi = node_narrow_finite_float(env, info[3], "targetMidi");

  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::PitchCorrector corrector;
  sonare::Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchCorrectToMidiTimevarying(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, targetMidi, hopLength, voiced?, voicedProb?)
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "targetMidi must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsNumber()) {
    Napi::TypeError::New(env, "hopLength must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  auto f0 = info[2].As<Napi::Float32Array>();
  float target_midi = node_narrow_finite_float(env, info[3], "targetMidi");
  int hop_length = node_narrow_int(env, info[4], "hopLength");
  const size_t n_frames = f0.ElementLength();

  sonare::editing::pitch_editor::F0Track track;
  track.sample_rate = sr;
  track.hop_length = hop_length;
  track.f0_hz.assign(f0.Data(), f0.Data() + n_frames);
  track.voiced.resize(n_frames);
  track.voiced_prob.resize(n_frames);

  const bool has_voiced = info.Length() > 5 && IsInt32Array(info[5]);
  // An explicit voiced track owns the decision and makes voicedProb irrelevant;
  // leave the ignored array untouched so malformed values and lengths cannot
  // leak into the core validator.
  const bool has_prob = !has_voiced && info.Length() > 6 && IsFloat32Array(info[6]);
  Napi::Int32Array voiced_arr;
  Napi::Float32Array prob_arr;
  if (has_voiced) voiced_arr = info[5].As<Napi::Int32Array>();
  if (has_prob) prob_arr = info[6].As<Napi::Float32Array>();
  if ((has_voiced && voiced_arr.ElementLength() != n_frames) ||
      (has_prob && prob_arr.ElementLength() != n_frames)) {
    Napi::RangeError::New(env, "voiced and voicedProb must match f0Hz length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  for (size_t i = 0; i < n_frames; ++i) {
    // voiced wins when present; absent that, voicedProb decides (>= 0.5 is
    // voiced, matching sonare_note_segments' own voiced_threshold default);
    // with neither, every frame defaults to voiced. Matches the C ABI's
    // sonare_pitch_correct_to_midi_timevarying, which this direct core call
    // otherwise bypasses.
    const bool is_voiced =
        has_voiced ? (voiced_arr[i] != 0) : (has_prob ? (prob_arr[i] >= 0.5f) : true);
    track.voiced[i] = is_voiced;
    track.voiced_prob[i] = has_prob ? prob_arr[i] : (is_voiced ? 1.0f : 0.0f);
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::PitchCorrector corrector;
  sonare::Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchCorrectTimevarying(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, hopLength, options?)
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "hopLength must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  int hop_length = node_narrow_int(env, info[3], "hopLength");

  SonarePitchCorrectionConfig config{};
  sonare_pitch_correction_config_default(&config);
  std::vector<int32_t> voiced;
  std::vector<float> voiced_prob;
  const int32_t* voiced_ptr = nullptr;
  const float* prob_ptr = nullptr;
  if (info.Length() > 4 && info[4].IsObject()) {
    Napi::Object opts = info[4].As<Napi::Object>();
    sonare_node::RefuseUnknownKeys(
        opts, "options",
        {"targetMode", "targetMidi", "scaleRoot", "scaleModeMask", "scaleReferenceMidi",
         "retuneAmount", "maxCorrectionSemitones", "retuneSpeedMs", "vibratoThresholdCents",
         "voiced", "voicedProb"});
    config.target_mode = sonare_node::NamedSelectorProperty(
        opts, "targetMode", std::array<const char*, 2>{"midi", "scale"}, config.target_mode);
    config.target_midi = sonare_node::FloatProperty(opts, "targetMidi", config.target_midi);
    config.scale_root = sonare_node::IntProperty(opts, "scaleRoot", config.scale_root);
    const Napi::Value mask_value = opts.Get("scaleModeMask");
    if (mask_value.IsString()) {
      uint16_t named_mask = 0;
      const SonareError mask_err =
          sonare_scale_mask_for_mode(0, node_mode_from_value(mask_value), &named_mask);
      if (mask_err != SONARE_OK) {
        sonare_node::ThrowSonareError(env, mask_err);
        return env.Undefined();
      }
      config.scale_mode_mask = named_mask;
    } else {
      config.scale_mode_mask =
          sonare_node::Uint32Property(opts, "scaleModeMask", config.scale_mode_mask);
    }
    config.scale_reference_midi =
        sonare_node::FloatProperty(opts, "scaleReferenceMidi", config.scale_reference_midi);
    config.retune_amount = sonare_node::FloatProperty(opts, "retuneAmount", config.retune_amount);
    config.max_correction_semitones =
        sonare_node::FloatProperty(opts, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms =
        sonare_node::FloatProperty(opts, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents =
        sonare_node::FloatProperty(opts, "vibratoThresholdCents", config.vibrato_threshold_cents);
    if (opts.Has("voiced") && IsInt32Array(opts.Get("voiced"))) {
      auto arr = opts.Get("voiced").As<Napi::Int32Array>();
      if (arr.ElementLength() != n_frames) {
        Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      voiced.assign(arr.Data(), arr.Data() + arr.ElementLength());
      voiced_ptr = voiced.data();
    }
    if (!voiced_ptr && opts.Has("voicedProb") && IsFloat32Array(opts.Get("voicedProb"))) {
      auto arr = opts.Get("voicedProb").As<Napi::Float32Array>();
      if (arr.ElementLength() != n_frames) {
        Napi::RangeError::New(env, "voicedProb must match f0Hz length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      voiced_prob.assign(arr.Data(), arr.Data() + arr.ElementLength());
      prob_ptr = voiced_prob.data();
    }
  }

  float* out = nullptr;
  size_t out_length = 0;
  SonareError err =
      sonare_pitch_correct_timevarying(data, length, sr, f0.Data(), prob_ptr, voiced_ptr, n_frames,
                                       hop_length, &config, &out, &out_length);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, out_length);
  if (out_length > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, out_length * sizeof(float));
    sonare_free_floats(out);
  }
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AutoTune(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, key | null, options?)
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!(info[2].IsNull() || info[2].IsUndefined() || info[2].IsObject())) {
    Napi::TypeError::New(env, "key must be an object or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");

  SonareKey key{};
  const SonareKey* key_ptr = nullptr;
  if (info[2].IsObject()) {
    const Napi::Object key_object = info[2].As<Napi::Object>();
    key.root = PitchClassFromValue(key_object.Get("root"), "key.root");
    key.mode = node_mode_from_value(key_object.Get("mode"));
    key.confidence = 1.0f;
    key_ptr = &key;
  }

  SonarePitchCorrectionConfig config{};
  sonare_pitch_correction_config_default(&config);
  if (info.Length() > 3 && info[3].IsObject()) {
    const Napi::Object opts = info[3].As<Napi::Object>();
    sonare_node::RefuseUnknownKeys(opts, "options",
                                   {"strength", "scaleReferenceMidi", "maxCorrectionSemitones",
                                    "retuneSpeedMs", "vibratoThresholdCents"});
    config.retune_amount = sonare_node::FloatProperty(opts, "strength", config.retune_amount);
    config.scale_reference_midi =
        sonare_node::FloatProperty(opts, "scaleReferenceMidi", config.scale_reference_midi);
    config.max_correction_semitones =
        sonare_node::FloatProperty(opts, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms =
        sonare_node::FloatProperty(opts, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents =
        sonare_node::FloatProperty(opts, "vibratoThresholdCents", config.vibrato_threshold_cents);
  }

  float* out = nullptr;
  size_t out_length = 0;
  SonareKey used{};
  const SonareError err = sonare_auto_tune(typed.Data(), typed.ElementLength(), sr, key_ptr,
                                           &config, &out, &out_length, &used);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  auto samples = Napi::Float32Array::New(env, out_length);
  if (out_length > 0 && out != nullptr) {
    std::memcpy(samples.Data(), out, out_length * sizeof(float));
    sonare_free_floats(out);
  }
  Napi::Object result = Napi::Object::New(env);
  result.Set("samples", samples);
  result.Set("key", KeyToObject(env, used.root, used.mode, used.confidence));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "onsetSample must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "offsetSample must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsNumber()) {
    Napi::TypeError::New(env, "stretchRatio must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  int onset_sample = node_narrow_int(env, info[2], "onsetSample");
  int offset_sample = node_narrow_int(env, info[3], "offsetSample");
  float stretch_ratio = node_narrow_finite_float(env, info[4], "stretchRatio");

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  sonare::editing::pitch_editor::NoteEditor editor;
  sonare::Audio result = editor.stretch_note(audio, region, stretch_ratio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteMove(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "onsetSample must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "offsetSample must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsNumber()) {
    Napi::TypeError::New(env, "targetOnsetSample must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::NoteRegion region;
  region.onset_sample = node_narrow_int(env, info[2], "onsetSample");
  region.offset_sample = node_narrow_int(env, info[3], "offsetSample");
  sonare::editing::pitch_editor::NoteEditor editor;
  sonare::Audio result =
      editor.move_note(audio, region, node_narrow_int(env, info[4], "targetOnsetSample"));
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VoiceChange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "pitchSemitones must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "formantFactor must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float pitch_semitones = node_narrow_finite_float(env, info[2], "pitchSemitones");
  float formant_factor = node_narrow_finite_float(env, info[3], "formantFactor");
  std::string formant_mode;
  if (!RequiredStringValue(env, info[4], "formantMode", &formant_mode)) {
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  config.formant_mode = sonare::editing::voice_changer::parse_formant_mode(formant_mode);
  const sonare::Audio result = sonare::util::check::with_camel_paths([&] {
    sonare::editing::voice_changer::VoiceChanger changer(config);
    return changer.process(audio);
  });
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VoiceChangeRealtime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "preset must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "channels must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto samples = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_narrow_int(env, info[1], "sampleRate");
  const std::string preset = node_narrow_string(env, info[2], "preset");
  const int channels = node_narrow_int(env, info[3], "channels");
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError err =
      sonare_voice_change_realtime(samples.Data(), samples.ElementLength(), sample_rate,
                                   preset.c_str(), channels, &output, &output_length);
  if (err != SONARE_OK) {
    sonare_free_floats(output);
    ThrowIfError(env, err);
    return env.Undefined();
  }
  std::vector<float> result(output, output + output_length);
  sonare_free_floats(output);
  return VecToFloat32(env, result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Trim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float threshold_db{};
  if (!OptionalFiniteFloatArg(env, info, 2, "thresholdDb", -60.0f, &threshold_db))
    return env.Undefined();
  auto parse_frame_option = [&](size_t index, const char* name, int fallback, int* output) {
    if (info.Length() <= index || info[index].IsUndefined()) {
      *output = fallback;
      return true;
    }
    if (!info[index].IsNumber()) {
      Napi::TypeError::New(env, std::string(name) + " must be an integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    const double value = info[index].As<Napi::Number>().DoubleValue();
    if (!std::isfinite(value) || std::floor(value) != value) {
      Napi::RangeError::New(env, std::string(name) + " must be an integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    if (value <= 0 || value > std::numeric_limits<int>::max()) {
      Napi::RangeError::New(env, std::string(name) + " must be an integer in [1, " +
                                     std::to_string(std::numeric_limits<int>::max()) + "]")
          .ThrowAsJavaScriptException();
      return false;
    }
    *output = static_cast<int>(value);
    return true;
  };
  int frame_length = sonare::constants::kDefaultNFft;
  int hop_length = sonare::constants::kDefaultHopLength;
  if (!parse_frame_option(3, "frameLength", sonare::constants::kDefaultNFft, &frame_length) ||
      !parse_frame_option(4, "hopLength", sonare::constants::kDefaultHopLength, &hop_length)) {
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim_absolute(audio, threshold_db, frame_length, hop_length);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sampleRate");
  float target_db{};
  if (!OptionalFiniteFloatArg(env, info, 2, "targetDb", 0.0f, &target_db)) return env.Undefined();
  std::string mode;
  if (!OptionalStringArg(env, info, 3, "mode", "peak", &mode)) {
    return env.Undefined();
  }
  if (mode != "peak" && mode != "rms") {
    Napi::TypeError::New(env, "mode must be 'peak' or 'rms'").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(nullptr, "samples", data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = mode == "rms" ? sonare::normalize_rms(audio, target_db, true)
                                       : sonare::normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NormalizeStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "left must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "right must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left_typed = info[0].As<Napi::Float32Array>();
  auto right_typed = info[1].As<Napi::Float32Array>();
  int sr = 0;
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  float target_db = 0.0f;
  if (!OptionalFloatArg(env, info, 3, "targetDb", 0.0f, &target_db)) return env.Undefined();
  std::string mode;
  if (!OptionalStringArg(env, info, 4, "mode", "peak", &mode)) return env.Undefined();
  if (mode != "peak" && mode != "rms") {
    Napi::TypeError::New(env, "mode must be 'peak' or 'rms'").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation these direct core calls would otherwise
  // bypass. Per channel, because each carries its own samples.
  sonare::validate_offline_audio_input(nullptr, "left", left_typed.Data(),
                                       left_typed.ElementLength(), sr);
  sonare::validate_offline_audio_input(nullptr, "right", right_typed.Data(),
                                       right_typed.ElementLength(), sr);
  sonare::Audio left =
      sonare::Audio::from_buffer(left_typed.Data(), left_typed.ElementLength(), sr);
  sonare::Audio right =
      sonare::Audio::from_buffer(right_typed.Data(), right_typed.ElementLength(), sr);
  sonare::NormalizeStereoResult result =
      mode == "rms" ? sonare::normalize_rms_stereo(left, right, target_db, true)
                    : sonare::normalize_stereo(left, right, target_db);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, left_out));
  out.Set("right", VecToFloat32(env, right_out));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  return out;
  SONARE_NODE_CATCH(env)
}
