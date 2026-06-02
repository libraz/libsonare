#include <algorithm>
#include <cmath>
#include <vector>

#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/room_estimator.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

// Acoustic sample-rate bounds, kept in sync with the C ABI's
// sonare_c_detail::kMinSampleRate / kMaxSampleRate so every binding rejects the
// same out-of-range rates (the C++ functions are otherwise called directly).
constexpr int kAcousticMinSampleRate = 8000;
constexpr int kAcousticMaxSampleRate = 384000;

// Throws (via ThrowAsJavaScriptException) and returns false when the rate is out
// of range, mirroring the C ABI's validate_audio_params bound.
bool ValidateAcousticSampleRate(const Napi::Env& env, int sample_rate) {
  if (sample_rate < kAcousticMinSampleRate || sample_rate > kAcousticMaxSampleRate) {
    Napi::RangeError::New(env, "sampleRate out of supported range [8000, 384000]")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

// Rejects an empty input buffer and any non-finite sample, matching the C ABI's
// validate_audio_params contract for the estimate/morph entry points.
bool ValidateAcousticInput(const Napi::Env& env, const float* data, size_t length) {
  if (data == nullptr || length == 0) {
    Napi::RangeError::New(env, "input buffer is empty").ThrowAsJavaScriptException();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!std::isfinite(data[i])) {
      Napi::RangeError::New(env, "input contains NaN or Inf samples").ThrowAsJavaScriptException();
      return false;
    }
  }
  return true;
}

// Builds a uniform-absorption shoebox + placement from a JS options object.
sonare::acoustic::ShoeboxRoom RoomFromOptions(const Napi::Object& opts, float def_absorption) {
  return sonare::acoustic::uniform_shoebox(
      {node_float_option(opts, "lengthM", 7.0f), node_float_option(opts, "widthM", 5.0f),
       node_float_option(opts, "heightM", 3.0f)},
      node_float_option(opts, "absorption", def_absorption));
}

sonare::acoustic::SourceListener PlacementFromOptions(const Napi::Object& opts) {
  return {{node_float_option(opts, "sourceX", 1.0f), node_float_option(opts, "sourceY", 1.0f),
           node_float_option(opts, "sourceZ", 1.2f)},
          {node_float_option(opts, "listenerX", 5.0f), node_float_option(opts, "listenerY", 4.0f),
           node_float_option(opts, "listenerZ", 1.7f)}};
}

std::vector<float> AudioToVector(const sonare::Audio& audio) {
  if (audio.empty()) return {};
  return std::vector<float>(audio.data(), audio.data() + audio.size());
}

}  // namespace

