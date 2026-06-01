#include "graph/graph.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

#include "rt/fractional_delay.h"
#include "util/exception.h"

namespace sonare::graph {

bool Graph::add_node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports) {
  if (id.empty() || node_map_.find(id) != node_map_.end() || !processor || num_ports <= 0) {
    return false;
  }
  auto new_node = std::make_unique<Node>(id, std::move(processor), num_ports);
  Node* raw = new_node.get();
  node_map_.emplace(raw->id(), raw);
  nodes_.push_back(std::move(new_node));
  compiled_ = false;
  return true;
}

bool Graph::remove_node(const std::string& id) {
  const auto found = node_map_.find(id);
  if (found == node_map_.end()) {
    return false;
  }
  connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                    [&](const Connection& connection) {
                                      return connection.source_node == id ||
                                             connection.dest_node == id;
                                    }),
                     connections_.end());
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                              [&](const std::unique_ptr<Node>& node) { return node->id() == id; }),
               nodes_.end());
  node_map_.erase(found);
  compiled_ = false;
  return true;
}

bool Graph::connect(Connection connection) {
  if (!validate_connection(connection)) {
    return false;
  }
  // Reject duplicate edges (same source/dest node and port) to avoid
  // double-summing the same signal at the destination port.
  const bool exists =
      std::any_of(connections_.begin(), connections_.end(), [&](const Connection& existing) {
        return existing.source_node == connection.source_node &&
               existing.source_port == connection.source_port &&
               existing.dest_node == connection.dest_node &&
               existing.dest_port == connection.dest_port;
      });
  if (exists) {
    return false;
  }
  connections_.push_back(std::move(connection));
  compiled_ = false;
  return true;
}

bool Graph::disconnect(const std::string& source_node, int source_port,
                       const std::string& dest_node, int dest_port) {
  const auto old_size = connections_.size();
  connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                    [&](const Connection& connection) {
                                      return connection.source_node == source_node &&
                                             connection.source_port == source_port &&
                                             connection.dest_node == dest_node &&
                                             connection.dest_port == dest_port;
                                    }),
                     connections_.end());
  compiled_ = false;
  return connections_.size() != old_size;
}

bool Graph::compile() {
  topo_order_.clear();
  topo_order_ids_.clear();
  topo_index_.clear();
  runtime_connections_.clear();
  incoming_by_topo_.clear();

  std::unordered_map<std::string, int> indegree;
  std::unordered_map<std::string, std::vector<std::string>> outgoing;
  for (const auto& node_ptr : nodes_) {
    indegree[node_ptr->id()] = 0;
  }

  for (const Connection& connection : connections_) {
    if (!validate_connection(connection)) {
      compiled_ = false;
      return false;
    }
    outgoing[connection.source_node].push_back(connection.dest_node);
    ++indegree[connection.dest_node];
  }

  std::queue<std::string> ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0) {
      ready.push(entry.first);
    }
  }

  while (!ready.empty()) {
    const std::string id = ready.front();
    ready.pop();
    Node* current = node_map_.at(id);
    topo_index_[id] = static_cast<int>(topo_order_.size());
    topo_order_.push_back(current);
    topo_order_ids_.push_back(id);

    for (const std::string& dest : outgoing[id]) {
      --indegree[dest];
      if (indegree[dest] == 0) {
        ready.push(dest);
      }
    }
  }

  if (topo_order_.size() != nodes_.size()) {
    topo_order_.clear();
    topo_order_ids_.clear();
    topo_index_.clear();
    compiled_ = false;
    return false;
  }

  std::unordered_map<std::string, int> latency_to_node_q8;
  for (Node* current : topo_order_) {
    int max_incoming_latency_q8 = 0;
    for (const Connection& connection : connections_) {
      if (connection.dest_node != current->id()) {
        continue;
      }
      const Node* source = node_map_.at(connection.source_node);
      const int path_latency_q8 =
          latency_to_node_q8[connection.source_node] +
          source->processor().output_latency_samples_q8(connection.source_port);
      max_incoming_latency_q8 = std::max(max_incoming_latency_q8, path_latency_q8);
    }
    latency_to_node_q8[current->id()] = max_incoming_latency_q8;
  }

  for (const Connection& connection : connections_) {
    RuntimeConnection runtime_connection;
    runtime_connection.connection = connection;
    runtime_connection.source = node_map_.at(connection.source_node);
    runtime_connection.dest = node_map_.at(connection.dest_node);
    const int source_path_latency_q8 =
        latency_to_node_q8[connection.source_node] +
        runtime_connection.source->processor().output_latency_samples_q8(connection.source_port);
    runtime_connection.delay_samples_q8 =
        std::max(0, latency_to_node_q8[connection.dest_node] - source_path_latency_q8);
    runtime_connection.delay_samples = runtime_connection.delay_samples_q8 >> 8;
    if (max_block_size_ > 0) {
      prepare_delay_lines(runtime_connection);
    }
    runtime_connections_.push_back(std::move(runtime_connection));
  }

  // Build the per-destination adjacency list keyed by the destination's topo
  // position so process_block() can iterate only the edges feeding each node.
  incoming_by_topo_.assign(topo_order_.size(), {});
  for (size_t connection_index = 0; connection_index < runtime_connections_.size();
       ++connection_index) {
    const std::string& dest_id = runtime_connections_[connection_index].connection.dest_node;
    incoming_by_topo_[static_cast<size_t>(topo_index_.at(dest_id))].push_back(
        static_cast<int>(connection_index));
  }

  compiled_ = true;
  return true;
}

