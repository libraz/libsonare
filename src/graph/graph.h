#pragma once

/// @file graph.h
/// @brief Offline/native audio routing graph.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/connection.h"
#include "graph/node.h"
#include "rt/delay_line.h"

namespace sonare::graph {

class Graph {
 public:
  bool add_node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports = 2);
  bool remove_node(const std::string& id);
  bool connect(Connection connection);
  bool disconnect(const std::string& source_node, int source_port, const std::string& dest_node,
                  int dest_port);

  bool compile();
  void prepare(double sample_rate, int max_block_size);
  void reset();
  void clear_inputs(int num_samples);
  void set_input(const std::string& node_id, int port, const float* samples, int num_samples);
  void process_block(int num_samples);

  const float* output(const std::string& node_id, int port) const;
  Node* node(const std::string& id);
  const Node* node(const std::string& id) const;

  bool compiled() const noexcept { return compiled_; }
  size_t node_count() const noexcept { return nodes_.size(); }
  size_t connection_count() const noexcept { return connections_.size(); }
  int connection_delay_samples(size_t connection_index) const;
  // Q8 delay inserted on a compiled connection by longest-path PDC. If the
  // value has a fractional part, Graph uses the same Lagrange3 fractional delay
  // convention as mixing::AlignmentDelay.
  int connection_delay_samples_q8(size_t connection_index) const;
  const std::vector<std::string>& topo_order_ids() const noexcept { return topo_order_ids_; }

 private:
  struct RuntimeConnection {
    struct FractionalDelayLine {
      std::vector<float> buffer{0.0f};
      size_t write_index = 0;
    };

    Connection connection;
    Node* source = nullptr;
    Node* dest = nullptr;
    int delay_samples = 0;
    int delay_samples_q8 = 0;
    std::vector<rt::DelayLine> delay_lines;
    std::vector<FractionalDelayLine> fractional_delay_lines;
  };

  bool validate_connection(const Connection& connection) const;
  void prepare_delay_lines(RuntimeConnection& runtime_connection);
  static float process_fractional_delay(RuntimeConnection::FractionalDelayLine& delay_line,
                                        int delay_samples_q8, float input) noexcept;

  std::vector<std::unique_ptr<Node>> nodes_;
  std::unordered_map<std::string, Node*> node_map_;
  std::vector<Connection> connections_;
  std::vector<RuntimeConnection> runtime_connections_;
  // For each node in topo order (indexed by topo position), the indices into
  // runtime_connections_ whose destination is that node. Built in compile() so
  // process_block() is O(E) instead of O(nodes * edges).
  std::vector<std::vector<int>> incoming_by_topo_;
  std::vector<Node*> topo_order_;
  std::vector<std::string> topo_order_ids_;
  std::unordered_map<std::string, int> topo_index_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool compiled_ = false;
};

}  // namespace sonare::graph
