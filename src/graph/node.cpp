#include "graph/node.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "util/exception.h"

namespace sonare::graph {

Node::Node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports)
    : id_(std::move(id)), processor_(std::move(processor)), num_ports_(num_ports) {
  if (id_.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "node id must not be empty");
  }
  if (!processor_) {
    throw SonareException(ErrorCode::InvalidParameter, "node processor must not be null");
  }
  if (num_ports_ <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "node must have at least one port");
  }
}

void Node::prepare(double sample_rate, int max_block_size) {
  if (max_block_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be positive");
  }
  max_block_size_ = max_block_size;
  input_.assign(static_cast<size_t>(num_ports_ * max_block_size_), 0.0f);
  output_.assign(static_cast<size_t>(num_ports_ * max_block_size_), 0.0f);
  process_channels_.assign(static_cast<size_t>(num_ports_), nullptr);
  // prepared_ flips true only after the processor's own prepare() has completed
  // without throwing, so process_block() (noexcept, audio thread) can rely on
  // it: a prepared Node always has a prepared processor, hence ensure_prepared()
  // inside processor_->process() can never throw across the noexcept boundary.
  prepared_ = false;
  processor_->prepare(sample_rate, max_block_size);
  prepared_ = true;
}

void Node::reset() {
  std::fill(input_.begin(), input_.end(), 0.0f);
  std::fill(output_.begin(), output_.end(), 0.0f);
  processor_->reset();
}

void Node::clear_inputs(int num_samples) noexcept {
  // Audio-thread path: silently no-op on out-of-range rather than throw (a
  // throw here would propagate through the noexcept GraphRuntime::process
  // chain and call std::terminate).
  if (num_samples < 0 || num_samples > max_block_size_) {
    return;
  }
  for (int port = 0; port < num_ports_; ++port) {
    float* data = input_port(port);
    std::fill(data, data + num_samples, 0.0f);
  }
}

void Node::process_block(int num_samples) noexcept {
  // Audio-thread path: silently no-op on out-of-range rather than throw.
  // Also bail when the node was compiled but never prepared: invoking
  // processor_->process() then could trip ProcessorBase::ensure_prepared(),
  // whose throw would cross this noexcept boundary and std::terminate.
  if (!prepared_ || num_samples < 0 || num_samples > max_block_size_) {
    return;
  }
  for (int port = 0; port < num_ports_; ++port) {
    float* input = input_port(port);
    float* output = output_port(port);
    std::copy(input, input + num_samples, output);
    process_channels_[static_cast<size_t>(port)] = output;
  }
  processor_->process(process_channels_.data(), num_ports_, num_samples);
}

float* Node::input_port(int port) noexcept { return port_data(input_, port); }

const float* Node::input_port(int port) const noexcept { return port_data(input_, port); }

float* Node::output_port(int port) noexcept { return port_data(output_, port); }

const float* Node::output_port(int port) const noexcept { return port_data(output_, port); }

float* Node::port_data(std::vector<float>& storage, int port) noexcept {
  // Hottest per-sample path: assert in debug, clamp to port 0 in release so a
  // bad index can never throw (which would terminate the noexcept audio chain)
  // nor index out of bounds.
  assert(port >= 0 && port < num_ports_ && max_block_size_ > 0);
  if (port < 0 || port >= num_ports_ || max_block_size_ <= 0) {
    return storage.data();
  }
  return storage.data() + static_cast<size_t>(port * max_block_size_);
}

const float* Node::port_data(const std::vector<float>& storage, int port) const noexcept {
  assert(port >= 0 && port < num_ports_ && max_block_size_ > 0);
  if (port < 0 || port >= num_ports_ || max_block_size_ <= 0) {
    return storage.data();
  }
  return storage.data() + static_cast<size_t>(port * max_block_size_);
}

}  // namespace sonare::graph
