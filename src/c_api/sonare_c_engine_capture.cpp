#include <sonare/sonare_c.h>

#include <array>
#include <string>

#include "engine/realtime_engine.h"
#include "sonare_c_engine_internal.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

#if defined(SONARE_WITH_GRAPH)
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/graph_runtime.h"
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

using util::check::indexed_subject;

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

// Maps the cause the engine retained for a refused swap to the argument it names.
SonareError graph_swap_error(const engine::GraphSwapFault& fault) {
  using Cause = engine::GraphSwapRefusal;
  const std::string id_subject =
      indexed_subject("spec.parameter_bindings", fault.index, "param_id");
  switch (fault.cause) {
    case Cause::kIncompleteArguments:
      set_last_error(SONARE_ERROR_INVALID_STATE,
                     "the engine was given no graph or no node id to swap in");
      return SONARE_ERROR_INVALID_STATE;
    case Cause::kNoChannels:
      return run_validation([&] { util::check::integer_at_least(0, 1, "spec.num_channels"); });
    case Cause::kReservedParameterId:
      return run_validation([&] {
        util::check::refuse(id_subject.c_str(), "must not be a reserved engine parameter id");
      });
    case Cause::kNotCompiled:
      set_last_error(SONARE_ERROR_INVALID_STATE, "the graph has not been compiled");
      return SONARE_ERROR_INVALID_STATE;
    case Cause::kUnknownInputNode:
      return run_validation([&] {
        util::check::refuse("spec.input_node", "must be the id of a node in spec.nodes");
      });
    case Cause::kUnknownOutputNode:
      return run_validation([&] {
        util::check::refuse("spec.output_node", "must be the id of a node in spec.nodes");
      });
    case Cause::kInputPortsShort:
      return run_validation([&] {
        util::check::refuse_relation({"spec.num_channels", "spec.input_node"},
                                     "{0} must not exceed the port count of {1}");
      });
    case Cause::kOutputPortsShort:
      return run_validation([&] {
        util::check::refuse_relation({"spec.num_channels", "spec.output_node"},
                                     "{0} must not exceed the port count of {1}");
      });
    case Cause::kTooManyBindings:
      return run_validation([&] {
        util::check::refuse(
            "spec.parameter_binding_count",
            util::refusal::integer_range_predicate(
                0, static_cast<double>(engine::GraphRuntime::kMaxParameterBindings)));
      });
    case Cause::kBindingIdZero:
      return run_validation([&] {
        util::check::integer_in_range(0, 1, std::numeric_limits<uint32_t>::max(),
                                      id_subject.c_str());
      });
    case Cause::kBindingIdRepeated:
      return run_validation([&] {
        util::check::refuse(id_subject.c_str(), "must be unique among spec.parameter_bindings");
      });
    case Cause::kBindingNodeUnknown:
      return run_validation([&] {
        util::check::refuse(
            indexed_subject("spec.parameter_bindings", fault.index, "node_id").c_str(),
            "must be the id of a node in spec.nodes");
      });
    case Cause::kNone:
      break;
  }
  set_last_error(SONARE_ERROR_INVALID_STATE, "the engine refused the graph and retained no cause");
  return SONARE_ERROR_INVALID_STATE;
}

}  // namespace
#endif

SonareError sonare_engine_set_capture_buffer(SonareEngine* engine,
                                             const SonareEngineCaptureBuffer* buffer) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(buffer);
  if (const SonareError err = run_validation([&] {
        util::check::non_null(buffer->channels, "buffer.channels");
        util::check::integer_at_least(buffer->num_channels, 1, "buffer.num_channels");
        util::check::integer_at_least(buffer->capacity_frames, 1, "buffer.capacity_frames");
        for (int ch = 0; ch < buffer->num_channels; ++ch) {
          util::check::non_null(buffer->channels[ch],
                                ("buffer.channels[" + std::to_string(ch) + "]").c_str());
        }
      });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_capture_segment(
      {buffer->channels, buffer->num_channels, buffer->capacity_frames});
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_engine_arm_capture(SonareEngine* engine, int armed) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.set_capture_armed(armed != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_punch(SonareEngine* engine, int64_t start_sample,
                                            int64_t end_sample, int enabled) {
  SONARE_C_API_ENTRY;
  // An enabled empty window would read as "no punch" and record everything.
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] {
        util::check::integer_at_least(start_sample, 0, "start_sample");
        if (end_sample < start_sample) {
          util::check::refuse_relation({"end_sample", "start_sample"},
                                       "{0} must not be before {1}");
        }
        if (enabled != 0 && end_sample == start_sample) {
          util::check::refuse_relation({"end_sample", "start_sample", "enabled"},
                                       "{0} must not equal {1} when {2} is not 0");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_capture_punch(start_sample, end_sample, enabled != 0);
  return SONARE_OK;
}

SonareError sonare_engine_set_capture_source(SonareEngine* engine,
                                             SonareEngineCaptureSource source) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  switch (source) {
    case SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kOutput);
      return SONARE_OK;
    case SONARE_ENGINE_CAPTURE_SOURCE_INPUT:
      engine->engine.set_capture_source(engine::CaptureSource::kInput);
      return SONARE_OK;
    default:
      return run_validation([&] {
        const std::array<int, 2> sources{SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT,
                                         SONARE_ENGINE_CAPTURE_SOURCE_INPUT};
        util::check::one_of(static_cast<int>(source), sources, "source");
      });
  }
  SONARE_C_CATCH
}

SonareError sonare_engine_set_record_offset_samples(SonareEngine* engine, int64_t offset_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.set_record_offset_samples(offset_samples);
  return SONARE_OK;
}

SonareError sonare_engine_set_input_monitor(SonareEngine* engine, int enabled, float gain) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = run_validation([&] { util::check::finite_float(gain, "gain"); });
      err != SONARE_OK) {
    return err;
  }
  engine->engine.set_input_monitor(enabled != 0, gain);
  return SONARE_OK;
}

