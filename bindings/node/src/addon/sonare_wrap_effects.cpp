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
#include "sonare_wrap_note_objects.h"
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
  int sr = node_narrow_int(env, info[1], "sr");
  HpssArguments args;
  if (!ReadHpssArguments(env, info, &args)) return env.Undefined();

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
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

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sr");

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
  int sr = node_narrow_int(env, info[1], "sr");

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
  int sr = node_narrow_int(env, info[1], "sr");
  float rate = info[2].As<Napi::Number>().FloatValue();
  int n_fft = node_arg_int(info, 3, 2048);
  int hop_length = node_arg_int(info, 4, 512);

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
  int sr = node_narrow_int(env, info[1], "sr");
  float semitones = info[2].As<Napi::Number>().FloatValue();
  int n_fft = node_arg_int(info, 3, 2048);
  int hop_length = node_arg_int(info, 4, 512);

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
  int sr = node_narrow_int(env, info[1], "sr");
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
  int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  float target_midi = info[3].As<Napi::Number>().FloatValue();
  int hop_length = node_narrow_int(env, info[4], "hopLength");
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
  int sr = node_narrow_int(env, info[1], "sr");
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
    config.target_midi = sonare_node::FloatProperty(opts, "targetMidi", config.target_midi);
    config.scale_root = sonare_node::IntProperty(opts, "scaleRoot", config.scale_root);
    config.scale_mode_mask = static_cast<uint32_t>(
        sonare_node::IntProperty(opts, "scaleModeMask", static_cast<int>(config.scale_mode_mask)));
    config.scale_reference_midi =
        sonare_node::FloatProperty(opts, "referenceMidi", config.scale_reference_midi);
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
  int sr = node_narrow_int(env, info[1], "sr");
  int onset_sample = node_narrow_int(env, info[2], "onsetSample");
  int offset_sample = node_narrow_int(env, info[3], "offsetSample");
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
  const int sr = node_narrow_int(env, info[1], "sr");
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::NoteRegion region;
  region.onset_sample = node_narrow_int(env, info[2], node_arg_label(2).c_str());
  region.offset_sample = node_narrow_int(env, info[3], node_arg_label(3).c_str());
  sonare::editing::pitch_editor::NoteEditor editor;
  sonare::Audio result =
      editor.move_note(audio, region, node_narrow_int(env, info[4], node_arg_label(4).c_str()));
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

