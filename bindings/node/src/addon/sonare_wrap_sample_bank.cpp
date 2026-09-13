#include "sonare_wrap_sample_bank.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::MidiByteProperty;
using sonare_node::OptionalUint32Arg;
using sonare_node::ThrowIfError;
using sonare_node::Uint32Property;

namespace {

// The class constructor, kept so a JS value can be checked with InstanceOf
// before it is unwrapped. Persistent and deliberately never released: the
// addon is loaded once per process and the reference dies with it.
Napi::FunctionReference* g_sample_bank_constructor = nullptr;

struct SampleDescLoopMode {
  const char* name;
  int value;
};

// SonareSampleDesc.loop_mode carries SoundFont sampleModes, so a NUMBER passes
// through as the raw SF2 value and SF2-derived data needs no translation. The
// names are the spellings SynthPatch.sampleLoop uses, mapped onto that scale.
constexpr SampleDescLoopMode kSampleDescLoopModes[] = {
    {"none", 0}, {"continuous", 1}, {"key-down", 3}};

// Reads SonareSampleDesc.loop_mode from either spelling. An absent value keeps
// the caller's zero-initialized default (no loop).
bool ReadSampleLoopMode(Napi::Env env, const Napi::Value& value, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  if (value.IsUndefined() || value.IsNull()) return true;
  if (value.IsNumber()) {
    *out = sonare_node::node_narrow_int(env, value, "loopMode");
    return !env.IsExceptionPending();
  }
  if (!value.IsString()) {
    Napi::TypeError::New(env, "loopMode must be a number or string").ThrowAsJavaScriptException();
    return false;
  }
  const std::string name = value.As<Napi::String>().Utf8Value();
  if (env.IsExceptionPending()) return false;
  for (const SampleDescLoopMode& entry : kSampleDescLoopModes) {
    if (name == entry.name) {
      *out = entry.value;
      return true;
    }
  }
  Napi::TypeError::New(
      env, "Unknown sample loop mode name: '" + name + "' (expected none, continuous or key-down)")
      .ThrowAsJavaScriptException();
  return false;
}

// True when @p value is a usable options bag. An absent bag leaves the C struct
// zero-initialized, which the C ABI documents as the neutral descriptor; a
// present one that is not a plain object is a caller mistake, not a default.
bool ReadOptionsBag(Napi::Env env, const Napi::Value& value, const char* what, Napi::Object* out) {
  if (env.IsExceptionPending()) return false;
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!value.IsObject() || value.IsArray()) {
    Napi::TypeError::New(env, std::string(what) + " must be an object")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Object>();
  return true;
}

}  // namespace

Napi::Object SampleBankWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "SampleBank",
                                    {
                                        InstanceMethod<&SampleBankWrap::AddSample>("addSample"),
                                        InstanceMethod<&SampleBankWrap::AddZone>("addZone"),
                                        InstanceMethod<&SampleBankWrap::SampleCount>("sampleCount"),
                                        InstanceMethod<&SampleBankWrap::SetCount>("setCount"),
                                        InstanceMethod<&SampleBankWrap::Destroy>("destroy"),
                                    });
  g_sample_bank_constructor = new Napi::FunctionReference();
  *g_sample_bank_constructor = Napi::Persistent(func);
  g_sample_bank_constructor->SuppressDestruct();
  exports.Set("SampleBank", func);
  return exports;
}

SampleBankWrap::SampleBankWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<SampleBankWrap>(info) {
  bank_ = sonare_sample_bank_create();
  if (bank_ == nullptr) {
    sonare_node::ThrowSonareError(info.Env(), SONARE_ERROR_OUT_OF_MEMORY);
  }
}

SampleBankWrap::~SampleBankWrap() {
  if (bank_ != nullptr) {
    sonare_sample_bank_destroy(bank_);
    bank_ = nullptr;
  }
}

