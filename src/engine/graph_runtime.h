#pragma once

/// @file graph_runtime.h
/// @brief RT-facing wrapper for prepared routing graphs.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "graph/graph.h"
#include "rt/rt_publisher.h"

namespace sonare::engine {

class GraphRuntime {
 public:
  struct ParameterBinding {
    uint32_t param_id = 0;
    std::string node_id;
  };

  static constexpr size_t kMaxParameterBindings = 128;

  // bind() and swap() run on the CONTROL thread and allocate a new Binding, so
  // they are intentionally NOT noexcept: a throwing allocation must propagate,
  // not terminate via a noexcept boundary.
  bool bind(graph::Graph* graph, const char* input_node_id, const char* output_node_id,
            int num_channels);
  graph::Graph* swap(graph::Graph* graph, const char* input_node_id, const char* output_node_id,
                     int num_channels);
  bool swap(std::shared_ptr<graph::Graph> graph, const char* input_node_id,
            const char* output_node_id, int num_channels,
            std::vector<ParameterBinding> parameter_bindings = {});

  /// Publish a single parameter binding as a new immutable graph snapshot.
  /// Existing bindings and graph ownership are preserved transactionally.
  bool bind_parameter(uint32_t param_id, const char* node_id);

  /// Adopt the latest graph + parameter-target snapshot on the audio thread.
  /// Call exactly once at block start before automation or graph processing.
  void acquire() noexcept {
    binding_.acquire();
    acquired_for_block_ = true;
  }

  /// Resolve a parameter against the graph snapshot adopted for this block.
  rt::ProcessorBase* parameter_target(uint32_t param_id) const noexcept;

  void process(float* const* io, int num_channels, int offset, int num_frames) noexcept;

  graph::Graph* active_graph() const noexcept;
  int num_channels() const noexcept;

 private:
  struct Binding {
    struct Target {
      uint32_t param_id = 0;
      rt::ProcessorBase* processor = nullptr;
    };

    std::shared_ptr<graph::Graph> graph;
    graph::Node* input = nullptr;
    graph::Node* output = nullptr;
    int num_channels = 0;
    std::vector<Target> parameter_targets;
  };

  mutable rt::RtPublisher<Binding> binding_;
  // Audio-thread-only handshake: RealtimeEngine explicitly acquires before
  // automation; standalone GraphRuntime::process callers acquire at offset 0.
  bool acquired_for_block_ = false;
};

}  // namespace sonare::engine