void Graph::prepare(double sample_rate, int max_block_size) {
  if (max_block_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be positive");
  }
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = max_block_size;
  for (auto& node_ptr : nodes_) {
    node_ptr->prepare(sample_rate_, max_block_size_);
  }
  // Topology must be compiled before processing. If a structural change
  // invalidated a prior compile (or it was never compiled), (re)compile now so
  // prepare() never leaves stale/empty runtime connections. compile() also sizes
  // the per-connection delay lines because max_block_size_ is now set.
  if (!compiled_) {
    if (!compile()) {
      throw SonareException(ErrorCode::InvalidState,
                            "graph topology could not be compiled (cycle or invalid edge)");
    }
  } else {
    for (RuntimeConnection& runtime_connection : runtime_connections_) {
      prepare_delay_lines(runtime_connection);
    }
  }
}

void Graph::reset() {
  for (auto& node_ptr : nodes_) {
    node_ptr->reset();
  }
  for (RuntimeConnection& runtime_connection : runtime_connections_) {
    for (rt::DelayLine& delay_line : runtime_connection.delay_lines) {
      delay_line.reset();
    }
    for (auto& delay_line : runtime_connection.fractional_delay_lines) {
      std::fill(delay_line.buffer.begin(), delay_line.buffer.end(), 0.0f);
      delay_line.write_index = 0;
    }
  }
}

void Graph::clear_inputs(int num_samples) noexcept {
  for (auto& node_ptr : nodes_) {
    node_ptr->clear_inputs(num_samples);
  }
}

void Graph::set_input(const std::string& node_id, int port, const float* samples, int num_samples) {
  Node* target = node(node_id);
  if (target == nullptr || samples == nullptr || num_samples < 0 ||
      num_samples > target->max_block_size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid graph input");
  }
  float* dest = target->input_port(port);
  std::copy(samples, samples + num_samples, dest);
}

