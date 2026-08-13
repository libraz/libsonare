#include "graph/node.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "rt/fractional_delay.h"
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
  if (num_ports_ > kMaxPorts) {
    throw SonareException(ErrorCode::InvalidParameter, "node port count exceeds maximum");
  }
}

void Node::prepare(double sample_rate, int max_block_size) {
  if (max_block_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be positive");
  }
  max_block_size_ = max_block_size;
  const size_t buffer_size = static_cast<size_t>(num_ports_) * static_cast<size_t>(max_block_size_);
  input_.assign(buffer_size, 0.0f);
  output_.assign(buffer_size, 0.0f);
  process_channels_.assign(static_cast<size_t>(num_ports_), nullptr);
  sidechain_channels_.assign(static_cast<size_t>(num_ports_), nullptr);
  // prepared_ flips true only after the processor's own prepare() has completed
  // without throwing, so process_block() (noexcept, audio thread) can rely on
  // it: a prepared Node always has a prepared processor, hence ensure_prepared()
  // inside processor_->process() can never throw across the noexcept boundary.
  prepared_ = false;
  processor_->prepare(sample_rate, max_block_size);
  // After the processor's prepare(): a processor reports its final per-port
  // latency only once prepared.
  prepare_bypass_compensation();
  prepared_ = true;
}

int Node::processed_port_count() const noexcept {
  return sidechain_num_ports_ > 0 ? sidechain_first_port_ : num_ports_;
}

void Node::prepare_bypass_compensation() {
  bypass_compensation_.assign(static_cast<size_t>(num_ports_), PortDelay{});
  has_bypass_compensation_ = false;
  for (int port = 0; port < num_ports_; ++port) {
    const int latency_q8 = processor_->output_latency_samples_q8(port);
    if (latency_q8 <= 0) {
      continue;
    }
    PortDelay& delay = bypass_compensation_[static_cast<size_t>(port)];
    delay.delay_samples_q8 = latency_q8;
    const int integer_delay = latency_q8 >> 8;
    if ((latency_q8 & 0xff) != 0) {
      // Fractional latency keeps its Lagrange interpolator, matching the
      // connection delay lines the compiler builds for the same Q8 value.
      delay.fractional_buffer.assign(static_cast<size_t>(std::max(8, integer_delay + 8)), 0.0f);
      delay.fractional_write_index = 0;
    } else {
      delay.integer_line.prepare(static_cast<size_t>(integer_delay));
    }
    has_bypass_compensation_ = true;
  }
  if (!has_bypass_compensation_) {
    // Nothing latent on any port: drop the storage so a plain gain/pan node
    // carries no per-port state at all.
    bypass_compensation_.clear();
  }
}

void Node::run_bypass_compensation(int process_ports, int num_samples, bool write_back) noexcept {
  for (int port = 0; port < process_ports; ++port) {
    PortDelay& delay = bypass_compensation_[static_cast<size_t>(port)];
    if (delay.delay_samples_q8 <= 0) {
      continue;
    }
    float* data = output_port(port);
    // Mode test hoisted out of the sample loop, as in AlignmentDelay.
    if (delay.fractional_buffer.empty()) {
      for (int i = 0; i < num_samples; ++i) {
        const float delayed = delay.integer_line.process(data[i]);
        if (write_back) {
          data[i] = delayed;
        }
      }
    } else {
      for (int i = 0; i < num_samples; ++i) {
        const float delayed = rt::lagrange3_fractional_delay(
            delay.fractional_buffer, delay.fractional_write_index, delay.delay_samples_q8, data[i]);
        if (write_back) {
          data[i] = delayed;
        }
      }
    }
  }
}

void Node::reset() {
  std::fill(input_.begin(), input_.end(), 0.0f);
  std::fill(output_.begin(), output_.end(), 0.0f);
  for (PortDelay& delay : bypass_compensation_) {
    delay.integer_line.reset();
    std::fill(delay.fractional_buffer.begin(), delay.fractional_buffer.end(), 0.0f);
    delay.fractional_write_index = 0;
  }
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
  int process_ports = num_ports_;
  if (sidechain_num_ports_ > 0) {
    process_ports = sidechain_first_port_;
    for (int port = 0; port < sidechain_num_ports_; ++port) {
      sidechain_channels_[static_cast<size_t>(port)] = output_port(sidechain_first_port_ + port);
    }
    processor_->set_sidechain(sidechain_channels_.data(), sidechain_num_ports_, num_samples);
  }
  if (processor_->bypassed()) {
    // Soft bypass: compile() bakes this node's per-port latency into the
    // downstream connection delays and never revisits it, while bypass flips on
    // the audio thread. Dropping the latency here would slide this node's
    // signal forward against every path the compiler aligned it with, for as
    // long as the bypass stands. Deliver it as a plain delay instead, so the
    // node's real latency keeps matching output_latency_samples_q8(port).
    if (has_bypass_compensation_) {
      run_bypass_compensation(process_ports, num_samples, /*write_back=*/true);
    }
    return;
  }
  // Keep the substitute delays fed with the same stream the processor is about
  // to consume, so engaging bypass switches to a warm delay line rather than
  // opening with a compensation-length dropout. Read-only, and ordered ahead of
  // process() because the processor overwrites these buffers in place.
  if (has_bypass_compensation_) {
    run_bypass_compensation(process_ports, num_samples, /*write_back=*/false);
  }
  processor_->process(process_channels_.data(), process_ports, num_samples);
}

bool Node::set_sidechain_ports(int first_port, int num_ports) noexcept {
  if (num_ports == 0) {
    sidechain_first_port_ = 0;
    sidechain_num_ports_ = 0;
    return true;
  }
  if (first_port <= 0 || num_ports < 0 || first_port >= num_ports_ ||
      num_ports > num_ports_ - first_port) {
    return false;
  }
  sidechain_first_port_ = first_port;
  sidechain_num_ports_ = num_ports;
  return true;
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
  return storage.data() + static_cast<size_t>(port) * static_cast<size_t>(max_block_size_);
}

const float* Node::port_data(const std::vector<float>& storage, int port) const noexcept {
  assert(port >= 0 && port < num_ports_ && max_block_size_ > 0);
  if (port < 0 || port >= num_ports_ || max_block_size_ <= 0) {
    return storage.data();
  }
  return storage.data() + static_cast<size_t>(port) * static_cast<size_t>(max_block_size_);
}

}  // namespace sonare::graph
