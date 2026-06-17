#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

Napi::Value RealtimeEngineWrap::SetGraph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a graph spec object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object spec_obj = info[0].As<Napi::Object>();
  Napi::Array node_input = spec_obj.Get("nodes").As<Napi::Array>();
  Napi::Array connection_input = spec_obj.Get("connections").As<Napi::Array>();

  std::vector<SonareEngineGraphNode> nodes;
  nodes.reserve(node_input.Length());
  for (uint32_t i = 0; i < node_input.Length(); ++i) {
    Napi::Object obj = node_input.Get(i).As<Napi::Object>();
    SonareEngineGraphNode node{};
    CopyString(node.id, sizeof(node.id), obj.Get("id").As<Napi::String>().Utf8Value());
    node.type = obj.Has("type") && !obj.Get("type").IsUndefined()
                    ? obj.Get("type").As<Napi::Number>().Int32Value()
                    : 0;
    node.gain_db = obj.Has("gainDb") && !obj.Get("gainDb").IsUndefined()
                       ? obj.Get("gainDb").As<Napi::Number>().FloatValue()
                       : 0.0f;
    node.num_ports = obj.Has("numPorts") && !obj.Get("numPorts").IsUndefined()
                         ? obj.Get("numPorts").As<Napi::Number>().Int32Value()
                         : 0;
    nodes.push_back(node);
  }

  std::vector<SonareEngineGraphConnection> connections;
  connections.reserve(connection_input.Length());
  for (uint32_t i = 0; i < connection_input.Length(); ++i) {
    Napi::Object obj = connection_input.Get(i).As<Napi::Object>();
    SonareEngineGraphConnection connection{};
    CopyString(connection.source_node, sizeof(connection.source_node),
               obj.Get("sourceNode").As<Napi::String>().Utf8Value());
    connection.source_port = obj.Get("sourcePort").As<Napi::Number>().Int32Value();
    CopyString(connection.dest_node, sizeof(connection.dest_node),
               obj.Get("destNode").As<Napi::String>().Utf8Value());
    connection.dest_port = obj.Get("destPort").As<Napi::Number>().Int32Value();
    connection.mix = obj.Has("mix") && !obj.Get("mix").IsUndefined()
                         ? obj.Get("mix").As<Napi::Number>().Int32Value()
                         : 1;
    connections.push_back(connection);
  }

  std::vector<SonareEngineGraphParameterBinding> parameter_bindings;
  if (spec_obj.Has("parameterBindings") && !spec_obj.Get("parameterBindings").IsUndefined()) {
    Napi::Array binding_input = spec_obj.Get("parameterBindings").As<Napi::Array>();
    parameter_bindings.reserve(binding_input.Length());
    for (uint32_t i = 0; i < binding_input.Length(); ++i) {
      Napi::Object obj = binding_input.Get(i).As<Napi::Object>();
      SonareEngineGraphParameterBinding binding{};
      binding.param_id = obj.Get("paramId").As<Napi::Number>().Uint32Value();
      CopyString(binding.node_id, sizeof(binding.node_id),
                 obj.Get("nodeId").As<Napi::String>().Utf8Value());
      parameter_bindings.push_back(binding);
    }
  }

  SonareEngineGraphSpec spec{};
  spec.nodes = nodes.data();
  spec.node_count = nodes.size();
  spec.connections = connections.data();
  spec.connection_count = connections.size();
  spec.parameter_bindings = parameter_bindings.data();
  spec.parameter_binding_count = parameter_bindings.size();
  CopyString(spec.input_node, sizeof(spec.input_node),
             spec_obj.Get("inputNode").As<Napi::String>().Utf8Value());
  CopyString(spec.output_node, sizeof(spec.output_node),
             spec_obj.Get("outputNode").As<Napi::String>().Utf8Value());
  spec.num_channels = spec_obj.Has("numChannels") && !spec_obj.Get("numChannels").IsUndefined()
                          ? spec_obj.Get("numChannels").As<Napi::Number>().Int32Value()
                          : 2;
  ThrowIfError(env, sonare_engine_set_graph(engine_, &spec));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::GraphNodeCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_graph_node_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::GraphConnectionCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_graph_connection_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::Process(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_process(engine_, block.pointers.data(),
                                          static_cast<int>(block.pointers.size()), block.frames));
  if (env.IsExceptionPending()) return env.Undefined();
  return ChannelsToJs(env, block);
}

Napi::Value RealtimeEngineWrap::ProcessWithMonitor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();

  ChannelBlock monitor;
  monitor.frames = block.frames;
  monitor.storage.resize(block.storage.size());
  monitor.pointers.reserve(block.storage.size());
  for (size_t ch = 0; ch < block.storage.size(); ++ch) {
    monitor.storage[ch].assign(static_cast<size_t>(block.frames), 0.0f);
    monitor.pointers.push_back(monitor.storage[ch].data());
  }

  ThrowIfError(env, sonare_engine_process_with_monitor(
                        engine_, block.pointers.data(), monitor.pointers.data(),
                        static_cast<int>(block.pointers.size()), block.frames));
  if (env.IsExceptionPending()) return env.Undefined();

  Napi::Object result = Napi::Object::New(env);
  result.Set("output", ChannelsToJs(env, block));
  result.Set("monitor", ChannelsToJs(env, monitor));
  return result;
}