Napi::Value SonareWrap::SynthesizeRir(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected (options) object").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  Napi::Object opts = info[0].As<Napi::Object>();
  const int sample_rate = node_int_option(opts, "sampleRate", 48000);
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  sonare::acoustic::RirSynthConfig cfg;
  cfg.ism_order = std::max(0, node_int_option(opts, "ismOrder", cfg.ism_order));
  cfg.late_model = node_bool_option(opts, "preferEyring", true)
                       ? sonare::acoustic::ReverbModel::Eyring
                       : sonare::acoustic::ReverbModel::Sabine;
  cfg.seed = static_cast<unsigned>(std::max(0, node_int_option(opts, "seed", 1)));
  cfg.max_seconds = node_float_option(opts, "maxSeconds", cfg.max_seconds);
  cfg.mixing_time_ms = node_float_option(opts, "mixingTimeMs", cfg.mixing_time_ms);
  cfg.crossfade_ms = node_float_option(opts, "crossfadeMs", cfg.crossfade_ms);

  const auto result = sonare::acoustic::synthesize_rir(
      RoomFromOptions(opts, 0.2f), PlacementFromOptions(opts), sample_rate, cfg);
  std::vector<float> rir = AudioToVector(result.rir);

  Napi::Object out = Napi::Object::New(env);
  out.Set("rir", VecToFloat32(env, rir));
  out.Set("sampleRate", Napi::Number::New(env, result.rir.sample_rate()));
  out.Set("hasError", Napi::Boolean::New(env, sonare::has_error(result.diagnostics)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  if (!ValidateAcousticSampleRate(env, sample_rate)) return env.Undefined();
  if (!ValidateAcousticInput(env, typed.Data(), typed.ElementLength())) return env.Undefined();
  const sonare::Audio audio =
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate);
  Napi::Object opts = info.Length() >= 3 && info[2].IsObject() ? info[2].As<Napi::Object>()
                                                               : Napi::Object::New(env);

  sonare::RoomEstimateConfig cfg;
  cfg.aspect_hint_lw = node_float_option(opts, "aspectHintLw", cfg.aspect_hint_lw);
  cfg.aspect_hint_lh = node_float_option(opts, "aspectHintLh", cfg.aspect_hint_lh);
  cfg.reference_absorption =
      node_float_option(opts, "referenceAbsorption", cfg.reference_absorption);
  cfg.prefer_eyring = node_bool_option(opts, "preferEyring", true);
  const int n_bands = node_int_option(opts, "nOctaveBands", 0);
  if (n_bands > 0) cfg.acoustic.n_octave_bands = n_bands;
  const float min_decay_db = node_float_option(opts, "minDecayDb", 0.0f);
  if (min_decay_db > 0.0f) cfg.acoustic.min_decay_db = min_decay_db;
  const float noise_floor_margin_db = node_float_option(opts, "noiseFloorMarginDb", 0.0f);
  if (noise_floor_margin_db > 0.0f) cfg.acoustic.noise_floor_margin_db = noise_floor_margin_db;
  switch (node_int_option(opts, "mode", 0)) {
    case 1:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::Blind;
      break;
    case 2:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::ImpulseResponse;
      break;
    default:
      cfg.acoustic.mode = sonare::AcousticConfig::Mode::Auto;
      break;
  }

  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  // The estimator always returns equal-length band vectors; report both at the
  // shared min length so consumers see the same band count as the C ABI/Python.
  const size_t band_count = std::min(est.absorption_bands.size(), est.rt60_bands.size());
  std::vector<float> absorption_bands(est.absorption_bands.begin(),
                                      est.absorption_bands.begin() + band_count);
  std::vector<float> rt60_bands(est.rt60_bands.begin(), est.rt60_bands.begin() + band_count);
  Napi::Object out = Napi::Object::New(env);
  out.Set("volume", Napi::Number::New(env, est.volume));
  out.Set("length", Napi::Number::New(env, est.dims.length));
  out.Set("width", Napi::Number::New(env, est.dims.width));
  out.Set("height", Napi::Number::New(env, est.dims.height));
  out.Set("drrDb", Napi::Number::New(env, est.drr_db));
  out.Set("confidence", Napi::Number::New(env, est.confidence));
  out.Set("absorptionBands", VecToFloat32(env, absorption_bands));
  out.Set("rt60Bands", VecToFloat32(env, rt60_bands));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RoomMorph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsObject()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  if (!ValidateAcousticSampleRate(env, sr)) return env.Undefined();
  if (!ValidateAcousticInput(env, typed.Data(), typed.ElementLength())) return env.Undefined();
  const sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  Napi::Object opts = info[2].As<Napi::Object>();

  sonare::effects::acoustic::RoomMorphConfig cfg;
  cfg.target = RoomFromOptions(opts, 0.2f);
  cfg.placement = PlacementFromOptions(opts);
  cfg.source_tail_suppression =
      node_float_option(opts, "sourceTailSuppression", cfg.source_tail_suppression);
  cfg.wet = node_float_option(opts, "wet", cfg.wet);
  cfg.ism_order = std::max(0, node_int_option(opts, "ismOrder", cfg.ism_order));
  cfg.seed = static_cast<unsigned>(std::max(0, node_int_option(opts, "seed", 1)));
  cfg.max_seconds = node_float_option(opts, "maxSeconds", cfg.max_seconds);

  const sonare::Audio result = sonare::effects::acoustic::room_morph(audio, cfg);
  std::vector<float> out = AudioToVector(result);
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}
