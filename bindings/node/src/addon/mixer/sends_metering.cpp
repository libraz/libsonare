#include <string>
#include <vector>

#include "sonare_wrap_mixer.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

Napi::Object MeterSnapshotToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot) {
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

  // Per-plane surround meters: expose [0, channelCount) as arrays alongside the
  // stereo convenience fields above. Clamp to the array capacity so a
  // future/out-of-range channel_count can never index past the fixed buffers.
  int channel_count = snapshot.channel_count;
  if (channel_count < 0) channel_count = 0;
  if (channel_count > SONARE_MAX_METER_CHANNELS) channel_count = SONARE_MAX_METER_CHANNELS;
  out.Set("channelCount", Napi::Number::New(env, channel_count));
  Napi::Array peak_db = Napi::Array::New(env, channel_count);
  Napi::Array rms_db = Napi::Array::New(env, channel_count);
  Napi::Array true_peak_db = Napi::Array::New(env, channel_count);
  for (int ch = 0; ch < channel_count; ++ch) {
    peak_db.Set(ch, Napi::Number::New(env, snapshot.peak_db[ch]));
    rms_db.Set(ch, Napi::Number::New(env, snapshot.rms_db[ch]));
    true_peak_db.Set(ch, Napi::Number::New(env, snapshot.true_peak_db[ch]));
  }
  out.Set("peakDb", peak_db);
  out.Set("rmsDb", rms_db);
  out.Set("truePeakDb", true_peak_db);
  return out;
}

}  // namespace

Napi::Value MixerWrap::AddSend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsString() || !info[2].IsString()) {
    Napi::TypeError::New(
        env, "Expected (strip, sendId: string, destinationBusId: string, sendDb?, timing?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const std::string send_id = info[1].As<Napi::String>().Utf8Value();
  const std::string destination_bus_id = info[2].As<Napi::String>().Utf8Value();
  const float send_db =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  const int timing =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 0;

  size_t index = 0;
  SonareError err = sonare_strip_add_send(strip, send_id.c_str(), destination_bus_id.c_str(),
                                          send_db, timing, &index);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to add strip send: ");
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(index));
}

Napi::Value MixerWrap::SetSendDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, sendIndex: number, sendDb: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const size_t send_index = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());
  SonareError err =
      sonare_strip_set_send_db(strip, send_index, info[2].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set strip send level: ");
  }
  return env.Undefined();
}

Napi::Value MixerWrap::RemoveSend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, sendIndex: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const unsigned int send_index =
      static_cast<unsigned int>(info[1].As<Napi::Number>().Int64Value());
  SonareError err = sonare_strip_remove_send(strip, send_index);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to remove strip send: ");
  }
  return env.Undefined();
}

Napi::Value MixerWrap::StripMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected (strip)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareMixMeterSnapshot snapshot{};
  SonareError err = sonare_strip_meter(strip, &snapshot);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to read strip meter: ");
    return env.Undefined();
  }
  return MeterSnapshotToObject(env, snapshot);
}

Napi::Value MixerWrap::MeterTap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, tap: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareMixMeterSnapshot snapshot{};
  SonareError err =
      sonare_strip_meter_tap(strip, info[1].As<Napi::Number>().Int32Value(), &snapshot);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to read strip meter tap: ");
    return env.Undefined();
  }
  return MeterSnapshotToObject(env, snapshot);
}

Napi::Value MixerWrap::ReadGoniometerLatest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, maxPoints: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t requested = info[1].As<Napi::Number>().Int64Value();
  const size_t max_points = requested > 0 ? static_cast<size_t>(requested) : 0;
  std::vector<SonareMixGoniometerPoint> points(max_points);
  const size_t count = sonare_strip_read_goniometer_latest(
      strip, max_points > 0 ? points.data() : nullptr, max_points);
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Object point = Napi::Object::New(env);
    point.Set("left", points[index].left);
    point.Set("right", points[index].right);
    out.Set(index, point);
  }
  return out;
}

Napi::Value MixerWrap::StripById(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
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
}

}  // namespace sonare_node