Napi::Value RealtimeEngineWrap::RenderOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ChannelBlock block = ReadChannels(info, 0);
  if (env.IsExceptionPending()) return env.Undefined();
  const int block_size =
      info.Length() > 1 && !info[1].IsUndefined() ? info[1].As<Napi::Number>().Int32Value() : 128;
  ThrowIfError(env, sonare_engine_render_offline(engine_, block.pointers.data(),
                                                 static_cast<int>(block.pointers.size()),
                                                 block.frames, block_size));
  if (env.IsExceptionPending()) return env.Undefined();
  return ChannelsToJs(env, block);
}

Napi::Value RealtimeEngineWrap::BounceOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a bounce options object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineBounceOptions options{};
  options.total_frames = Int64Property(obj, "totalFrames", 0);
  options.block_size = IntProperty(obj, "blockSize", 128);
  options.num_channels = IntProperty(obj, "numChannels", 2);
  options.target_sample_rate = IntProperty(obj, "targetSampleRate", 48000);
  options.source_sample_rate = IntProperty(obj, "sourceSampleRate", 48000);
  options.normalize_lufs = BoolProperty(obj, "normalizeLufs", false) ? 1 : 0;
  options.target_lufs = FloatProperty(obj, "targetLufs", -14.0f);
  options.dither = IntProperty(obj, "dither", 0);
  options.dither_bits = IntProperty(obj, "ditherBits", 16);
  options.dither_seed = static_cast<uint32_t>(Int64Property(obj, "ditherSeed", 0));
  SonareEngineBounceResult result{};
  ThrowIfError(env, sonare_engine_bounce_offline(engine_, &options, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Float32Array interleaved = Napi::Float32Array::New(env, result.sample_count);
  if (result.sample_count > 0 && result.interleaved != nullptr) {
    std::memcpy(interleaved.Data(), result.interleaved, result.sample_count * sizeof(float));
  }
  // Capture scalars before freeing: the free contract only promises to release
  // the owned buffers, not to keep scalar fields readable afterwards.
  const int64_t frames = result.frames;
  const int num_channels = result.num_channels;
  const int sample_rate = result.sample_rate;
  const float integrated_lufs = result.integrated_lufs;
  sonare_free_bounce_result(&result);
  Napi::Object out = Napi::Object::New(env);
  out.Set("interleaved", interleaved);
  out.Set("frames", Napi::Number::New(env, static_cast<double>(frames)));
  out.Set("numChannels", Napi::Number::New(env, num_channels));
  out.Set("sampleRate", Napi::Number::New(env, sample_rate));
  out.Set("integratedLufs", Napi::Number::New(env, integrated_lufs));
  return out;
}

Napi::Value RealtimeEngineWrap::FreezeOffline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a freeze options object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareEngineFreezeOptions options{};
  options.total_frames = Int64Property(obj, "totalFrames", 0);
  options.block_size = IntProperty(obj, "blockSize", 128);
  options.num_channels = IntProperty(obj, "numChannels", 2);
  options.clip_id = static_cast<uint32_t>(Int64Property(obj, "clipId", 1));
  options.start_ppq = sonare_node::DoubleProperty(obj, "startPpq", 0.0);
  options.gain = FloatProperty(obj, "gain", 1.0f);
  SonareEngineFreezeResult result{};
  ThrowIfError(env, sonare_engine_freeze_offline(engine_, &options, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, result.clip_id));
  out.Set("frames", Napi::Number::New(env, static_cast<double>(result.frames)));
  out.Set("numChannels", Napi::Number::New(env, result.num_channels));
  return out;
}

Napi::Value RealtimeEngineWrap::DrainTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareEngineTelemetry> records(max_records);
  size_t written = 0;
  ThrowIfError(env,
               sonare_engine_drain_telemetry(engine_, records.data(), records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), TelemetryToObject(env, records[i]));
  }
  return out;
}

Napi::Value RealtimeEngineWrap::DrainMeterTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareMeterTelemetryRecord> records(max_records);
  size_t written = 0;
  ThrowIfError(
      env, sonare_engine_drain_meter_telemetry(engine_, records.data(), records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), MeterTelemetryToObject(env, records[i]));
  }
  return out;
}

Napi::Value RealtimeEngineWrap::DrainMeterTelemetryWide(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareMeterTelemetryRecordWide> records(max_records);
  size_t written = 0;
  ThrowIfError(env, sonare_engine_drain_meter_telemetry_wide(engine_, records.data(),
                                                             records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), MeterTelemetryWideToObject(env, records[i]));
  }
  return out;
}

Napi::Value RealtimeEngineWrap::ConfigureScopeTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int interval_frames = info[0].As<Napi::Number>().Int32Value();
  const unsigned int band_count = info[1].As<Napi::Number>().Uint32Value();
  unsigned int applied = 0;
  ThrowIfError(
      env, sonare_engine_configure_scope_telemetry(engine_, interval_frames, band_count, &applied));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, applied);
}

Napi::Value RealtimeEngineWrap::DrainScopeTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t max_records = info.Length() > 0 && !info[0].IsUndefined()
                                 ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                                 : 1024;
  std::vector<SonareScopeTelemetryRecord> records(max_records);
  size_t written = 0;
  ThrowIfError(
      env, sonare_engine_drain_scope_telemetry(engine_, records.data(), records.size(), &written));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array out = Napi::Array::New(env, written);
  for (size_t i = 0; i < written; ++i) {
    out.Set(static_cast<uint32_t>(i), ScopeTelemetryToObject(env, records[i]));
  }
  return out;
}
