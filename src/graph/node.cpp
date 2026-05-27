#include "graph/node.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonare::graph {

Node::Node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports)
    : id_(std::move(id)), processor_(std::move(processor)), num_ports_(num_ports) {
  if (id_.empty()) {
    throw std::invalid_argument("node id must not be empty");
  }
  if (!processor_) {
    throw std::invalid_argument("node processor must not be null");
  }
  if (num_ports_ <= 0) {
    throw std::invalid_argument("node must have at least one port");
  }
}

void Node::prepare(double sample_rate, int max_block_size) {
  if (max_block_size <= 0) {
    throw std::invalid_argument("max_block_size must be positive");
  }
  max_block_size_ = max_block_size;
  input_.assign(static_cast<size_t>(num_ports_ * max_block_size_), 0.0f);
  output_.assign(static_cast<size_t>(num_ports_ * max_block_size_), 0.0f);
  process_channels_.assign(static_cast<size_t>(num_ports_), nullptr);
  processor_->prepare(sample_rate, max_block_size);
}

void Node::reset() {
  std::fill(input_.begin(), input_.end(), 0.0f);
  std::fill(output_.begin(), output_.end(), 0.0f);
  processor_->reset();
}

void Node::clear_inputs(int num_samples) {
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw std::invalid_argument("num_samples out of prepared range");
  }
  for (int port = 0; port < num_ports_; ++port) {
    float* data = input_port(port);
    std::fill(data, data + num_samples, 0.0f);
  }
}

void Node::process_block(int num_samples) {
  if (num_samples < 0 || num_samples > max_block_size_) {
    throw std::invalid_argument("num_samples out of prepared range");
  }
  for (int port = 0; port < num_ports_; ++port) {
    float* input = input_port(port);
    float* output = output_port(port);
    std::copy(input, input + num_samples, output);
    process_channels_[static_cast<size_t>(port)] = output;
  }
  processor_->process(process_channels_.data(), num_ports_, num_samples);
}

float* Node::input_port(int port) { return port_data(input_, port); }

const float* Node::input_port(int port) const { return port_data(input_, port); }

float* Node::output_port(int port) { return port_data(output_, port); }

const float* Node::output_port(int port) const { return port_data(output_, port); }

float* Node::port_data(std::vector<float>& storage, int port) {
  if (port < 0 || port >= num_ports_ || max_block_size_ <= 0) {
    throw std::out_of_range("node port out of range");
  }
  return storage.data() + static_cast<size_t>(port * max_block_size_);
}

const float* Node::port_data(const std::vector<float>& storage, int port) const {
  if (port < 0 || port >= num_ports_ || max_block_size_ <= 0) {
    throw std::out_of_range("node port out of range");
  }
  return storage.data() + static_cast<size_t>(port * max_block_size_);
}

}  // namespace sonare::graph
