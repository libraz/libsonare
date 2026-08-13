#include <sonare/sonare_c.h>

#include <cmath>

#include "engine/realtime_engine.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_GRAPH)
#include <memory>
#include <string>

#include "graph/connection.h"
#include "graph/graph.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_engine_detail;

#if defined(SONARE_WITH_GRAPH)
namespace {

std::unique_ptr<rt::ProcessorBase> make_graph_processor(const SonareEngineGraphNode& node) {
  switch (node.type) {
    case 0:
      return std::make_unique<rt::PassProcessor>();
    case 1:
      return std::make_unique<rt::GainProcessor>(node.gain_db);
    default:
      return nullptr;
  }
}

}  // namespace
#endif

SonareError sonare_engine_set_capture_buffer(SonareRealtimeEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer) {
  SONARE_C_API_ENTRY;
  if (!engine || !buffer || !buffer->channels || buffer->num_channels <= 0 ||
      buffer->capacity_frames <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (int ch = 0; ch < buffer->num_channels; ++ch) {
    if (!buffer->channels[ch]) return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_segment(
      {buffer->channels, buffer->num_channels, buffer->capacity_frames});
  return SONARE_OK;
}

SonareError sonare_engine_arm_capture(SonareRealtimeEngine* engine, int armed) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_capture_armed(armed != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_punch(SonareRealtimeEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled) {
  SONARE_C_API_ENTRY;
  if (!engine || start_sample < 0 || end_sample < start_sample) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_punch(start_sample, end_sample, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_source(SonareRealtimeEngine* engine,
                                             SonareEngineCaptureSource source) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  switch (source) {
    case SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kOutput);
      return SONARE_OK;
    case SONARE_ENGINE_CAPTURE_SOURCE_INPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kInput);
      return SONARE_OK;
    default:
      return SONARE_ERROR_INVALID_PARAMETER;
  }
}

SonareError sonare_engine_set_record_offset_samples(SonareRealtimeEngine* engine,
                                                    int64_t offset_samples) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_record_offset_samples(offset_samples);
  return SONARE_OK;
}

SonareError sonare_engine_set_input_monitor(SonareRealtimeEngine* engine, int enabled, float gain) {
  SONARE_C_API_ENTRY;
  if (!engine || !std::isfinite(gain)) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.set_input_monitor(enabled != 0, gain);
  return SONARE_OK;
}

SonareError sonare_engine_reset_capture(SonareRealtimeEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  engine->engine.reset_capture();
  return SONARE_OK;
}

SonareError sonare_engine_capture_status(SonareRealtimeEngine* engine,
                                         SonareEngineCaptureStatus* out) {
  SONARE_C_API_ENTRY;
  if (!engine || !out) return SONARE_ERROR_INVALID_PARAMETER;
  out->captured_frames = engine->engine.captured_frames();
  out->overflow_count = engine->engine.capture_overflow_count();
  out->armed = engine->engine.capture_armed() ? 1 : 0;
  out->punch_enabled = engine->engine.capture_punch_enabled() ? 1 : 0;
  out->source = engine->engine.capture_source() == engine::CaptureSource::kInput
                    ? SONARE_ENGINE_CAPTURE_SOURCE_INPUT
                    : SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
  out->record_offset_samples = engine->engine.record_offset_samples();
  return SONARE_OK;
}

SonareError sonare_engine_set_graph(SonareRealtimeEngine* engine,
                                    const SonareEngineGraphSpec* spec) {
  SONARE_C_API_ENTRY;
  if (!engine || !spec || !spec->nodes || spec->node_count == 0 || spec->num_channels <= 0 ||
      (spec->connection_count > 0 && !spec->connections) ||
      (spec->parameter_binding_count > 0 && !spec->parameter_bindings)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  SONARE_C_TRY
  auto graph = std::make_unique<graph::Graph>();
  for (size_t i = 0; i < spec->node_count; ++i) {
    const SonareEngineGraphNode& node = spec->nodes[i];
    const int ports = node.num_ports > 0 ? node.num_ports : spec->num_channels;
    auto processor = make_graph_processor(node);
    if (!processor ||
        !graph->add_node(fixed_text(node.id, sizeof(node.id)), std::move(processor), ports)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  for (size_t i = 0; i < spec->connection_count; ++i) {
    const SonareEngineGraphConnection& connection = spec->connections[i];
    graph::Connection graph_connection{};
    graph_connection.source_node =
        fixed_text(connection.source_node, sizeof(connection.source_node));
    graph_connection.source_port = connection.source_port;
    graph_connection.dest_node = fixed_text(connection.dest_node, sizeof(connection.dest_node));
    graph_connection.dest_port = connection.dest_port;
    graph_connection.mix =
        connection.mix == 0 ? graph::Connection::Mix::Replace : graph::Connection::Mix::Add;
    if (!graph->connect(std::move(graph_connection))) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  if (!graph->compile()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto state = engine->engine.transport().snapshot_control();
  graph->prepare(state.sample_rate, engine->engine.max_block_size());
  const std::string input_node = fixed_text(spec->input_node, sizeof(spec->input_node));
  const std::string output_node = fixed_text(spec->output_node, sizeof(spec->output_node));
  std::vector<engine::GraphRuntime::ParameterBinding> parameter_bindings;
  parameter_bindings.reserve(spec->parameter_binding_count);
  for (size_t i = 0; i < spec->parameter_binding_count; ++i) {
    const SonareEngineGraphParameterBinding& binding = spec->parameter_bindings[i];
    parameter_bindings.push_back(
        {binding.param_id, fixed_text(binding.node_id, sizeof(binding.node_id))});
  }
  if (!engine->engine.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(),
                                 spec->num_channels, std::move(parameter_bindings))) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_node_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_node_count();
  return SONARE_OK;
#else
  // Match sonare_engine_set_graph: a feature-off build cannot have a graph, so
  // report NOT_SUPPORTED rather than SONARE_OK with 0, which a caller cannot
  // distinguish from "the feature is compiled in and the graph has no nodes".
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_connection_count(SonareRealtimeEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_connection_count();
  return SONARE_OK;
#else
  // See sonare_engine_graph_node_count.
  *out_count = 0;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