SonareError sonare_engine_reset_capture(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  engine->engine.reset_capture();
  return SONARE_OK;
}

SonareError sonare_engine_capture_status(SonareEngine* engine, SonareEngineCaptureStatus* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = SonareEngineCaptureStatus{};
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out);
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
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(spec);
  if (const SonareError err = run_validation([&] {
        util::check::non_null(spec->nodes, "spec.nodes");
        util::check::integer_at_least(spec->node_count, 1, "spec.node_count");
        util::check::integer_at_least(spec->num_channels, 1, "spec.num_channels");
        util::check::non_null_when_count(spec->connections, spec->connection_count,
                                         "spec.connections", "spec.connection_count");
        util::check::non_null_when_count(spec->parameter_bindings, spec->parameter_binding_count,
                                         "spec.parameter_bindings", "spec.parameter_binding_count");
      });
      err != SONARE_OK) {
    return err;
  }
#if defined(SONARE_WITH_GRAPH)
  SONARE_C_TRY
  auto graph = std::make_unique<graph::Graph>();
  for (size_t i = 0; i < spec->node_count; ++i) {
    const SonareEngineGraphNode& node = spec->nodes[i];
    // Only 0 asks for the spec's width. A negative count is a caller error, and
    // add_node would have refused it, so it must not reach the derivation.
    if (const SonareError err = run_validation([&] {
          util::check::integer_at_least(node.num_ports, 0,
                                        indexed_subject("spec.nodes", i, "num_ports").c_str());
        });
        err != SONARE_OK) {
      return err;
    }
    const int ports = node.num_ports == 0 ? spec->num_channels : node.num_ports;
    auto processor = make_graph_processor(node);
    if (!processor) {
      return run_validation([&] {
        const std::array<int, 2> types{0, 1};
        util::check::one_of(node.type, types, indexed_subject("spec.nodes", i, "type").c_str());
      });
    }
    if (!graph->add_node(fixed_text(node.id, sizeof(node.id)), std::move(processor), ports)) {
      return run_validation([&] {
        util::check::refuse(("spec.nodes[" + std::to_string(i) + "]").c_str(),
                            "must have a unique non-empty id and a port count in range");
      });
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
      return run_validation([&] {
        const std::array<int, 2> modes{0, 1};
        util::check::one_of(connection.mix, modes,
                            indexed_subject("spec.connections", i, "mix").c_str());
      });
    }
    if (!graph->connect(std::move(graph_connection))) {
      return run_validation([&] {
        util::check::refuse(("spec.connections[" + std::to_string(i) + "]").c_str(),
                            "must be a valid connection that no earlier entry repeats");
      });
    }
  }
  if (!graph->compile()) {
    return run_validation(
        [&] { util::check::refuse("spec.connections", "must form a graph that can be compiled"); });
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
    return graph_swap_error(engine->engine.last_graph_swap_fault());
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_node_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_node_count();
  return SONARE_OK;
#else
  // Match sonare_engine_set_graph: a feature-off build cannot have a graph, so
  // report NOT_SUPPORTED rather than SONARE_OK with 0, which a caller cannot
  // distinguish from "the feature is compiled in and the graph has no nodes".
  *out_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_engine_graph_connection_count(SonareEngine* engine, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_count);
#if defined(SONARE_WITH_GRAPH)
  *out_count = engine->engine.graph_connection_count();
  return SONARE_OK;
#else
  // See sonare_engine_graph_node_count.
  *out_count = 0;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "graph support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
