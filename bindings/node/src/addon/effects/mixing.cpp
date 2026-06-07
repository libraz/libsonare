#include <algorithm>
#include <cctype>
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

namespace {

int PanModeValue(const Napi::Value& value) {
  if (value.IsNumber()) {
    return value.As<Napi::Number>().Int32Value();
  }
  if (!value.IsString()) {
    return SONARE_PAN_MODE_BALANCE;
  }
  std::string mode = value.As<Napi::String>().Utf8Value();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return SONARE_PAN_MODE_STEREO_PAN;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

Napi::Value OptionAt(Napi::Env env, const Napi::Object& options, const char* key, size_t index) {
  if (!options.Has(key)) {
    return env.Undefined();
  }
  Napi::Value value = options.Get(key);
  if (value.IsArray()) {
    return value.As<Napi::Array>().Get(index);
  }
  return value;
}

Napi::Object MixMeterToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("peakDbL", snapshot.peak_db_l);
  out.Set("peakDbR", snapshot.peak_db_r);
  out.Set("rmsDbL", snapshot.rms_db_l);
  out.Set("rmsDbR", snapshot.rms_db_r);
  out.Set("correlation", snapshot.correlation);
  out.Set("monoCompatWidth", snapshot.mono_compat_width);
  out.Set("monoCompatPeak", snapshot.mono_compat_peak);
  out.Set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.Set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.Set("momentaryLufs", snapshot.momentary_lufs);
  out.Set("shortTermLufs", snapshot.short_term_lufs);
  out.Set("integratedLufs", snapshot.integrated_lufs);
  out.Set("gainReductionDb", snapshot.gain_reduction_db);
  out.Set("truePeakDbL", snapshot.true_peak_db_l);
  out.Set("truePeakDbR", snapshot.true_peak_db_r);
  out.Set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));
  return out;
}

}  // namespace

Napi::Value SonareWrap::MixingScenePresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const char* raw = sonare_mixing_scene_preset_names();
  Napi::Array out = Napi::Array::New(env);
  if (raw == nullptr || raw[0] == '\0') {
    return out;
  }
  std::string names(raw);
  size_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    out.Set(index++,
            names.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

Napi::Value SonareWrap::MixingScenePresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected preset name").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  SonareError err =
      sonare_mixing_scene_preset_json(info[0].As<Napi::String>().Utf8Value().c_str(), &json);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  std::string result = json != nullptr ? json : "";
  sonare_free_string(json);
  return Napi::String::New(env, result);
}

Napi::Value SonareWrap::MixStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsArray() || !info[1].IsArray() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (leftChannels, rightChannels, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array left_input = info[0].As<Napi::Array>();
  Napi::Array right_input = info[1].As<Napi::Array>();
  const size_t count = left_input.Length();
  if (count == 0 || right_input.Length() != count) {
    Napi::TypeError::New(env, "leftChannels and rightChannels must have the same non-zero length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Float32Array> left_arrays;
  std::vector<Napi::Float32Array> right_arrays;
  std::vector<const float*> left_ptrs;
  std::vector<const float*> right_ptrs;
  left_arrays.reserve(count);
  right_arrays.reserve(count);
  left_ptrs.reserve(count);
  right_ptrs.reserve(count);

  size_t length = 0;
  for (size_t index = 0; index < count; ++index) {
    Napi::Value left_value = left_input.Get(index);
    Napi::Value right_value = right_input.Get(index);
    if (!IsFloat32Array(left_value) || !IsFloat32Array(right_value)) {
      Napi::TypeError::New(env, "all channels must be Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_arrays.push_back(left_value.As<Napi::Float32Array>());
    right_arrays.push_back(right_value.As<Napi::Float32Array>());
    if (left_arrays.back().ElementLength() != right_arrays.back().ElementLength()) {
      Napi::TypeError::New(env, "left and right channel lengths must match")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (index == 0) {
      length = left_arrays.back().ElementLength();
    } else if (left_arrays.back().ElementLength() != length) {
      Napi::TypeError::New(env, "all strips must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_ptrs.push_back(left_arrays.back().Data());
    right_ptrs.push_back(right_arrays.back().Data());
  }

  const int sample_rate = info[2].As<Napi::Number>().Int32Value();
  Napi::Object options = info.Length() >= 4 && info[3].IsObject() ? info[3].As<Napi::Object>()
                                                                  : Napi::Object::New(env);

  SonareMixer* mixer =
      sonare_mixer_create(sample_rate, static_cast<int>(std::max<size_t>(1, length)));
  if (mixer == nullptr) {
    Napi::Error::New(env, "failed to create mixer").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<SonareStrip*> strips;
  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  try {
    for (size_t index = 0; index < count; ++index) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer, ("strip" + std::to_string(index)).c_str());
      if (strip == nullptr) {
        throw std::runtime_error("failed to add mixer strip");
      }
      strips.push_back(strip);
      Napi::Value inputTrim = OptionAt(env, options, "inputTrimDb", index);
      if (inputTrim.IsNumber()) {
        SonareError err =
            sonare_strip_set_input_trim_db(strip, inputTrim.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK)
          throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      }
      Napi::Value fader = OptionAt(env, options, "faderDb", index);
      if (fader.IsNumber()) {
        SonareError err = sonare_strip_set_fader_db(strip, fader.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK)
          throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      }
      Napi::Value pan = OptionAt(env, options, "pan", index);
      if (pan.IsNumber()) {
        Napi::Value mode = OptionAt(env, options, "panMode", index);
        SonareError err =
            sonare_strip_set_pan(strip, pan.As<Napi::Number>().FloatValue(), PanModeValue(mode));
        if (err != SONARE_OK)
          throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      }
      Napi::Value width = OptionAt(env, options, "width", index);
      if (width.IsNumber()) {
        SonareError err = sonare_strip_set_width(strip, width.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK)
          throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      }
      Napi::Value muted = OptionAt(env, options, "muted", index);
      if (muted.IsBoolean()) {
        SonareError err = sonare_strip_set_muted(strip, muted.As<Napi::Boolean>().Value() ? 1 : 0);
        if (err != SONARE_OK)
          throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      }
    }

    SonareError err = sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(), count,
                                                  out_left.data(), out_right.data(), length);
    if (err != SONARE_OK)
      throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));

    // Per-strip meter snapshots. NOTE: the integrating fields
    // (momentaryLufs / shortTermLufs / integratedLufs / truePeakDb*) require
    // sustained streaming to converge; on a short one-shot mix they have not
    // accumulated enough signal and read the -120 dB floor sentinel. Use the
    // streaming Mixer for meaningful loudness/true-peak readings.
    Napi::Array meters = Napi::Array::New(env, strips.size());
    for (size_t index = 0; index < strips.size(); ++index) {
      SonareMixMeterSnapshot snapshot{};
      err = sonare_strip_meter(strips[index], &snapshot);
      if (err != SONARE_OK)
        throw sonare::SonareException(sonare_node::CodeFromCError(err), ErrorMessageForCode(err));
      meters.Set(index, MixMeterToObject(env, snapshot));
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("left", VecToFloat32(env, out_left));
    out.Set("right", VecToFloat32(env, out_right));
    out.Set("sampleRate", sample_rate);
    out.Set("meters", meters);
    sonare_mixer_destroy(mixer);
    return out;
  } catch (const std::exception& e) {
    sonare_mixer_destroy(mixer);
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
}
