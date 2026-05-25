#pragma once

/// @file graph_runtime.h
/// @brief RT-facing wrapper for prepared routing graphs.

#include <memory>

#include "graph/graph.h"
#include "rt/rt_publisher.h"

namespace sonare::engine {

class GraphRuntime {
 public:
  // bind() and swap() run on the CONTROL thread and allocate a new Binding, so
  // they are intentionally NOT noexcept: a throwing allocation must propagate,
  // not terminate via a noexcept boundary.
  bool bind(graph::Graph* graph, const char* input_node_id, const char* output_node_id,
            int num_channels);
  graph::Graph* swap(graph::Graph* graph, const char* input_node_id, const char* output_node_id,
                     int num_channels);
  bool swap(std::shared_ptr<graph::Graph> graph, const char* input_node_id,
            const char* output_node_id, int num_channels);

  void process(float* const* io, int num_channels, int offset, int num_frames) noexcept;

  graph::Graph* active_graph() const noexcept;
  int num_channels() const noexcept;

 private:
  struct Binding {
    std::shared_ptr<graph::Graph> graph;
    graph::Node* input = nullptr;
    graph::Node* output = nullptr;
    int num_channels = 0;
  };

  mutable rt::RtPublisher<Binding> binding_;
};

}  // namespace sonare::engine
