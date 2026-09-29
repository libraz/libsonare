#pragma once

/// @file graph_engine.h
/// @brief Double-precision runner for one GS classic type graph at 32 kHz.
///
/// The schedule is the soundings renderer's: a loop-free component is drawn over the
/// whole block, a looped one a sample at a time, and a control-driven value is read
/// per sample. A delay or pitch node reads its input from a history ring the engine
/// fills, so a loop closes through it one sample late at the least.
///
/// The engine draws gain, mix, pan, delay, lfo, hold, quantize and x-noise itself.
/// Section, shaper, envelope, gain_computer, vca and pitch plug in through a
/// `GsClassicKernelTable` handed to `prepare()`; a graph using one of them is refused
/// when the table lacks it. `prepare()` allocates everything; `process()` and
/// `set_byte()` do not allocate and do not throw.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/synth/gs_classic/model_format.h"

namespace sonare::midi::synth::gs_classic {

/// `np.interp` over sorted points: end values held outside, bit-for-bit numpy's formula.
double gs_classic_interp(double x, const GsClassicPoint* points, std::size_t n) noexcept;

/// Expands one byte map into its 128-entry LUT, the port of soundings `_from_map`.
///
/// A `states` map without `*` gives an unnamed byte in [accept_lo, accept_hi] the value
/// of the nearest named byte (the lower on a tie) and any other unnamed byte the value
/// of `power_on`. Returns false on a spec that indexes outside its own entries.
bool gs_classic_expand_map(const GsClassicMapSpec& spec, const uint8_t* map_keys,
                           const double* map_values, GsClassicLut& out) noexcept;

class GsClassicGraph;

/// What a node kernel sees while it draws `[a, b)` of the current block.
class GsClassicRenderContext {
 public:
  /// Audio input `k` of `node`, indexed within the block.
  const double* input(const GsClassicNode& node, std::size_t k) const noexcept;
  /// Output port `port` of `node`, indexed within the block.
  double* output(const GsClassicNode& node, std::size_t port = 0) const noexcept;
  /// Value `k` of `node` at block sample `i`.
  double value(const GsClassicNode& node, std::size_t k, std::size_t i) const noexcept;
  /// Whether value `k` of `node` follows a control signal (varies per sample).
  bool value_varies(const GsClassicNode& node, std::size_t k) const noexcept;
  /// The node's input at absolute sample `at` (delay and pitch only); zero before the start.
  double history(const GsClassicNode& node, int64_t at) const noexcept;
  /// Absolute index of block sample 0.
  int64_t block_start() const noexcept;
  /// Whether the node is being drawn inside a loop.
  bool looped() const noexcept;
  /// Bumped on every byte change, so a kernel can rebuild what bytes decide.
  uint32_t byte_generation() const noexcept;
  const GsClassicModelSet& models() const noexcept;
  const GsClassicType& type() const noexcept;

