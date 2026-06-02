#include <algorithm>
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
  sonare::acoustic::RirSynthConfig cfg;
  cfg.ism_order = std::max(0, node_int_option(opts, "ismOrder", cfg.ism_order));
  cfg.seed = static_cast<unsigned>(std::max(0, node_int_option(opts, "seed", 1)));
  cfg.max_seconds = node_float_option(opts, "maxSeconds", cfg.max_seconds);

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
  const sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(),
                                                         info[1].As<Napi::Number>().Int32Value());
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

  const sonare::RoomEstimate est = sonare::estimate_room(audio, cfg);
  Napi::Object out = Napi::Object::New(env);
  out.Set("volume", Napi::Number::New(env, est.volume));
  out.Set("length", Napi::Number::New(env, est.dims.length));
  out.Set("width", Napi::Number::New(env, est.dims.width));
  out.Set("height", Napi::Number::New(env, est.dims.height));
  out.Set("drrDb", Napi::Number::New(env, est.drr_db));
  out.Set("confidence", Napi::Number::New(env, est.confidence));
  out.Set("absorptionBands", VecToFloat32(env, est.absorption_bands));
  out.Set("rt60Bands", VecToFloat32(env, est.rt60_bands));
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
