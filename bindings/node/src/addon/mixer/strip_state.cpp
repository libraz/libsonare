#include <string>

#include "c_api/engine_config_domains.h"
#include "sonare_wrap_mixer.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

SonareStrip* MixerWrap::ResolveStrip(const Napi::CallbackInfo& info, const Napi::Value& ref) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    sonare_node::ThrowHandleClosed(env, "Mixer");
    return nullptr;
  }
  SonareStrip* strip = nullptr;
  if (ref.IsNumber()) {
    const size_t index = node_narrow_uint32(env, ref, "strip");
    strip = sonare_mixer_strip_at(mixer_, index);
    if (strip == nullptr) {
      sonare_node::ThrowSonareError(env, sonare_last_error_code());
    }
  } else if (ref.IsString()) {
    const std::string id = node_narrow_string(env, ref, "strip");
    strip = sonare_mixer_strip_by_id(mixer_, id.c_str());
    if (strip == nullptr) {
      sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_PARAMETER,
                                           "no strip with id '" + id + "'");
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
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "db must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_input_trim_db(strip, node_narrow_float(env, info[1], "db"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetFaderDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "db must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_fader_db(strip, node_narrow_float(env, info[1], "db"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "pan must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Omitting panMode passes SONARE_PAN_MODE_KEEP (-1) so a plain pan nudge does
  // not reset a scene strip's current pan mode.
  int pan_mode{};
  if (!OptionalIntArg(env, info, 2, "panMode", SONARE_PAN_MODE_KEEP, &pan_mode)) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_pan(strip, node_narrow_float(env, info[1], "pan"), pan_mode);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetWidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "width must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_width(strip, node_narrow_float(env, info[1], "width"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetMuted(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsBoolean()) {
    Napi::TypeError::New(env, "muted must be a boolean").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_muted(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSoloed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsBoolean()) {
    Napi::TypeError::New(env, "soloed must be a boolean").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_soloed(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSoloSafe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsBoolean()) {
    Napi::TypeError::New(env, "soloSafe must be a boolean").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_solo_safe(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPolarityInvert(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsBoolean()) {
    Napi::TypeError::New(env, "invertLeft must be a boolean").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsBoolean()) {
    Napi::TypeError::New(env, "invertRight must be a boolean").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_polarity_invert(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0,
                                       info[2].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetPanLaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "panLaw must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_pan_law(strip, sonare_node::node_narrow_int(env, info[1], "panLaw"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetChannelDelaySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "delaySamples must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_channel_delay_samples(
      strip, sonare_node::node_domain_number<int>(info[1], "delaySamples",
                                                  sonare_c_detail::kEngineAlignmentDelayDomain));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetVcaOffsetDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "offsetDb must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_vca_offset_db(strip, node_narrow_float(env, info[1], "offsetDb"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetDualPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "leftPan must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "rightPan must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_dual_pan(strip, node_narrow_float(env, info[1], "leftPan"),
                                              node_narrow_float(env, info[2], "rightPan"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSurroundPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsObject()) {
    Napi::TypeError::New(env, "pan must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object obj = info[1].As<Napi::Object>();
  SonareSurroundPan pan{};
  pan.azimuth = DomainFloatProperty(obj, "azimuth", pan.azimuth);
  pan.divergence = DomainFloatProperty(obj, "divergence", pan.divergence);
  pan.lfe = DomainFloatProperty(obj, "lfe", pan.lfe);
  if (env.IsExceptionPending()) return env.Undefined();
  sonare_node::ThrowIfErrorFlattening(env, sonare_strip_set_surround_pan(strip, &pan), "pan");
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::Settle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_settle(strip);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
