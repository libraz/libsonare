#include <sonare/sonare_c.h>

#include <cmath>
#include <string>

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

static_assert(static_cast<int>(graph::Connection::Mix::Replace) == 0 &&
                  static_cast<int>(graph::Connection::Mix::Add) == 1,
              "SonareEngineGraphConnection::mix ordinals must mirror graph::Connection::Mix");

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

SonareError sonare_engine_set_capture_buffer(SonareEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_buffer: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!buffer) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_buffer: buffer must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!buffer->channels) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_buffer: buffer->channels must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (buffer->num_channels <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_buffer: buffer->num_channels must be positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (buffer->capacity_frames <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_buffer: buffer->capacity_frames must be positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (int ch = 0; ch < buffer->num_channels; ++ch) {
    if (!buffer->channels[ch]) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_capture_buffer: buffer->channels[" + std::to_string(ch) +
                      "] must not be NULL")
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  engine->engine.set_capture_segment(
      {buffer->channels, buffer->num_channels, buffer->capacity_frames});
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_arm_capture(SonareEngine* engine, int armed) {
  SONARE_C_API_ENTRY;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_arm_capture: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_armed(armed != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_punch(SonareEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled) {
  SONARE_C_API_ENTRY;
  // An enabled empty window would read as "no punch" and record everything.
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_punch: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (start_sample < 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_punch: start_sample must not be negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (end_sample < start_sample) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_punch: end_sample must not be before start_sample");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (enabled != 0 && end_sample == start_sample) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_punch: an enabled punch window must not be empty "
                   "(end_sample equals start_sample)");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_capture_punch(start_sample, end_sample, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_source(SonareEngine* engine,
                                             SonareEngineCaptureSource source) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_capture_source: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  switch (source) {
    case SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kOutput);
      return SONARE_OK;
    case SONARE_ENGINE_CAPTURE_SOURCE_INPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kInput);
      return SONARE_OK;
    default: {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_capture_source: source is not a capture source: " +
                      std::to_string(static_cast<int>(source)))
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  SONARE_C_CATCH
}

SonareError sonare_engine_set_record_offset_samples(SonareEngine* engine, int64_t offset_samples) {
  SONARE_C_API_ENTRY;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_record_offset_samples: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_record_offset_samples(offset_samples);
  return SONARE_OK;
}

SonareError sonare_engine_set_input_monitor(SonareEngine* engine, int enabled, float gain) {
  SONARE_C_API_ENTRY;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_input_monitor: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(gain)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_set_input_monitor: gain must be finite");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.set_input_monitor(enabled != 0, gain);
  return SONARE_OK;
}

SonareError sonare_engine_reset_capture(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_reset_capture: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  engine->engine.reset_capture();
  return SONARE_OK;
}

SonareError sonare_engine_capture_status(SonareEngine* engine, SonareEngineCaptureStatus* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = SonareEngineCaptureStatus{};
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_capture_status: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_capture_status: out must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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

SonareError sonare_engine_set_graph(SonareEngine* engine, const SonareEngineGraphSpec* spec) {
  SONARE_C_API_ENTRY;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_set_graph: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!spec) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "engine_set_graph: spec must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!spec->nodes) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->nodes must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (spec->node_count == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->node_count must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (spec->num_channels <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->num_channels must be positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (spec->connection_count > 0 && !spec->connections) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->connections must not be NULL when "
                   "spec->connection_count is not 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (spec->parameter_binding_count > 0 && !spec->parameter_bindings) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->parameter_bindings must not be NULL when "
                   "spec->parameter_binding_count is not 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  SONARE_C_TRY
  auto graph = std::make_unique<graph::Graph>();
  for (size_t i = 0; i < spec->node_count; ++i) {
    const SonareEngineGraphNode& node = spec->nodes[i];
    // Only 0 asks for the spec's width. A negative count is a caller error, and
    // add_node would have refused it, so it must not reach the derivation.
    if (node.num_ports < 0) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_graph: spec->nodes[" + std::to_string(i) +
                      "].num_ports must not be negative")
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const int ports = node.num_ports == 0 ? spec->num_channels : node.num_ports;
    auto processor = make_graph_processor(node);
    if (!processor) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_graph: spec->nodes[" + std::to_string(i) +
                      "].type is not a node type: " + std::to_string(node.type))
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!graph->add_node(fixed_text(node.id, sizeof(node.id)), std::move(processor), ports)) {
      set_last_error(
          SONARE_ERROR_INVALID_PARAMETER,
          ("engine_set_graph: spec->nodes[" + std::to_string(i) +
           "] was refused: its id is empty or repeated, or its port count is out of range")
              .c_str());
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
    if (!ordinal_to_enum(connection.mix, 0, 1, &graph_connection.mix)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_graph: spec->connections[" + std::to_string(i) +
                      "].mix is not a mix mode: " + std::to_string(connection.mix))
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!graph->connect(std::move(graph_connection))) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("engine_set_graph: spec->connections[" + std::to_string(i) +
                      "] was refused: it is not a valid connection or repeats an earlier one")
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  if (!graph->compile()) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: spec->connections do not form a graph that can be compiled");
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
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_set_graph: the engine refused the graph (a spec->parameter_bindings "
                   "entry targets a reserved parameter, or the runtime swap failed)");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "engine_set_graph: graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_node_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_graph_node_count: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_count) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_graph_node_count: out_count must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_node_count();
  return SONARE_OK;
#else
  // Match sonare_engine_set_graph: a feature-off build cannot have a graph, so
  // report NOT_SUPPORTED rather than SONARE_OK with 0, which a caller cannot
  // distinguish from "the feature is compiled in and the graph has no nodes".
  *out_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                 "engine_graph_node_count: graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_connection_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
  if (!engine) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_graph_connection_count: engine must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_count) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "engine_graph_connection_count: out_count must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_connection_count();
  return SONARE_OK;
#else
  // See sonare_engine_graph_node_count.
  *out_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                 "engine_graph_connection_count: graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
