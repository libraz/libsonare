#include <string>

#include "sonare_wrap_mixer.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

SonareStrip* MixerWrap::ResolveStrip(const Napi::CallbackInfo& info, const Napi::Value& ref) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return nullptr;
  }
  SonareStrip* strip = nullptr;
  if (ref.IsNumber()) {
    const size_t index = static_cast<size_t>(node_narrow_int64(env, ref, "strip"));
    strip = sonare_mixer_strip_at(mixer_, index);
    if (strip == nullptr) {
      Napi::Error::New(env, "mixer strip index out of range").ThrowAsJavaScriptException();
    }
  } else if (ref.IsString()) {
    const std::string id = ref.As<Napi::String>().Utf8Value();
    strip = sonare_mixer_strip_by_id(mixer_, id.c_str());
    if (strip == nullptr) {
      Napi::Error::New(env, std::string("mixer strip not found: ") + id)
          .ThrowAsJavaScriptException();
    }
  } else {
    Napi::TypeError::New(env, "strip reference must be a number (index) or string (id)")
        .ThrowAsJavaScriptException();
  }
  return strip;
}

Napi::Value MixerWrap::SetInputTrimDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, db: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_input_trim_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip input trim: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetFaderDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, db: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_fader_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip fader: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, pan: number, panMode?: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  // Omitting panMode passes SONARE_PAN_MODE_KEEP (-1) so a plain pan nudge does
  // not reset a scene strip's current pan mode.
  const int pan_mode = node_arg_int(info, 2, -1);
  SonareError err = sonare_strip_set_pan(strip, info[1].As<Napi::Number>().FloatValue(), pan_mode);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip pan: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetWidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, width: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_width(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip width: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetMuted(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, muted: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_muted(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip muted: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSoloed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, soloed: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_soloed(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip solo: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSoloSafe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, soloSafe: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_solo_safe(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip solo-safe: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPolarityInvert(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[1].IsBoolean() || !info[2].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, invertLeft: boolean, invertRight: boolean)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_polarity_invert(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0,
                                       info[2].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip polarity: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPanLaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, panLaw: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_pan_law(
      strip, sonare_node::node_narrow_int(env, info[1], sonare_node::node_arg_label(1).c_str()));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip pan law: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetChannelDelaySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, delaySamples: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_channel_delay_samples(
      strip, sonare_node::node_narrow_int(env, info[1], sonare_node::node_arg_label(1).c_str()));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip channel delay: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetVcaOffsetDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, offsetDb: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_vca_offset_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip VCA offset: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetDualPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, leftPan: number, rightPan: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_dual_pan(strip, info[1].As<Napi::Number>().FloatValue(),
                                              info[2].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip dual pan: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSurroundPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 2 || !info[1].IsObject()) {
    Napi::TypeError::New(env, "Expected (strip, pan: SurroundPan)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const Napi::Object obj = info[1].As<Napi::Object>();
  SonareSurroundPan pan{};
  pan.azimuth = node_float_option(obj, "azimuth", 0.0f);
  pan.elevation = node_float_option(obj, "elevation", 0.0f);
  pan.divergence = node_float_option(obj, "divergence", 0.0f);
  pan.lfe = node_float_option(obj, "lfe", 0.0f);
  pan.distance = node_float_option(obj, "distance", 1.0f);
  SonareError err = sonare_strip_set_surround_pan(strip, &pan);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip surround pan: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