 private:
  friend class GsClassicGraph;
  const GsClassicGraph* graph_ = nullptr;
  bool looped_ = false;
};

/// Doubles of per-node state a kind needs; null means none.
using GsClassicStateSizeFn = std::size_t (*)(const GsClassicModelSet&, const GsClassicType&,
                                             const GsClassicNode&);
/// Sets a node's state after it has been zeroed; null leaves it zero.
using GsClassicResetFn = void (*)(const GsClassicRenderContext&, const GsClassicNode&,
                                  double* state);
/// Draws `[a, b)` of the node's output(s).
using GsClassicRenderFn = void (*)(const GsClassicRenderContext&, const GsClassicNode&,
                                   double* state, std::size_t a, std::size_t b);

struct GsClassicNodeKernel {
  GsClassicStateSizeFn state_size = nullptr;
  GsClassicResetFn reset = nullptr;
  GsClassicRenderFn render = nullptr;
};

/// Kernels for the kinds the engine does not draw itself.
struct GsClassicKernelTable {
  GsClassicNodeKernel section;
  GsClassicNodeKernel shaper;
  GsClassicNodeKernel envelope;
  GsClassicNodeKernel gain_computer;
  GsClassicNodeKernel vca;
  GsClassicNodeKernel pitch;
};

/// Section designer and filter (sections.cpp).
std::size_t gs_classic_section_state_size(const GsClassicModelSet&, const GsClassicType&,
                                          const GsClassicNode&);
void gs_classic_section_reset(const GsClassicRenderContext&, const GsClassicNode&, double* state);
void gs_classic_render_section(const GsClassicRenderContext&, const GsClassicNode&, double* state,
                               std::size_t a, std::size_t b);
/// Level nodes (nodes_dynamics.cpp).
void gs_classic_render_shaper(const GsClassicRenderContext&, const GsClassicNode&, double* state,
                              std::size_t a, std::size_t b);
std::size_t gs_classic_envelope_state_size(const GsClassicModelSet&, const GsClassicType&,
                                           const GsClassicNode&);
void gs_classic_envelope_reset(const GsClassicRenderContext&, const GsClassicNode&, double* state);
void gs_classic_render_envelope(const GsClassicRenderContext&, const GsClassicNode&, double* state,
                                std::size_t a, std::size_t b);
void gs_classic_render_gain_computer(const GsClassicRenderContext&, const GsClassicNode&,
                                     double* state, std::size_t a, std::size_t b);
void gs_classic_render_vca(const GsClassicRenderContext&, const GsClassicNode&, double* state,
                           std::size_t a, std::size_t b);
/// Two-pointer pitch shifter (nodes_pitch.cpp).
std::size_t gs_classic_pitch_state_size(const GsClassicModelSet&, const GsClassicType&,
                                        const GsClassicNode&);
void gs_classic_render_pitch(const GsClassicRenderContext&, const GsClassicNode&, double* state,
                             std::size_t a, std::size_t b);
/// The table of the kernels above, for whoever builds a full classic unit.
const GsClassicKernelTable& gs_classic_extension_kernels() noexcept;

/// One type's graph, prepared for blocks of up to `max_block` samples.
class GsClassicGraph {
 public:
  GsClassicGraph() = default;
  GsClassicGraph(const GsClassicGraph&) = delete;
  GsClassicGraph& operator=(const GsClassicGraph&) = delete;

  /// Validates and allocates; `models` and `kernels` must outlive the graph.
  bool prepare(const GsClassicModelSet& models, const GsClassicType& type, std::size_t max_block,
               const GsClassicKernelTable* kernels = nullptr);
  /// Clears every state, history and the sample clock; noise reseeds.
  void reset() noexcept;
  /// Sets the byte in one of the 20 slots; values above 127 keep their low seven bits.
  void set_byte(std::size_t slot, uint8_t value) noexcept;
  uint8_t byte(std::size_t slot) const noexcept;
  /// Draws `n` samples at 32 kHz; any `n` is split into blocks of at most `max_block`.
  void process(const double* in_l, const double* in_r, double* out_l, double* out_r,
               std::size_t n) noexcept;

 private:
  friend class GsClassicRenderContext;

  void process_block(const double* in_l, const double* in_r, double* out_l, double* out_r,
                     std::size_t n) noexcept;
  void render_node(std::size_t index, std::size_t a, std::size_t b, bool looped) noexcept;
  void push_history(std::size_t index, std::size_t a, std::size_t b) noexcept;
  double* signal(uint16_t ref) const noexcept;
  std::size_t node_index(const GsClassicNode& node) const noexcept;
  const GsClassicNodeKernel* extension(GsClassicNodeKind kind) const noexcept;

  const GsClassicModelSet* models_ = nullptr;
  const GsClassicType* type_ = nullptr;
  const GsClassicKernelTable* kernels_ = nullptr;
  std::size_t max_block_ = 0;
  std::size_t n_signals_ = 0;
  mutable std::vector<double> signals_;
  std::vector<uint16_t> node_out_;
  std::vector<GsClassicNodeKernel> kernel_of_;
  std::vector<std::size_t> state_offset_;
  std::vector<double> state_;
  std::vector<std::size_t> history_offset_;  ///< SIZE_MAX for a node without history
  std::vector<int64_t> history_written_;
  std::vector<double> history_;
  std::size_t history_mask_ = 0;
  uint8_t bytes_[kGsClassicByteSlots] = {};
  uint32_t byte_generation_ = 0;
  int64_t block_start_ = 0;
  GsClassicRenderContext context_;
};

}  // namespace sonare::midi::synth::gs_classic
