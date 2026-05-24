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
  int connection_delay_samples(size_t connection_index) const;
  const std::vector<std::string>& topo_order_ids() const noexcept { return topo_order_ids_; }

 private:
  struct RuntimeConnection {
    Connection connection;
    Node* source = nullptr;
    Node* dest = nullptr;
    int delay_samples = 0;
    std::vector<rt::DelayLine> delay_lines;
  };

  bool validate_connection(const Connection& connection) const;
  void prepare_delay_lines(RuntimeConnection& runtime_connection);

  std::vector<std::unique_ptr<Node>> nodes_;
  std::unordered_map<std::string, Node*> node_map_;
  std::vector<Connection> connections_;
  std::vector<RuntimeConnection> runtime_connections_;
  std::vector<Node*> topo_order_;
  std::vector<std::string> topo_order_ids_;
  std::unordered_map<std::string, int> topo_index_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool compiled_ = false;
};

}  // namespace sonare::graph
