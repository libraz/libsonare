#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

/// Records pulled from the engine per C-ABI call. The drains are destructive but
/// lossless per chunk (every drained record is emitted), so chunking only caps
/// the working buffer — it never drops a record.
constexpr size_t kTelemetryDrainChunk = 256;

/// Default record budget when the caller passes no maxRecords.
constexpr size_t kTelemetryDrainDefault = 1024;

/// @brief Shared body of the four telemetry drains.
/// @details `maxRecords` is a record budget, not an allocation request: the
///   working buffer is capped at kTelemetryDrainChunk so a legitimately huge
///   budget cannot reserve gigabytes up front, and an unvalidated negative value
///   can no longer wrap to SIZE_MAX and raise a std::length_error across the
///   N-API boundary (which terminates the process under
///   NAPI_DISABLE_CPP_EXCEPTIONS). Anything that is not a finite non-negative
///   integer is rejected before a single record is drained.
template <typename Record, typename Convert>
Napi::Value DrainTelemetryInto(const Napi::CallbackInfo& info, SonareRealtimeEngine* engine,
                               SonareError (*drain)(SonareRealtimeEngine*, Record*, size_t,
                                                    size_t*),
                               Convert convert) {
  Napi::Env env = info.Env();
  size_t budget = kTelemetryDrainDefault;
  if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!NonNegativeSizeTArg(env, info, 0, "maxRecords", &budget)) return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env);
  if (budget == 0) return out;

  std::vector<Record> records(std::min(budget, kTelemetryDrainChunk));
  uint32_t out_index = 0;
  while (budget > 0) {
    const size_t want = std::min(records.size(), budget);
    size_t written = 0;
    ThrowIfError(env, drain(engine, records.data(), want, &written));
    if (env.IsExceptionPending()) return env.Undefined();
    if (written == 0) break;
    // Defensive: the C ABI promises written <= want, but clamp anyway so a
    // misreporting drain cannot read past the buffer or wrap the budget.
    written = std::min(written, want);
    for (size_t i = 0; i < written; ++i) {
      out.Set(out_index++, convert(env, records[i]));
    }
    budget -= written;
  }
  return out;
}

}  // namespace