namespace {

/// Releases a note-object result however the marshalling below leaves scope.
struct OwnedNoteObjects {
  SonareNoteObjectsResult value{};
  ~OwnedNoteObjects() { sonare_free_note_objects(&value); }
};

/// Releases a percussive-event result however the marshalling below leaves scope.
struct OwnedPercussiveEvents {
  SonarePercussiveEventsResult value{};
  ~OwnedPercussiveEvents() { sonare_free_percussive_events(&value); }
};

/// Releases a pitch decomposition however the marshalling below leaves scope.
struct OwnedPitchDecomposition {
  SonarePitchDecompositionResult value{};
  ~OwnedPitchDecomposition() { sonare_free_pitch_decomposition(&value); }
};

/// The options bag extraction, split and merge share: the segmentation knobs
/// plus the voicing arrays the track is thresholded with.
struct NoteTrackOptions {
  SonareNoteExtractorConfig config{};
  std::vector<int32_t> voiced;
  std::vector<float> voiced_prob;
  const int32_t* voiced_ptr = nullptr;
  const float* prob_ptr = nullptr;
};

/// Reads the shared options bag, which may be absent. A voicing array that does
/// not match the track leaves a RangeError pending, so the caller must bail out
/// on a false return before its C-ABI call.
bool ReadNoteTrackOptions(Napi::Env env, const Napi::Value& value, size_t n_frames,
                          NoteTrackOptions* out) {
  out->config.struct_version = 1;
  if (!value.IsObject()) {
    return true;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it.
  out->config.segmentation_threshold_cents =
      FloatProperty(opts, "segmentationThresholdCents", 0.0f);
  out->config.min_note_ms = FloatProperty(opts, "minNoteMs", 0.0f);
  out->config.reference_hz = FloatProperty(opts, "referenceHz", 0.0f);
  out->config.voiced_threshold = FloatProperty(opts, "voicedThreshold", 0.0f);

  const Napi::Value voiced_value = opts.Get("voiced");
  if (IsInt32Array(voiced_value)) {
    auto arr = voiced_value.As<Napi::Int32Array>();
    if (arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced.assign(arr.Data(), arr.Data() + arr.ElementLength());
    out->voiced_ptr = out->voiced.data();
  }
  const Napi::Value prob_value = opts.Get("voicedProb");
  if (IsFloat32Array(prob_value)) {
    auto arr = prob_value.As<Napi::Float32Array>();
    if (arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voicedProb must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced_prob.assign(arr.Data(), arr.Data() + arr.ElementLength());
    out->prob_ptr = out->voiced_prob.data();
  }
  return true;
}

/// Rejects a row whose declared-mandatory sample bound is absent or not a
/// number, in place of the type-checked reader's silent fallback. Throws the
/// error class the WASM surface throws for the same omission, so the two agree
/// on the rejection as well as on the policy. Shared by the note and the
/// percussive-event readers, whose input types declare the same two fields.
void RequireSpanKey(const char* fn, const char* subject, const Napi::Object& row, const char* key) {
  const Napi::Value value = row.Get(key);
  if (value.IsUndefined() || value.IsNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(fn) + " " + subject + "." + key + " is required");
  }
  if (!value.IsNumber()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        std::string(fn) + " " + subject + "." + key + " must be a number");
  }
}

/// Reads a JS note array onto the C structs, plus the envelope pool their edits
/// index into. A render reads the sample spans and a curve edit the frame
/// bounds; split and merge re-derive both from the frame bounds alone, so all
/// of them are read here and the entry point decides what it needs.
///
/// @p require_span demands the sample bounds the NoteObjectInput type declares
/// mandatory. Only a render reads them, and an omitted one used to default to
/// 0, which is a zero-length span: the note's edit silently rendered as nothing
/// while the same request threw on the WASM surface. The note-set entries split
/// and merge take do not declare the bounds at all, so they keep the default.
void ReadNotes(const char* fn, const Napi::Array& js_notes, std::vector<SonareNoteObject>* notes,
               std::vector<float>* envelopes, bool require_span = false) {
  const uint32_t count = js_notes.Length();
  notes->assign(count, SonareNoteObject{});
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value item = js_notes.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error(std::string(fn) + ": each note must be a plain object");
    }
    Napi::Object note = item.As<Napi::Object>();
    SonareNoteObject& row = (*notes)[i];
    if (require_span) {
      RequireSpanKey(fn, "note", note, "onsetSample");
      RequireSpanKey(fn, "note", note, "offsetSample");
    }
    row.onset_sample = Int64Property(note, "onsetSample", 0);
    row.offset_sample = Int64Property(note, "offsetSample", 0);
    row.frame_start = IntProperty(note, "frameStart", 0);
    row.frame_end = IntProperty(note, "frameEnd", 0);
    row.median_hz = FloatProperty(note, "medianHz", 0.0f);
    ReadNoteEdit(fn, note, envelopes, &row.edit);
  }
}

/// Bounds-checks a note's slice of one of a result's shared pools. Only
/// reachable if the C ABI contradicted itself; say so, because an empty slice
/// here would read like a short note and hide the inconsistency.
void RequireSliceInRange(const char* fn, const char* field, int64_t offset, int64_t span,
                         size_t pool_count) {
  if (span < 0 || offset < 0 ||
      static_cast<uint64_t>(offset) + static_cast<uint64_t>(span) > pool_count) {
    throw std::runtime_error(std::string(fn) + ": " + field + " slice out of range");
  }
}

