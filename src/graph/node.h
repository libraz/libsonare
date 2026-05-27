#pragma once

/// @file node.h
/// @brief Processor node with preallocated port buffers.

#include <memory>
#include <string>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::graph {

class Node {
 public:
  Node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports);

  void prepare(double sample_rate, int max_block_size);
  void reset();
  void clear_inputs(int num_samples);
  void process_block(int num_samples);

  float* input_port(int port);
  const float* input_port(int port) const;
  float* output_port(int port);
  const float* output_port(int port) const;

  const std::string& id() const noexcept { return id_; }
  rt::ProcessorBase& processor() noexcept { return *processor_; }
  const rt::ProcessorBase& processor() const noexcept { return *processor_; }
  int num_ports() const noexcept { return num_ports_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
  float* port_data(std::vector<float>& storage, int port);
  const float* port_data(const std::vector<float>& storage, int port) const;

  std::string id_;
  std::unique_ptr<rt::ProcessorBase> processor_;
  int num_ports_ = 0;
  int max_block_size_ = 0;
  std::vector<float> input_;
  std::vector<float> output_;
  std::vector<float*> process_channels_;
};

}  // namespace sonare::graph
