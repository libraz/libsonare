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
  // Audio-thread path: must be noexcept so the noexcept GraphRuntime::process
  // chain never hits std::terminate. Out-of-range / unprepared inputs are
  // handled by early return rather than throwing.
  void clear_inputs(int num_samples) noexcept;
  void process_block(int num_samples) noexcept;

  float* input_port(int port) noexcept;
  const float* input_port(int port) const noexcept;
  float* output_port(int port) noexcept;
  const float* output_port(int port) const noexcept;

  const std::string& id() const noexcept { return id_; }
  rt::ProcessorBase& processor() noexcept { return *processor_; }
  const rt::ProcessorBase& processor() const noexcept { return *processor_; }
  int num_ports() const noexcept { return num_ports_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
  float* port_data(std::vector<float>& storage, int port) noexcept;
  const float* port_data(const std::vector<float>& storage, int port) const noexcept;

  std::string id_;
  std::unique_ptr<rt::ProcessorBase> processor_;
  int num_ports_ = 0;
  int max_block_size_ = 0;
  // Set by prepare(); a node that was compiled but never prepared must not have
  // its processor's process() invoked, because a derived process() may call
  // ProcessorBase::ensure_prepared() and throw -- which would cross the
  // noexcept process_block() boundary and call std::terminate on the audio
  // thread. Guards the call locally so an unprepared swap cannot terminate.
  bool prepared_ = false;
  std::vector<float> input_;
  std::vector<float> output_;
  std::vector<float*> process_channels_;
};

}  // namespace sonare::graph
