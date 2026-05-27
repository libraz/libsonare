#include "engine/graph_runtime.h"

#include <algorithm>
#include <memory>

namespace sonare::engine {

bool GraphRuntime::bind(graph::Graph* graph, const char* input_node_id, const char* output_node_id,
                        int num_channels) {
  if (!graph || !graph->compiled() || !input_node_id || !output_node_id || num_channels <= 0) {
    return false;
  }
  graph::Node* input = graph->node(input_node_id);
  graph::Node* output = graph->node(output_node_id);
  if (!input || !output || input->num_ports() < num_channels ||
      output->num_ports() < num_channels) {
    return false;
  }
  auto alias = std::shared_ptr<graph::Graph>(graph, [](graph::Graph*) {});
  return swap(std::move(alias), input_node_id, output_node_id, num_channels);
}

bool GraphRuntime::swap(std::shared_ptr<graph::Graph> graph, const char* input_node_id,
                        const char* output_node_id, int num_channels) {
  if (!graph || !graph->compiled() || !input_node_id || !output_node_id || num_channels <= 0) {
    return false;
  }
  graph::Node* input = graph->node(input_node_id);
  graph::Node* output = graph->node(output_node_id);
  if (!input || !output || input->num_ports() < num_channels ||
      output->num_ports() < num_channels) {
    return false;
  }
  // Control thread: the allocation may throw bad_alloc and that propagates
  // (this function is intentionally NOT noexcept). publish() hands the new
  // binding to the audio thread lock-free; the old binding is later freed off
  // the audio thread when publish() reclaims retired snapshots.
  auto binding = std::make_shared<Binding>();
  binding->graph = std::move(graph);
  binding->input = input;
  binding->output = output;
  binding->num_channels = num_channels;
  return binding_.publish(std::shared_ptr<const Binding>(std::move(binding)));
}

graph::Graph* GraphRuntime::swap(graph::Graph* graph, const char* input_node_id,
                                 const char* output_node_id, int num_channels) {
  graph::Graph* old = active_graph();
  auto alias = std::shared_ptr<graph::Graph>(graph, [](graph::Graph*) {});
  if (!swap(std::move(alias), input_node_id, output_node_id, num_channels)) {
    return nullptr;
  }
  return old;
}

void GraphRuntime::process(float* const* io, int num_channels, int offset,
                           int num_frames) noexcept {
  // Audio thread: adopt the latest published binding once. current() then keeps
  // the graph alive for the entire render below, so there is no per-block
  // refcount churn, no lock, and no free on this thread.
  binding_.acquire();
  const Binding* binding = binding_.current();
  if (!binding || !binding->graph || !binding->input || !binding->output || !io ||
      num_channels <= 0 || num_frames <= 0 || offset < 0) {
    return;
  }
  const int channels = std::min(num_channels, binding->num_channels);
  binding->graph->clear_inputs(num_frames);
  for (int ch = 0; ch < channels; ++ch) {
    if (!io[ch]) continue;
    float* dest = binding->input->input_port(ch);
    std::copy(io[ch] + offset, io[ch] + offset + num_frames, dest);
  }

  binding->graph->process_block(num_frames);

  for (int ch = 0; ch < channels; ++ch) {
    if (!io[ch]) continue;
    const float* source = binding->output->output_port(ch);
    std::copy(source, source + num_frames, io[ch] + offset);
  }
}

graph::Graph* GraphRuntime::active_graph() const noexcept {
  // Control-thread view: must not call acquire() (that is the audio thread's
  // single-consumer role). Reports the most recently published binding.
  const Binding* binding = binding_.control_current().get();
  return binding ? binding->graph.get() : nullptr;
}

int GraphRuntime::num_channels() const noexcept {
  const Binding* binding = binding_.control_current().get();
  return binding ? binding->num_channels : 0;
}

}  // namespace sonare::engine
