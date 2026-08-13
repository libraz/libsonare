#pragma once

/// @file node.h
/// @brief Processor node with preallocated port buffers.

#include <memory>
#include <string>
#include <vector>

#include "rt/delay_line.h"
#include "rt/processor_base.h"

namespace sonare::graph {

class Node {
 public:
  // Upper bound on per-node ports. num_ports flows in from user-supplied graph
  // specs (WASM/C engine); bounding it keeps num_ports * max_block_size from
  // overflowing int and prevents pathological buffer allocations.
  static constexpr int kMaxPorts = 1024;

  Node(std::string id, std::unique_ptr<rt::ProcessorBase> processor, int num_ports);

  void prepare(double sample_rate, int max_block_size);
  void reset();
  // Audio-thread path: must be noexcept so the noexcept GraphRuntime::process
  // chain never hits std::terminate. Out-of-range / unprepared inputs are
  // handled by early return rather than throwing.
  void clear_inputs(int num_samples) noexcept;
  void process_block(int num_samples) noexcept;
  bool set_sidechain_ports(int first_port, int num_ports) noexcept;

  float* input_port(int port) noexcept;
  const float* input_port(int port) const noexcept;
  float* output_port(int port) noexcept;
  const float* output_port(int port) const noexcept;

  const std::string& id() const noexcept { return id_; }
  rt::ProcessorBase& processor() noexcept { return *processor_; }
  const rt::ProcessorBase& processor() const noexcept { return *processor_; }
  int num_ports() const noexcept { return num_ports_; }
  int sidechain_first_port() const noexcept { return sidechain_first_port_; }
  int sidechain_num_ports() const noexcept { return sidechain_num_ports_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
  float* port_data(std::vector<float>& storage, int port) noexcept;
  const float* port_data(const std::vector<float>& storage, int port) const noexcept;
  // Number of leading ports the processor actually writes this block; the
  // trailing ports are sidechain inputs it only reads.
  int processed_port_count() const noexcept;
  void prepare_bypass_compensation();
  // Advances every port's compensation delay over the block. @p write_back true
  // substitutes the delayed signal (the node is bypassed); false discards it and
  // only keeps the state moving (the processor is about to run).
  void run_bypass_compensation(int process_ports, int num_samples, bool write_back) noexcept;

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
  std::vector<const float*> sidechain_channels_;
  // One compensation delay per written port, carrying that port's own Q8
  // latency. Graph PDC is baked into the compiled connection delays from
  // output_latency_samples_q8(port), which does not consult bypass, so a
  // bypassed node still owes the graph the latency it was compiled against.
  // Sized per port rather than once per node because a node's ports need not
  // share a latency (a strip node's sends report the pre-fader value while its
  // main outs report the post-fader one). Ports whose latency is zero -- the
  // usual case -- own no storage. Deliberately built from the rt delay
  // primitives rather than mixing::AlignmentDelay: sonare_graph links only
  // sonare_rt, and reaching for the mixing type would pull sonare_mastering in
  // behind it. Same shape as Graph::RuntimeConnection's delay lines.
  struct PortDelay {
    rt::DelayLine integer_line;
    std::vector<float> fractional_buffer;
    size_t fractional_write_index = 0;
    int delay_samples_q8 = 0;
  };
  std::vector<PortDelay> bypass_compensation_;
  bool has_bypass_compensation_ = false;
  int sidechain_first_port_ = 0;
  int sidechain_num_ports_ = 0;
};

}  // namespace sonare::graph