bool SampleBankWrap::ReadHandle(Napi::Env env, const Napi::Value& value, SonareSampleBank** out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  *out = nullptr;
  if (value.IsUndefined() || value.IsNull()) return true;
  if (g_sample_bank_constructor == nullptr || !value.IsObject() ||
      !value.As<Napi::Object>().InstanceOf(g_sample_bank_constructor->Value())) {
    Napi::TypeError::New(env, "sampleBank must be a SampleBank instance")
        .ThrowAsJavaScriptException();
    return false;
  }
  SampleBankWrap* wrap = SampleBankWrap::Unwrap(value.As<Napi::Object>());
  if (wrap == nullptr || wrap->bank_ == nullptr) {
    Napi::TypeError::New(env, "sampleBank is destroyed").ThrowAsJavaScriptException();
    return false;
  }
  *out = wrap->bank_;
  return true;
}

// addSample(data, desc?) -> the new sample's index. `data` is mono float frames
// (copied into the bank, so the caller may reuse the array afterwards).
Napi::Value SampleBankWrap::AddSample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (bank_ == nullptr) {
    Napi::Error::New(env, "SampleBank is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!sonare_node::RequireFloat32Array(info, 0,
                                        "addSample expects a Float32Array of mono frames")) {
    return env.Undefined();
  }
  Napi::Float32Array data = info[0].As<Napi::Float32Array>();

  SonareSampleDesc desc{};
  Napi::Object obj;
  if (!ReadOptionsBag(env, info[1], "addSample desc", &obj)) return env.Undefined();
  if (!obj.IsEmpty()) {
    desc.root_key = MidiByteProperty(env, obj, "rootKey", 0);
    desc.fine_tune_cents = FloatProperty(obj, "fineTuneCents", 0.0f);
    desc.source_rate = DoubleProperty(obj, "sourceRate", 0.0);
    desc.loop_start = Uint32Property(obj, "loopStart", 0u);
    desc.loop_end = Uint32Property(obj, "loopEnd", 0u);
    if (env.IsExceptionPending()) return env.Undefined();
    if (!ReadSampleLoopMode(env, obj.Get("loopMode"), &desc.loop_mode)) return env.Undefined();
  }

  uint32_t index = 0;
  ThrowIfError(
      env, sonare_sample_bank_add_sample(bank_, data.Data(), data.ElementLength(), &desc, &index));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, index);
  SONARE_NODE_CATCH(env)
}

// addZone(setIndex?, zone?) — appends a key/velocity rectangle to a keymap set.
// The C ABI defaults every bound on its own, so a zero left here is a real
// "unset" for that edge alone.
Napi::Value SampleBankWrap::AddZone(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (bank_ == nullptr) {
    Napi::Error::New(env, "SampleBank is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t set_index = 0;
  if (!OptionalUint32Arg(env, info, 0, "setIndex", 0u, &set_index)) return env.Undefined();

  SonareSampleZoneDesc zone{};
  Napi::Object obj;
  if (!ReadOptionsBag(env, info[1], "addZone zone", &obj)) return env.Undefined();
  if (!obj.IsEmpty()) {
    zone.key_lo = MidiByteProperty(env, obj, "keyLo", 0);
    zone.key_hi = MidiByteProperty(env, obj, "keyHi", 0);
    zone.vel_lo = MidiByteProperty(env, obj, "velLo", 0);
    zone.vel_hi = MidiByteProperty(env, obj, "velHi", 0);
    zone.sample_index = Uint32Property(obj, "sampleIndex", 0u);
    zone.tune_cents = FloatProperty(obj, "tuneCents", 0.0f);
    zone.gain = FloatProperty(obj, "gain", 0.0f);
    zone.pan_units = FloatProperty(obj, "panUnits", 0.0f);
    if (env.IsExceptionPending()) return env.Undefined();
  }

  ThrowIfError(env, sonare_sample_bank_add_zone(bank_, set_index, &zone));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value SampleBankWrap::SampleCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (bank_ == nullptr) {
    Napi::Error::New(env, "SampleBank is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t count = 0;
  ThrowIfError(env, sonare_sample_bank_sample_count(bank_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value SampleBankWrap::SetCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (bank_ == nullptr) {
    Napi::Error::New(env, "SampleBank is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t count = 0;
  ThrowIfError(env, sonare_sample_bank_set_count(bank_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

void SampleBankWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY(void) info;
  if (bank_ != nullptr) {
    sonare_sample_bank_destroy(bank_);
    bank_ = nullptr;
  }
  SONARE_NODE_CATCH_VOID(env)
}
