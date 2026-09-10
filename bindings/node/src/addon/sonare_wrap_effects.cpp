#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
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
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

Napi::Value SonareWrap::Hpss(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int kernel_harmonic =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 31;
  int kernel_percussive =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 31;
  int n_fft =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 512;
  const bool hard_mask =
      info.Length() >= 7 && info[6].IsBoolean() ? info[6].As<Napi::Boolean>().Value() : false;

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);

  sonare::HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;
  config.use_soft_mask = !hard_mask;

  sonare::StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;

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

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Percussive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();
  int n_fft =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 512;

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
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

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, semitones)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float semitones = info[2].As<Napi::Number>().FloatValue();
  int n_fft =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 512;

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
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

  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, currentMidi, targetMidi)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float current_midi = info[2].As<Napi::Number>().FloatValue();
  float target_midi = info[3].As<Napi::Number>().FloatValue();

  sonare::validate_offline_audio_input(data, length, sr);
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
  if (info.Length() < 5 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(
        env, "Expected (Float32Array, sampleRate, f0Hz Float32Array, targetMidi, hopLength)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  auto f0 = info[2].As<Napi::Float32Array>();
  float target_midi = info[3].As<Napi::Number>().FloatValue();
  int hop_length = info[4].As<Napi::Number>().Int32Value();
  const size_t n_frames = f0.ElementLength();

  sonare::editing::pitch_editor::F0Track track;
  track.sample_rate = sr;
  track.hop_length = hop_length;
  track.f0_hz.assign(f0.Data(), f0.Data() + n_frames);
  track.voiced.resize(n_frames);
  track.voiced_prob.resize(n_frames);

  const bool has_voiced = info.Length() > 5 && IsInt32Array(info[5]);
  const bool has_prob = info.Length() > 6 && IsFloat32Array(info[6]);
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
    const bool is_voiced = has_voiced ? (voiced_arr[i] != 0) : true;
    track.voiced[i] = is_voiced;
    track.voiced_prob[i] = has_prob ? prob_arr[i] : (is_voiced ? 1.0f : 0.0f);
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
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
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, f0Hz Float32Array, hopLength)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  int hop_length = info[3].As<Napi::Number>().Int32Value();

  SonarePitchCorrectionConfig config{};
  sonare_pitch_correction_config_default(&config);
  std::vector<int32_t> voiced;
  std::vector<float> voiced_prob;
  const int32_t* voiced_ptr = nullptr;
  const float* prob_ptr = nullptr;
  if (info.Length() > 4 && info[4].IsObject()) {
    Napi::Object opts = info[4].As<Napi::Object>();
    const Napi::Value mode_value = opts.Get("mode");
    if (!mode_value.IsUndefined() && !mode_value.IsNull()) {
      if (!mode_value.IsString()) {
        Napi::TypeError::New(env, "pitch correction mode must be 'midi' or 'scale'")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      std::string mode = mode_value.As<Napi::String>().Utf8Value();
      if (mode == "scale") {
        config.target_mode = SONARE_PITCH_TARGET_SCALE;
      } else if (mode != "midi") {
        Napi::RangeError::New(env, "unknown pitch correction mode").ThrowAsJavaScriptException();
        return env.Undefined();
      }
    }
    config.target_midi = sonare_node::node_float_option(opts, "targetMidi", config.target_midi);
    config.scale_root = sonare_node::node_int_option(opts, "scaleRoot", config.scale_root);
    config.scale_mode_mask = static_cast<uint32_t>(sonare_node::node_int_option(
        opts, "scaleModeMask", static_cast<int>(config.scale_mode_mask)));
    config.scale_reference_midi =
        sonare_node::node_float_option(opts, "referenceMidi", config.scale_reference_midi);
    config.retune_amount =
        sonare_node::node_float_option(opts, "retuneAmount", config.retune_amount);
    config.max_correction_semitones = sonare_node::node_float_option(
        opts, "maxCorrectionSemitones", config.max_correction_semitones);
    config.retune_speed_ms =
        sonare_node::node_float_option(opts, "retuneSpeedMs", config.retune_speed_ms);
    config.vibrato_threshold_cents = sonare_node::node_float_option(opts, "vibratoThresholdCents",
                                                                    config.vibrato_threshold_cents);
    if (opts.Has("voiced") && IsInt32Array(opts.Get("voiced"))) {
      auto arr = opts.Get("voiced").As<Napi::Int32Array>();
      if (arr.ElementLength() != n_frames) {
        Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      voiced.assign(arr.Data(), arr.Data() + arr.ElementLength());
      voiced_ptr = voiced.data();
    }
    if (opts.Has("voicedProb") && IsFloat32Array(opts.Get("voicedProb"))) {
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

Napi::Value SonareWrap::NoteStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 5 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, onsetSample, offsetSample, "
                         "stretchRatio)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int onset_sample = info[2].As<Napi::Number>().Int32Value();
  int offset_sample = info[3].As<Napi::Number>().Int32Value();
  float stretch_ratio = info[4].As<Napi::Number>().FloatValue();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
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
  if (info.Length() < 5 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, onsetSample, offsetSample, "
                         "targetOnsetSample)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::NoteRegion region;
  region.onset_sample = info[2].As<Napi::Number>().Int32Value();
  region.offset_sample = info[3].As<Napi::Number>().Int32Value();
  sonare::editing::pitch_editor::NoteEditor editor;
  sonare::Audio result = editor.move_note(audio, region, info[4].As<Napi::Number>().Int32Value());
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ExtractNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, options?)
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = info[3].As<Napi::Number>().FloatValue();

  SonareNoteExtractorConfig config{};
  config.struct_version = 1;
  std::vector<int32_t> voiced;
  std::vector<float> voiced_prob;
  const int32_t* voiced_ptr = nullptr;
  const float* prob_ptr = nullptr;
  if (info.Length() > 4 && info[4].IsObject()) {
    Napi::Object opts = info[4].As<Napi::Object>();
    // Effects options bag: the type-checked reader family, so an explicit
    // `undefined` (or any non-number) reads as the documented default.
    config.segmentation_threshold_cents =
        node_float_option(opts, "segmentationThresholdCents", 0.0f);
    config.min_note_ms = node_float_option(opts, "minNoteMs", 0.0f);
    config.reference_hz = node_float_option(opts, "referenceHz", 0.0f);
    config.voiced_threshold = node_float_option(opts, "voicedThreshold", 0.0f);
    const Napi::Value voiced_value = opts.Get("voiced");
    if (IsInt32Array(voiced_value)) {
      auto arr = voiced_value.As<Napi::Int32Array>();
      if (arr.ElementLength() != n_frames) {
        Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      voiced.assign(arr.Data(), arr.Data() + arr.ElementLength());
      voiced_ptr = voiced.data();
    }
    const Napi::Value prob_value = opts.Get("voicedProb");
    if (IsFloat32Array(prob_value)) {
      auto arr = prob_value.As<Napi::Float32Array>();
      if (arr.ElementLength() != n_frames) {
        Napi::RangeError::New(env, "voicedProb must match f0Hz length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      voiced_prob.assign(arr.Data(), arr.Data() + arr.ElementLength());
      prob_ptr = voiced_prob.data();
    }
  }

  SonareNoteObjectsResult result{};
  const SonareError err = sonare_extract_notes(data, length, sr, f0.Data(), prob_ptr, voiced_ptr,
                                               n_frames, frame_rate, &config, &result);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  Napi::Array notes = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    Napi::Object row = Napi::Object::New(env);
    // No int64 in N-API: sample positions marshal as JS numbers, exact up to
    // Number.MAX_SAFE_INTEGER (2^53-1 samples, millennia of audio).
    row.Set("onsetSample", Napi::Number::New(env, static_cast<double>(note.onset_sample)));
    row.Set("offsetSample", Napi::Number::New(env, static_cast<double>(note.offset_sample)));
    row.Set("frameStart", Napi::Number::New(env, note.frame_start));
    row.Set("frameEnd", Napi::Number::New(env, note.frame_end));
    row.Set("medianHz", Napi::Number::New(env, note.median_hz));
    row.Set("medianCents", Napi::Number::New(env, note.median_cents));
    row.Set("f0Stability", Napi::Number::New(env, note.f0_stability));

    Napi::Object edit = Napi::Object::New(env);
    edit.Set("timeOffsetSamples",
             Napi::Number::New(env, static_cast<double>(note.edit.time_offset_samples)));
    edit.Set("pitchShiftSemitones", Napi::Number::New(env, note.edit.pitch_shift_semitones));
    edit.Set("gainDb", Napi::Number::New(env, note.edit.gain_db));
    edit.Set("timeStretchRatio", Napi::Number::New(env, note.edit.time_stretch_ratio));
    edit.Set("muted", Napi::Boolean::New(env, note.edit.muted != 0));
    row.Set("edit", edit);

    // Each note carries its own slice, so amplitudeOffset never reaches JS.
    const int64_t span = note.frame_end - note.frame_start;
    const int64_t offset = note.amplitude_offset;
    if (span < 0 || offset < 0 || static_cast<size_t>(offset + span) > result.amplitude_count) {
      // Only reachable if the C ABI contradicted itself. Say so: an empty curve
      // here would read like a short note and hide the inconsistency.
      sonare_free_note_objects(&result);
      Napi::Error::New(env, "extractNotes: amplitude slice out of range")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    auto amplitude = Napi::Float32Array::New(env, static_cast<size_t>(span));
    if (span > 0) {
      std::memcpy(amplitude.Data(), result.amplitude + offset,
                  static_cast<size_t>(span) * sizeof(float));
    }
    row.Set("amplitude", amplitude);

    notes.Set(static_cast<uint32_t>(i), row);
  }
  sonare_free_note_objects(&result);
  return notes;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RenderNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, notes, options?)
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, notes: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = info[1].As<Napi::Number>().Int32Value();

  SonareNoteRenderConfig config{};
  config.struct_version = 1;
  if (info.Length() >= 4 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.fade_ms = node_float_option(opts, "fadeMs", 0.0f);
  }

  // A zeroed SonareNoteObject is the identity edit, so an omitted key is a no-op.
  auto js_notes = info[2].As<Napi::Array>();
  const uint32_t note_count = js_notes.Length();
  std::vector<SonareNoteObject> notes(note_count);
  for (uint32_t i = 0; i < note_count; ++i) {
    Napi::Value item = js_notes.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error("renderNotes: each note must be a plain object");
    }
    Napi::Object note = item.As<Napi::Object>();
    notes[i].onset_sample = node_int64_option(note, "onsetSample", 0);
    notes[i].offset_sample = node_int64_option(note, "offsetSample", 0);

    const Napi::Value edit_value = note.Get("edit");
    if (!edit_value.IsUndefined() && !edit_value.IsNull()) {
      if (!edit_value.IsObject() || edit_value.IsArray()) {
        throw std::runtime_error("renderNotes: note.edit must be a plain object");
      }
      Napi::Object edit = edit_value.As<Napi::Object>();
      notes[i].edit.time_offset_samples = node_int64_option(edit, "timeOffsetSamples", 0);
      notes[i].edit.pitch_shift_semitones = node_float_option(edit, "pitchShiftSemitones", 0.0f);
      notes[i].edit.gain_db = node_float_option(edit, "gainDb", 0.0f);
      notes[i].edit.time_stretch_ratio = node_float_option(edit, "timeStretchRatio", 0.0f);
      notes[i].edit.muted = node_bool_option(edit, "muted", false) ? 1 : 0;
    }
  }

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_notes(data, length, sr, note_count > 0 ? notes.data() : nullptr, note_count,
                          &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, out_length);
  if (out_length > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, out_length * sizeof(float));
  }
  sonare_free_floats(out);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VoiceChange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, pitchSemitones, formantFactor)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float pitch_semitones = info[2].As<Napi::Number>().FloatValue();
  float formant_factor = info[3].As<Napi::Number>().FloatValue();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  sonare::editing::voice_changer::VoiceChanger changer(config);
  sonare::Audio result = changer.process(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VoiceChangeRealtime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsString() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, preset, channels)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto samples = info[0].As<Napi::Float32Array>();
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  const std::string preset = info[2].As<Napi::String>().Utf8Value();
  const int channels = info[3].As<Napi::Number>().Int32Value();
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
}

Napi::Value SonareWrap::Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float target_db =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  std::string mode =
      info.Length() >= 4 && info[3].IsString() ? info[3].As<Napi::String>().Utf8Value() : "peak";
  if (mode != "peak" && mode != "rms") {
    Napi::TypeError::New(env, "normalize: mode must be 'peak' or 'rms'")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = mode == "rms" ? sonare::normalize_rms(audio, target_db, true)
                                       : sonare::normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Map a lowercase window string to the SonareWindowType integer.
/// Returns -1 on an unrecognised name (caller should throw).
int parse_window_type(const std::string& s) {
  if (s == "hann") return 0;
  if (s == "hamming") return 1;
  if (s == "blackman") return 2;
  if (s == "rectangular" || s == "rect") return 3;
  return -1;
}

/// @brief Map a lowercase spectral-edit mode string to SonareSpectralEditMode.
/// Returns -1 on an unrecognised name (caller should throw).
int parse_spectral_edit_mode(const std::string& s) {
  if (s == "gain") return SONARE_SPECTRAL_EDIT_MODE_GAIN;
  if (s == "attenuate") return SONARE_SPECTRAL_EDIT_MODE_ATTENUATE;
  if (s == "mute") return SONARE_SPECTRAL_EDIT_MODE_MUTE;
  if (s == "heal") return SONARE_SPECTRAL_EDIT_MODE_HEAL;
  return -1;
}

}  // namespace

Napi::Value SonareWrap::SpectralEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples: Float32Array, sampleRate: number, ops: object[], options?: object)
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, ops: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  // Build config (optional fourth argument).
  SonareSpectralEditConfig config{};  // zero-init = all defaults
  const SonareSpectralEditConfig* config_ptr = nullptr;
  if (info.Length() >= 4 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.n_fft = node_int_option(opts, "nFft", 0);
    config.hop_length = node_int_option(opts, "hopLength", 0);
    config.heal_radius_frames = node_int_option(opts, "healRadiusFrames", 0);

    // Parse optional window string.
    Napi::Value win_val = opts.Get("window");
    if (!win_val.IsUndefined() && !win_val.IsNull()) {
      if (!win_val.IsString()) {
        throw std::runtime_error("spectralEdit: window must be a string");
      }
      std::string win_str = win_val.As<Napi::String>().Utf8Value();
      std::transform(win_str.begin(), win_str.end(), win_str.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      int win_int = parse_window_type(win_str);
      if (win_int < 0) {
        throw std::runtime_error("spectralEdit: unknown window type: " +
                                 win_val.As<Napi::String>().Utf8Value());
      }
      config.window = win_int;
    }
    config_ptr = &config;
  }

  // Build ops array from the JS array of plain objects.
  auto js_ops = info[2].As<Napi::Array>();
  const uint32_t n_ops = js_ops.Length();
  std::vector<SonareSpectralRegionOp> ops(n_ops);
  for (uint32_t i = 0; i < n_ops; ++i) {
    Napi::Value item = js_ops.Get(i);
    if (!item.IsObject()) {
      throw std::runtime_error("spectralEdit: each op must be a plain object");
    }
    Napi::Object op = item.As<Napi::Object>();
    // Effects options bag: the type-checked reader family, so an explicit
    // `undefined` (or any non-number) reads as the documented default.
    ops[i].start_sample = node_int64_option(op, "startSample", 0);
    ops[i].end_sample = node_int64_option(op, "endSample", static_cast<int64_t>(length));
    ops[i].low_hz = node_float_option(op, "lowHz", 0.0f);
    ops[i].high_hz = node_float_option(op, "highHz", 0.0f);
    ops[i].gain_db = node_float_option(op, "gainDb", 0.0f);

    // Resolve mode: required string field.
    Napi::Value mode_val = op.Get("mode");
    if (mode_val.IsUndefined() || mode_val.IsNull()) {
      ops[i].mode = SONARE_SPECTRAL_EDIT_MODE_GAIN;
    } else {
      if (!mode_val.IsString()) {
        throw std::runtime_error("spectralEdit: op.mode must be a string");
      }
      std::string mode_str = mode_val.As<Napi::String>().Utf8Value();
      std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      int mode_int = parse_spectral_edit_mode(mode_str);
      if (mode_int < 0) {
        throw std::runtime_error("spectralEdit: unknown mode: " +
                                 mode_val.As<Napi::String>().Utf8Value());
      }
      ops[i].mode = mode_int;
    }
  }

  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_spectral_edit(
      data, length, sr, config_ptr, n_ops > 0 ? ops.data() : nullptr, n_ops, &out, &out_length);
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
