#include <algorithm>
#include <string>
#include <vector>

#include "sonare_wrap_mixer.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

/// Upper bound on the goniometer working buffer, mirroring the strip ring
/// (sonare::mixing::ChannelStrip::kGoniometerCapacity, out of reach here: an
/// addon TU includes the C ABI only). One read never returns more than the ring
/// holds, so capping here drops no point: it only stops a caller-supplied
/// maxPoints from reserving gigabytes, which under NAPI_DISABLE_CPP_EXCEPTIONS
/// would surface as an uncatchable std::bad_alloc that terminates the process.
constexpr size_t kGoniometerReadCap = 4096;

}  // namespace

Napi::Value MixerWrap::AddSend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsString()) {
    Napi::TypeError::New(env, "sendId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "destinationBusId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string send_id = node_narrow_string(env, info[1], "sendId");
  const std::string destination_bus_id = node_narrow_string(env, info[2], "destinationBusId");
  float send_db{};
  if (!OptionalFloatArg(env, info, 3, "sendDb", 0.0f, &send_db)) return env.Undefined();
  int timing{};
  if (!OptionalIntArg(env, info, 4, "timing", 0, &timing)) return env.Undefined();

  size_t index = 0;
  SonareError err = sonare_strip_add_send(strip, send_id.c_str(), destination_bus_id.c_str(),
                                          send_db, timing, &index);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to add strip send: ");
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(index));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetSendDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sendIndex must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sendDb must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t send_index = sonare_node::node_narrow_uint32(env, info[1], "sendIndex");
  SonareError err =
      sonare_strip_set_send_db(strip, send_index, node_narrow_float(env, info[2], "sendDb"));
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip send level: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::RemoveSend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sendIndex must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const unsigned int send_index = sonare_node::node_narrow_uint32(env, info[1], "sendIndex");
  SonareError err = sonare_strip_remove_send(strip, send_index);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to remove strip send: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::StripMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  int tap = SONARE_METER_TAP_POST_FADER;
  if (!info[1].IsUndefined() && !info[1].IsNull()) {
    if (!info[1].IsNumber()) {
      Napi::TypeError::New(env, "tap must be a number").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    tap = sonare_node::node_narrow_int(env, info[1], "tap");
  }
  SonareMixMeterSnapshot snapshot{};
  SonareError err = sonare_strip_meter(strip, tap, &snapshot);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to read strip meter: ");
    return env.Undefined();
  }
  return MixMeterSnapshotToObject(env, snapshot);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::BusMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "busId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareMixMeterSnapshot snapshot{};
  const std::string bus_id = node_narrow_string(env, info[0], "busId");
  const SonareError err = sonare_mixer_bus_meter(mixer_, bus_id.c_str(), &snapshot);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to read bus meter: ");
    return env.Undefined();
  }
  return MixMeterSnapshotToObject(env, snapshot);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ConfigureMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  bool enabled = false;
  if (!sonare_node::RequiredBoolValue(env, info[0], "enabled", &enabled)) return env.Undefined();
  // Seeded from the C defaults; only what the caller gave overrides them.
  SonareMixerMasterMeterConfig config{};
  sonare_mixer_master_meter_config_init(&config);
  config.enabled = enabled ? 1 : 0;
  bool measure_lufs = config.measure_lufs != 0;
  bool measure_true_peak = config.measure_true_peak != 0;
  if (!sonare_node::OptionalIntArg(env, info, 1, "truePeakOversample", config.true_peak_oversample,
                                   &config.true_peak_oversample) ||
      !sonare_node::OptionalBoolArg(env, info, 2, "measureLufs", measure_lufs, &measure_lufs) ||
      !sonare_node::OptionalBoolArg(env, info, 3, "measureTruePeak", measure_true_peak,
                                    &measure_true_peak)) {
    return env.Undefined();
  }
  config.measure_lufs = measure_lufs ? 1 : 0;
  config.measure_true_peak = measure_true_peak ? 1 : 0;
  const SonareError err = sonare_mixer_configure_master_meter(mixer_, &config);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to configure the master meter: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::MeterSnapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareMixMeterSnapshot snapshot{};
  // The read carries SONARE_C_RT_API_ENTRY and records no message of its own.
  sonare_node::ThrowIfRealtimeError(env, sonare_mixer_master_meter(mixer_, &snapshot));
  if (env.IsExceptionPending()) return env.Undefined();
  return MixMeterSnapshotToObject(env, snapshot);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::BusNonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "busId must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string bus_id = node_narrow_string(env, info[0], "busId");
  size_t count = 0;
  // The entry carries SONARE_C_RT_API_ENTRY, so it records no message of its
  // own and the thread-local one would belong to an unrelated earlier call.
  sonare_node::ThrowIfRealtimeError(
      env, sonare_mixer_bus_non_finite_discard_count(mixer_, bus_id.c_str(), &count));
  if (env.IsExceptionPending()) {
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ReadGoniometerLatest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  // maxPoints is a request, not an allocation: reject anything that is not a
  // finite non-negative integer first, then size the working buffer against the
  // ring capacity rather than against the caller's number.
  size_t requested = 0;
  if (!NonNegativeSizeTArg(env, info, 1, "maxPoints", &requested)) return env.Undefined();
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const size_t max_points = std::min(requested, kGoniometerReadCap);
  std::vector<SonareMixGoniometerPoint> points(max_points);
  // Defensive: the C ABI promises a count no larger than the capacity it was
  // given, but clamp anyway so a misreporting read cannot index past the buffer.
  const size_t count = std::min(sonare_strip_read_goniometer_latest(
                                    strip, max_points > 0 ? points.data() : nullptr, max_points),
                                max_points);
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Object point = Napi::Object::New(env);
    point.Set("left", points[index].left);
    point.Set("right", points[index].right);
    out.Set(index, point);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::StripNonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  size_t count = 0;
  // Same audio-thread entry policy as the bus counterpart above.
  sonare_node::ThrowIfRealtimeError(env, sonare_strip_non_finite_discard_count(strip, &count));
  if (env.IsExceptionPending()) {
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::StripById(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "id must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  SonareStrip* strip = sonare_mixer_strip_by_id(mixer_, id.c_str());
  if (strip == nullptr) {
    return env.Null();
  }
  const size_t count = sonare_mixer_strip_count(mixer_);
  for (size_t index = 0; index < count; ++index) {
    if (sonare_mixer_strip_at(mixer_, index) == strip) {
      return Napi::Number::New(env, static_cast<double>(index));
    }
  }
  return env.Null();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