void Graph::process_block(int num_samples) noexcept {
  // Audio-thread path: an uncompiled graph or out-of-range size is a safe
  // no-op rather than a throw. GraphRuntime guarantees only compiled graphs
  // ever reach the audio thread (see GraphRuntime::swap), so this is purely
  // defensive against misuse.
  if (!compiled_ || num_samples < 0 || num_samples > max_block_size_) {
    return;
  }

  for (size_t topo_position = 0; topo_position < topo_order_.size(); ++topo_position) {
    Node* current = topo_order_[topo_position];
    for (const int connection_index : incoming_by_topo_[topo_position]) {
      RuntimeConnection& runtime_connection =
          runtime_connections_[static_cast<size_t>(connection_index)];

      const int source_port = runtime_connection.connection.source_port;
      const int dest_port = runtime_connection.connection.dest_port;
      const float* source = runtime_connection.source->output_port(source_port);
      float* dest = runtime_connection.dest->input_port(dest_port);
      rt::DelayLine* delay_line =
          runtime_connection.delay_lines.empty() ? nullptr : &runtime_connection.delay_lines[0];
      RuntimeConnection::FractionalDelayLine* fractional_delay_line =
          runtime_connection.fractional_delay_lines.empty()
              ? nullptr
              : &runtime_connection.fractional_delay_lines[0];

      for (int i = 0; i < num_samples; ++i) {
        float value = source[i];
        if (fractional_delay_line != nullptr) {
          value = process_fractional_delay(*fractional_delay_line,
                                           runtime_connection.delay_samples_q8, value);
        } else if (delay_line != nullptr) {
          value = delay_line->process(value);
        }
        if (runtime_connection.connection.mix == Connection::Mix::Replace) {
          dest[i] = value;
        } else {
          dest[i] += value;
        }
      }
    }
    current->process_block(num_samples);
  }
}

const float* Graph::output(const std::string& node_id, int port) const {
  const Node* target = node(node_id);
  return target == nullptr ? nullptr : target->output_port(port);
}

Node* Graph::node(const std::string& id) {
  const auto found = node_map_.find(id);
  return found == node_map_.end() ? nullptr : found->second;
}

const Node* Graph::node(const std::string& id) const {
  const auto found = node_map_.find(id);
  return found == node_map_.end() ? nullptr : found->second;
}

int Graph::connection_delay_samples(size_t connection_index) const {
  if (connection_index >= runtime_connections_.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "connection index out of range");
  }
  return runtime_connections_[connection_index].delay_samples;
}

int Graph::connection_delay_samples_q8(size_t connection_index) const {
  if (connection_index >= runtime_connections_.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "connection index out of range");
  }
  return runtime_connections_[connection_index].delay_samples_q8;
}

bool Graph::validate_connection(const Connection& connection) const {
  const Node* source = node(connection.source_node);
  const Node* dest = node(connection.dest_node);
  if (source == nullptr || dest == nullptr) {
    return false;
  }
  return connection.source_port >= 0 && connection.source_port < source->num_ports() &&
         connection.dest_port >= 0 && connection.dest_port < dest->num_ports();
}

void Graph::prepare_delay_lines(RuntimeConnection& runtime_connection) {
  runtime_connection.fractional_delay_lines.clear();
  runtime_connection.delay_lines.assign(1, rt::DelayLine{});
  if (runtime_connection.delay_samples <= 0) {
    runtime_connection.delay_lines.clear();
  }
  if ((runtime_connection.delay_samples_q8 & 0xff) != 0) {
    runtime_connection.delay_lines.clear();
    runtime_connection.fractional_delay_lines.assign(1, RuntimeConnection::FractionalDelayLine{});
    const int integer_delay = runtime_connection.delay_samples_q8 >> 8;
    const size_t size = static_cast<size_t>(std::max(8, integer_delay + 8));
    runtime_connection.fractional_delay_lines[0].buffer.assign(size, 0.0f);
    runtime_connection.fractional_delay_lines[0].write_index = 0;
  } else if (!runtime_connection.delay_lines.empty()) {
    runtime_connection.delay_lines[0].prepare(
        static_cast<size_t>(runtime_connection.delay_samples));
  }
}

float Graph::process_fractional_delay(RuntimeConnection::FractionalDelayLine& delay_line,
                                      int delay_samples_q8, float input) noexcept {
  return rt::lagrange3_fractional_delay(delay_line.buffer, delay_line.write_index, delay_samples_q8,
                                        input);
}

}  // namespace sonare::graph