/// Marshals a heap-owned result into the JS note array, giving every note its
/// own copy of its amplitude and envelope slices.
Napi::Array NoteObjectsToJs(Napi::Env env, const char* fn, const SonareNoteObjectsResult& result) {
  Napi::Array notes = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    // Each note is given its own copy of both slices, so neither pool offset
    // ever reaches JS.
    const size_t envelope_count = note.edit.envelope_count;
    RequireSliceInRange(fn, "amplitudeEnvelope", note.edit.envelope_offset,
                        static_cast<int64_t>(envelope_count), result.envelope_count);
    const int64_t span = note.frame_end - note.frame_start;
    RequireSliceInRange(fn, "amplitude", note.amplitude_offset, span, result.amplitude_count);
    notes.Set(static_cast<uint32_t>(i),
              NoteObjectToJs(env, note, result.amplitude + note.amplitude_offset,
                             static_cast<size_t>(span),
                             result.envelopes + note.edit.envelope_offset, envelope_count));
  }
  return notes;
}

/// Reads the four separation keys both percussive-event entry points share. The
/// framing is one input, not a knob each side restates, so neither can lift a
/// signal out on a framing the other did not measure on.
void ReadPercussiveSeparation(const Napi::Object& opts, int32_t* n_fft, int32_t* hop_length,
                              int32_t* kernel_harmonic, int32_t* kernel_percussive) {
  *n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
  *hop_length = IntProperty(opts, "hopLength", kZeroIsSentinel);
  *kernel_harmonic = IntProperty(opts, "hpssKernelHarmonic", kZeroIsSentinel);
  *kernel_percussive = IntProperty(opts, "hpssKernelPercussive", kZeroIsSentinel);
}

/// Reads the extraction options bag, which may be absent. Every field takes its
/// default at 0 on the C side, so an omitted bag and an all-zero one agree.
void ReadPercussiveEventConfig(const Napi::Value& value, SonarePercussiveEventConfig* out) {
  out->struct_version = 1;
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it.
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->onset_wait = IntProperty(opts, "onsetWait", kZeroIsSentinel);
  out->onset_delta = FloatProperty(opts, "onsetDelta", 0.0f);
  out->max_event_ms = FloatProperty(opts, "maxEventMs", 0.0f);
  // 0 is this field's own meaning as well as its default, and the C ABI assigns
  // it as-is, so "keep everything" stays reachable from here.
  out->min_percussive_ratio = FloatProperty(opts, "minPercussiveRatio", 0.0f);
}

/// Reads the render options bag, under the same rule.
void ReadPercussiveRenderConfig(const Napi::Value& value, SonarePercussiveRenderConfig* out) {
  out->struct_version = 1;
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->fade_ms = FloatProperty(opts, "fadeMs", 0.0f);
}

/// Reads an event's optional `edit`. A zeroed SonarePercussiveEventEdit is the
/// identity, so an omitted edit, and an omitted key within one, is a no-op.
void ReadPercussiveEventEdit(const char* fn, const Napi::Object& event,
                             SonarePercussiveEventEdit* out) {
  const Napi::Value edit_value = event.Get("edit");
  if (edit_value.IsUndefined() || edit_value.IsNull()) {
    return;
  }
  if (!edit_value.IsObject() || edit_value.IsArray()) {
    throw std::runtime_error(std::string(fn) + ": event.edit must be a plain object");
  }
  Napi::Object edit = edit_value.As<Napi::Object>();
  out->time_offset_samples = Int64Property(edit, "timeOffsetSamples", 0);
  out->gain_db = FloatProperty(edit, "gainDb", 0.0f);
  out->muted = BoolProperty(edit, "muted", false) ? 1 : 0;
}