Napi::Value RealtimeEngineWrap::SetGraph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a graph spec object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object spec_obj = info[0].As<Napi::Object>();
  const Napi::Value nodes_value = spec_obj.Get("nodes");
  if (!nodes_value.IsArray()) {
    Napi::TypeError::New(env, "graph spec nodes must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Value connections_value = spec_obj.Get("connections");
  if (!connections_value.IsArray()) {
    Napi::TypeError::New(env, "graph spec connections must be an array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array node_input = nodes_value.As<Napi::Array>();
  Napi::Array connection_input = connections_value.As<Napi::Array>();

  std::string text;
  std::vector<SonareEngineGraphNode> nodes;
  nodes.reserve(node_input.Length());
  for (uint32_t i = 0; i < node_input.Length(); ++i) {
    Napi::Value entry = node_input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "graph node must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareEngineGraphNode node{};
    if (!RequiredStringProperty(env, obj, "id", &text)) return env.Undefined();
    CopyString(node.id, sizeof(node.id), text);
    node.type = IntProperty(obj, "type", 0);
    node.gain_db = FloatProperty(obj, "gainDb", 0.0f);
    node.num_ports = IntProperty(obj, "numPorts", 0);
    // A wrong-typed optional field left one pending JS exception; stop here so
    // no further N-API throw lands on top of it (that would be a fatal abort).
    if (env.IsExceptionPending()) return env.Undefined();
    nodes.push_back(node);
  }

  std::vector<SonareEngineGraphConnection> connections;
  connections.reserve(connection_input.Length());
  for (uint32_t i = 0; i < connection_input.Length(); ++i) {
    Napi::Value entry = connection_input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "graph connection must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = entry.As<Napi::Object>();
    SonareEngineGraphConnection connection{};
    if (!RequiredStringProperty(env, obj, "sourceNode", &text)) return env.Undefined();
    CopyString(connection.source_node, sizeof(connection.source_node), text);
    if (!RequiredIntProperty(env, obj, "sourcePort", &connection.source_port)) {
      return env.Undefined();
    }
    if (!RequiredStringProperty(env, obj, "destNode", &text)) return env.Undefined();
    CopyString(connection.dest_node, sizeof(connection.dest_node), text);
    if (!RequiredIntProperty(env, obj, "destPort", &connection.dest_port)) return env.Undefined();
    connection.mix = IntProperty(obj, "mix", 1);
    if (env.IsExceptionPending()) return env.Undefined();
    connections.push_back(connection);
  }

  std::vector<SonareEngineGraphParameterBinding> parameter_bindings;
  const Napi::Value bindings_value = spec_obj.Get("parameterBindings");
  if (!bindings_value.IsUndefined() && !bindings_value.IsNull()) {
    if (!bindings_value.IsArray()) {
      Napi::TypeError::New(env, "graph spec parameterBindings must be an array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Array binding_input = bindings_value.As<Napi::Array>();
    parameter_bindings.reserve(binding_input.Length());
    for (uint32_t i = 0; i < binding_input.Length(); ++i) {
      Napi::Value entry = binding_input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "graph parameter binding must be an object")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareEngineGraphParameterBinding binding{};
      if (!RequiredUint32Property(env, obj, "paramId", &binding.param_id)) return env.Undefined();
      if (!RequiredStringProperty(env, obj, "nodeId", &text)) return env.Undefined();
      CopyString(binding.node_id, sizeof(binding.node_id), text);
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
  if (!RequiredStringProperty(env, spec_obj, "inputNode", &text)) return env.Undefined();
  CopyString(spec.input_node, sizeof(spec.input_node), text);
  if (!RequiredStringProperty(env, spec_obj, "outputNode", &text)) return env.Undefined();
  CopyString(spec.output_node, sizeof(spec.output_node), text);
  spec.num_channels = IntProperty(spec_obj, "numChannels", 2);
  if (env.IsExceptionPending()) return env.Undefined();
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
  // Audio-thread entry point: its failure carries no detail message of its own,
  // so it must not be reported through the thread-local one (see
  // ThrowIfRealtimeError).
  ThrowIfRealtimeError(
      env, sonare_engine_process(engine_, block.pointers.data(),
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

  // Audio-thread entry point; see the note in Process above.
  ThrowIfRealtimeError(env, sonare_engine_process_with_monitor(
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
  int block_size = 128;
  bool finalize = true;
  if (!OptionalIntArg(env, info, 1, "blockSize", 128, &block_size) ||
      !OptionalBoolArg(env, info, 2, "finalize", true, &finalize)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_render_offline_ex(engine_, block.pointers.data(),
                                                    static_cast<int>(block.pointers.size()),
                                                    block.frames, block_size, finalize ? 1 : 0));
  if (env.IsExceptionPending()) return env.Undefined();
  return ChannelsToJs(env, block);
}

Napi::Value RealtimeEngineWrap::FinishOfflineRender(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_finish_offline_render(engine_));
  return env.Undefined();
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
  options.target_lufs = FloatProperty(obj, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
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
  return DrainTelemetryInto<SonareEngineTelemetry>(info, engine_, sonare_engine_drain_telemetry,
                                                   TelemetryToObject);
}

Napi::Value RealtimeEngineWrap::DrainMeterTelemetry(const Napi::CallbackInfo& info) {
  return DrainTelemetryInto<SonareMeterTelemetryRecord>(
      info, engine_, sonare_engine_drain_meter_telemetry, MeterTelemetryToObject);
}

Napi::Value RealtimeEngineWrap::DrainMeterTelemetryWide(const Napi::CallbackInfo& info) {
  return DrainTelemetryInto<SonareMeterTelemetryRecordWide>(
      info, engine_, sonare_engine_drain_meter_telemetry_wide, MeterTelemetryWideToObject);
}

Napi::Value RealtimeEngineWrap::ConfigureScopeTelemetry(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "configureScopeTelemetry expects (intervalFrames, bandCount) numbers")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int interval_frames = info[0].As<Napi::Number>().Int32Value();
  const unsigned int band_count = info[1].As<Napi::Number>().Uint32Value();
  unsigned int applied = 0;
  ThrowIfError(
      env, sonare_engine_configure_scope_telemetry(engine_, interval_frames, band_count, &applied));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, applied);
}

Napi::Value RealtimeEngineWrap::DrainScopeTelemetry(const Napi::CallbackInfo& info) {
  return DrainTelemetryInto<SonareScopeTelemetryRecord>(
      info, engine_, sonare_engine_drain_scope_telemetry, ScopeTelemetryToObject);
}