/// Reads a JS event array onto the C structs. Only the span and the edit are
/// read: rendering ignores the measured figures, so an event straight from an
/// extraction round-trips without them having to survive the trip. The span is
/// required, as PercussiveEventInput declares it — an omitted bound defaulted to
/// 0, and a zero-length span renders the event's edit as nothing.
void ReadPercussiveEvents(const char* fn, const Napi::Array& js_events,
                          std::vector<SonarePercussiveEvent>* events) {
  const uint32_t count = js_events.Length();
  events->assign(count, SonarePercussiveEvent{});
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value item = js_events.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error(std::string(fn) + ": each event must be a plain object");
    }
    Napi::Object event = item.As<Napi::Object>();
    SonarePercussiveEvent& row = (*events)[i];
    RequireSpanKey(fn, "event", event, "onsetSample");
    RequireSpanKey(fn, "event", event, "offsetSample");
    row.onset_sample = Int64Property(event, "onsetSample", 0);
    row.offset_sample = Int64Property(event, "offsetSample", 0);
    ReadPercussiveEventEdit(fn, event, &row.edit);
  }
}

/// Marshals a heap-owned result into the JS event array. One allocation, unlike
/// the note objects: an event carries three scalars and nothing per frame, so
/// there is no pool to slice.
Napi::Array PercussiveEventsToJs(Napi::Env env, const SonarePercussiveEventsResult& result) {
  Napi::Array events = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonarePercussiveEvent& event = result.events[i];
    Napi::Object row = Napi::Object::New(env);
    // No int64 in N-API: sample positions marshal as JS numbers, exact up to
    // Number.MAX_SAFE_INTEGER (2^53-1 samples, millennia of audio).
    row.Set("onsetSample", Napi::Number::New(env, static_cast<double>(event.onset_sample)));
    row.Set("offsetSample", Napi::Number::New(env, static_cast<double>(event.offset_sample)));
    row.Set("strength", Napi::Number::New(env, event.strength));
    row.Set("peakAmplitude", Napi::Number::New(env, event.peak_amplitude));
    row.Set("percussiveRatio", Napi::Number::New(env, event.percussive_ratio));

    Napi::Object edit = Napi::Object::New(env);
    edit.Set("timeOffsetSamples",
             Napi::Number::New(env, static_cast<double>(event.edit.time_offset_samples)));
    edit.Set("gainDb", Napi::Number::New(env, event.edit.gain_db));
    edit.Set("muted", Napi::Boolean::New(env, event.edit.muted != 0));
    row.Set("edit", edit);

    events.Set(static_cast<uint32_t>(i), row);
  }
  return events;
}

}  // namespace

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
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = info[3].As<Napi::Number>().FloatValue();

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[4], n_frames, &options)) {
    return env.Undefined();
  }

  OwnedNoteObjects result;
  const SonareError err =
      sonare_extract_notes(data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr,
                           n_frames, frame_rate, &options.config, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "extractNotes", result.value);
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
  const int sr = node_narrow_int(env, info[1], "sr");

  SonareNoteRenderConfig config{};
  config.struct_version = 1;
  // The track a curve edit acts on rides in the same bag; the C ABI's frame
  // count is derived from its length rather than taken separately.
  Napi::Float32Array f0;
  const float* f0_data = nullptr;
  size_t n_frames = 0;
  float frame_rate = 0.0f;
  if (info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.fade_ms = FloatProperty(opts, "fadeMs", 0.0f);
    config.vibrato_cutoff_hz = FloatProperty(opts, "vibratoCutoffHz", 0.0f);
    frame_rate = FloatProperty(opts, "frameRate", 0.0f);
    const Napi::Value f0_value = opts.Get("f0Hz");
    if (IsFloat32Array(f0_value)) {
      f0 = f0_value.As<Napi::Float32Array>();
      f0_data = f0.Data();
      n_frames = f0.ElementLength();
    }
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("renderNotes", info[2].As<Napi::Array>(), &notes, &envelopes, /*require_span=*/true);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_notes(data, length, sr, notes.empty() ? nullptr : notes.data(), notes.size(),
                          envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), f0_data,
                          n_frames, frame_rate, &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = CopyToFloat32(env, out, out_length);
  sonare_free_floats(out);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DecomposeNotePitch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (f0Hz, frameRate, medianHz, vibratoCutoffHz)
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (f0Hz Float32Array, frameRate, medianHz, vibratoCutoffHz)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto f0 = info[0].As<Napi::Float32Array>();
  const float frame_rate = info[1].As<Napi::Number>().FloatValue();
  const float median_hz = info[2].As<Napi::Number>().FloatValue();
  const float vibrato_cutoff_hz = info[3].As<Napi::Number>().FloatValue();

  OwnedPitchDecomposition decomposition;
  const SonareError err =
      sonare_decompose_note_pitch(f0.Data(), f0.ElementLength(), frame_rate, median_hz,
                                  vibrato_cutoff_hz, &decomposition.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("centreHz", Napi::Number::New(env, decomposition.value.centre_hz));
  // A note with no usable pitch comes back as a zero centre and NULL curves,
  // which marshal to two empty arrays rather than to an error.
  out.Set("driftCents",
          CopyToFloat32(env, decomposition.value.drift_cents, decomposition.value.count));
  out.Set("vibratoCents",
          CopyToFloat32(env, decomposition.value.vibrato_cents, decomposition.value.count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, index, frame, options?)
  if (info.Length() < 7 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsArray() ||
      !info[5].IsNumber() || !info[6].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate, "
                         "notes: object[], index, frame)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = info[3].As<Napi::Number>().FloatValue();
  // A negative index arrives as a size_t past every note, which the C ABI
  // rejects as the out-of-range index it is.
  const size_t index = static_cast<size_t>(node_narrow_int64(env, info[5], "index"));
  const int32_t frame = node_narrow_int(env, info[6], "frame");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("splitNote", info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err =
      sonare_split_note(data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr, n_frames,
                        frame_rate, &options.config, notes.empty() ? nullptr : notes.data(),
                        notes.size(), envelopes.empty() ? nullptr : envelopes.data(),
                        envelopes.size(), index, frame, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "splitNote", result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MergeNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, first, last, options?)
  if (info.Length() < 7 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsArray() ||
      !info[5].IsNumber() || !info[6].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate, "
                         "notes: object[], first, last)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = info[3].As<Napi::Number>().FloatValue();
  const size_t first = static_cast<size_t>(node_narrow_int64(env, info[5], "first"));
  const size_t last = static_cast<size_t>(node_narrow_int64(env, info[6], "last"));

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("mergeNotes", info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err = sonare_merge_notes(
      data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr, n_frames, frame_rate,
      &options.config, notes.empty() ? nullptr : notes.data(), notes.size(),
      envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), first, last, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "mergeNotes", result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ExtractPercussiveEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, options?)
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");

  SonarePercussiveEventConfig config{};
  ReadPercussiveEventConfig(info[2], &config);

  OwnedPercussiveEvents result;
  const SonareError err =
      sonare_extract_percussive_events(data, length, sr, &config, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  // Audio in which nothing was detected comes back as a NULL pointer and a zero
  // count, which marshals to an empty array rather than to an error.
  return PercussiveEventsToJs(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RenderPercussiveEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, events, options?)
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, events: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");

  SonarePercussiveRenderConfig config{};
  ReadPercussiveRenderConfig(info[3], &config);

  std::vector<SonarePercussiveEvent> events;
  ReadPercussiveEvents("renderPercussiveEvents", info[2].As<Napi::Array>(), &events);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_percussive_events(data, length, sr, events.empty() ? nullptr : events.data(),
                                      events.size(), &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = CopyToFloat32(env, out, out_length);
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
  int sr = node_narrow_int(env, info[1], "sr");
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
  SONARE_NODE_TRY
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsString() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, preset, channels)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto samples = info[0].As<Napi::Float32Array>();
  const int sample_rate = node_narrow_int(env, info[1], "sampleRate");
  const std::string preset = info[2].As<Napi::String>().Utf8Value();
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
  int sr = node_narrow_int(env, info[1], "sr");
  float target_db = node_arg_finite_float(info, 2, 0.0f);
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
  if (s == "hann") return SONARE_WINDOW_HANN;
  if (s == "hamming") return SONARE_WINDOW_HAMMING;
  if (s == "blackman") return SONARE_WINDOW_BLACKMAN;
  if (s == "rectangular" || s == "rect") return SONARE_WINDOW_RECTANGULAR;
  return -1;
}

/// @brief Resolve a window given as a name or as a @ref SonareWindowType ordinal.
/// @details Both spellings reach the same rejection, matching the C ABI and the
///   WASM reader. Validating only the name leaves the numeric form -- the one a
///   generated binding produces -- as a type error, and the ordinal is bounded
///   by the enumerators rather than by a literal, so a member added later
///   widens the accepted set instead of silently changing what 4 means.
/// @throws SonareException(InvalidParameter) for a value that is neither, or an
///   ordinal outside the enum.
int read_window_type(Napi::Env env, const Napi::Value& value) {
  if (value.IsNumber()) {
    // An ordinal is a member, not a magnitude, so the fraction is refused here
    // rather than truncated: the positional readers let 31.5 reach a callee as
    // 31 because that does not change what was asked for, but 1.5 selecting
    // hamming is a window the caller never named. A domain check, not a
    // narrowing one -- node_narrow_int has already accepted the value.
    if (sonare_node::node_is_fraction(value)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "spectralEdit: window ordinal must be an integer, not " +
                                        std::to_string(value.As<Napi::Number>().DoubleValue()));
    }
    const int ordinal = sonare_node::node_narrow_int(env, value, "window");
    if (ordinal < SONARE_WINDOW_HANN || ordinal > SONARE_WINDOW_RECTANGULAR) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "spectralEdit: unknown window type: " + std::to_string(ordinal));
    }
    return ordinal;
  }
  if (!value.IsString()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "spectralEdit: window must be a window name or a window ordinal");
  }
  std::string name = value.As<Napi::String>().Utf8Value();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const int mapped = parse_window_type(name);
  if (mapped < 0) {
    throw std::runtime_error("spectralEdit: unknown window type: " +
                             value.As<Napi::String>().Utf8Value());
  }
  return mapped;
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
  int sr = node_narrow_int(env, info[1], "sr");

  // Build config (optional fourth argument).
  SonareSpectralEditConfig config{};  // zero-init = all defaults
  const SonareSpectralEditConfig* config_ptr = nullptr;
  if (info.Length() >= 4 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
    config.hop_length = IntProperty(opts, "hopLength", kZeroIsSentinel);
    config.heal_radius_frames = node_int_option(opts, "healRadiusFrames", kZeroIsSentinel);

    // Optional window, by name or by ordinal.
    Napi::Value win_val = opts.Get("window");
    if (!win_val.IsUndefined() && !win_val.IsNull()) {
      config.window = read_window_type(env, win_val);
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
    // This bag reads two ways. startSample and gainDb here, and nFft and
    // hopLength in the config object above, refuse a wrong-typed value by name;
    // endSample, lowHz, highHz and healRadiusFrames answer one with the default.
    // The WASM binding reads every one of them through an inline
    // hasProperty/as<T>() pair, which coerces rather than doing either.
    ops[i].start_sample = Int64Property(op, "startSample", 0);
    ops[i].end_sample = node_int64_option(op, "endSample", static_cast<int64_t>(length));
    ops[i].low_hz = node_float_option(op, "lowHz", 0.0f);
    ops[i].high_hz = node_float_option(op, "highHz", 0.0f);
    ops[i].gain_db = FloatProperty(op, "gainDb", 0.0f);

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
