#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <memory>

#include "engine/graph_runtime.h"
#include "rt/processor_base.h"

// These tests focus on two realtime-engine concurrency contracts that are hard
// to exercise deterministically with real threads:
//
//  1. The per-slot SysEx seqlock published by RealtimeEngine::push_midi_sysex
//     and consumed in apply_command (kMidiSysExImmediate). The seqlock protocol
//     is mirrored here as a standalone slot so its ordering logic can be driven
//     single-threaded, interleaving writer and reader steps by hand. A real
//     torn-read race is only reliably surfaced under a ThreadSanitizer stress
//     run; these cases pin the protocol's accept/reject decisions.
//
//  2. The graph-binding block-freeze invariant: GraphRuntime adopts the latest
//     published binding once per block (at the first sub-block, offset 0) and
//     freezes it for the remaining sub-blocks, matching the clip / automation
//     snapshots. This one IS observable through the public GraphRuntime API.

namespace {

// Standalone mirror of RealtimeEngine::SysExPayloadSlot and its seqlock
// protocol. The write/read steps are split so a test can interleave them. The
// payload lives in relaxed atomic words (word 0 = size, the rest = bytes) so the
// shared data itself is race-free, not merely gated by the generation guard.
struct SeqlockSlot {
  static constexpr size_t kMaxBytes = 512;
  static constexpr size_t kPayloadWords = (kMaxBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  std::atomic<uint32_t> size{0};
  std::array<std::atomic<uint32_t>, kPayloadWords> byte_words{};
  std::atomic<uint32_t> generation{0};

  void store_payload(const uint8_t* data, uint32_t n) noexcept {
    std::array<uint32_t, kPayloadWords> packed{};
    std::memcpy(packed.data(), data, n);
    const size_t words = (n + sizeof(uint32_t) - 1) / sizeof(uint32_t);
    for (size_t i = 0; i < words; ++i) {
      byte_words[i].store(packed[i], std::memory_order_relaxed);
    }
    size.store(n, std::memory_order_relaxed);
  }

  uint32_t load_payload(uint8_t* out) const noexcept {
    uint32_t n = size.load(std::memory_order_relaxed);
    if (n > kMaxBytes) n = kMaxBytes;
    std::array<uint32_t, kPayloadWords> packed{};
    const size_t words = (n + sizeof(uint32_t) - 1) / sizeof(uint32_t);
    for (size_t i = 0; i < words; ++i) {
      packed[i] = byte_words[i].load(std::memory_order_relaxed);
    }
    std::memcpy(out, packed.data(), n);
    return n;
  }
};

// Writer: mark the slot in progress (odd), then return the even "done" value the
// commit step will publish. Control thread is the sole writer, so it reads its
// own last even value with relaxed order.
uint32_t seqlock_write_begin(SeqlockSlot& slot) {
  const uint32_t base = slot.generation.load(std::memory_order_relaxed);
  slot.generation.store(base + 1u, std::memory_order_relaxed);  // odd: in progress
  std::atomic_thread_fence(std::memory_order_release);
  return base + 2u;  // even: to be published by seqlock_write_commit
}

void seqlock_write_payload(SeqlockSlot& slot, const uint8_t* data, uint32_t n) {
  slot.store_payload(data, n);
}

void seqlock_write_commit(SeqlockSlot& slot, uint32_t done_generation) {
  slot.generation.store(done_generation, std::memory_order_release);  // even: done
}

// Full publish, returning the even generation the command would carry.
uint32_t seqlock_write(SeqlockSlot& slot, const uint8_t* data, uint32_t n) {
  const uint32_t done = seqlock_write_begin(slot);
  seqlock_write_payload(slot, data, n);
  seqlock_write_commit(slot, done);
  return done;
}

// Reader (audio thread): bracket the payload copy with two acquire loads and
// accept only a stable even generation matching the command's generation.
bool seqlock_read(SeqlockSlot& slot, uint32_t generation, uint8_t* out, uint32_t* out_size) {
  const uint32_t seq_before = slot.generation.load(std::memory_order_acquire);
  const uint32_t size = slot.load_payload(out);
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t seq_after = slot.generation.load(std::memory_order_relaxed);
  if (seq_before == seq_after && (seq_before & 1u) == 0u && seq_before == generation && size > 0) {
    *out_size = size;
    return true;
  }
  return false;
}

class GainProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit GainProcessor(float gain) : gain_(gain) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= gain_;
      }
    }
  }
  void reset() override {}

 private:
  float gain_ = 1.0f;
};

std::unique_ptr<sonare::graph::Graph> make_gain_graph(float gain) {
  auto graph = std::make_unique<sonare::graph::Graph>();
  REQUIRE(graph->add_node("in", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph->add_node("gain", std::make_unique<GainProcessor>(gain), 1));
  REQUIRE(graph->add_node("out", std::make_unique<GainProcessor>(1.0f), 1));
  REQUIRE(graph->connect({"in", 0, "gain", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->connect({"gain", 0, "out", 0, sonare::graph::Connection::Mix::Add}));
  REQUIRE(graph->compile());
  graph->prepare(48000.0, 8);
  return graph;
}

}  // namespace

TEST_CASE("SysEx seqlock accepts a completed even-generation publish", "[engine][seqlock]") {
  SeqlockSlot slot;
  const std::array<uint8_t, 4> data{0xF0, 0x41, 0x12, 0xF7};
  const uint32_t generation = seqlock_write(slot, data.data(), 4);

  // First publish steps the per-slot sequence to the first even value.
  REQUIRE(generation == 2u);
  REQUIRE((generation & 1u) == 0u);

  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  uint32_t out_size = 0;
  REQUIRE(seqlock_read(slot, generation, out.data(), &out_size));
  REQUIRE(out_size == 4u);
  REQUIRE(std::memcmp(out.data(), data.data(), 4) == 0);
}

TEST_CASE("SysEx seqlock rejects a stale command generation after slot recycle",
          "[engine][seqlock]") {
  SeqlockSlot slot;
  const std::array<uint8_t, 3> first{0xF0, 0x01, 0xF7};
  const std::array<uint8_t, 3> second{0xF0, 0x02, 0xF7};

  const uint32_t gen_first = seqlock_write(slot, first.data(), 3);
  // The control thread recycles the same slot before the audio thread drains the
  // first command; the generation advances past the value the first command
  // carries.
  const uint32_t gen_second = seqlock_write(slot, second.data(), 3);
  REQUIRE(gen_second == gen_first + 2u);

  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  uint32_t out_size = 0;
  // A command still referencing the first generation is dropped, not dispatched
  // with the recycled payload.
  REQUIRE_FALSE(seqlock_read(slot, gen_first, out.data(), &out_size));
  // The current command is still accepted.
  REQUIRE(seqlock_read(slot, gen_second, out.data(), &out_size));
  REQUIRE(out_size == 3u);
  REQUIRE(std::memcmp(out.data(), second.data(), 3) == 0);
}

TEST_CASE("SysEx seqlock rejects a read while a write is in progress", "[engine][seqlock]") {
  SeqlockSlot slot;
  const std::array<uint8_t, 2> data{0xF0, 0xF7};

  // Begin a write: the slot generation is now odd (in progress). The done value
  // is not yet committed.
  const uint32_t done_generation = seqlock_write_begin(slot);
  seqlock_write_payload(slot, data.data(), 2);
  REQUIRE((slot.generation.load(std::memory_order_relaxed) & 1u) == 1u);

  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  uint32_t out_size = 0;
  // The reader observes the odd (in-progress) generation and refuses the copy,
  // whether it references the pending done value or anything else.
  REQUIRE_FALSE(seqlock_read(slot, done_generation, out.data(), &out_size));

  // Once committed, the same generation is accepted.
  seqlock_write_commit(slot, done_generation);
  REQUIRE(seqlock_read(slot, done_generation, out.data(), &out_size));
  REQUIRE(out_size == 2u);
}

TEST_CASE("SysEx seqlock rejects a copy torn by a concurrent rewrite", "[engine][seqlock]") {
  SeqlockSlot slot;
  const std::array<uint8_t, 3> original{0xF0, 0xAA, 0xF7};
  const std::array<uint8_t, 3> rewrite{0xF0, 0xBB, 0xF7};
  const uint32_t generation = seqlock_write(slot, original.data(), 3);

  // Model the torn read by hand: the reader latches the pre-copy generation, and
  // before it latches the post-copy generation the control thread fully rewrites
  // the slot. The bracketing generations then differ and the copy is rejected.
  const uint32_t seq_before = slot.generation.load(std::memory_order_acquire);
  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  const uint32_t size = slot.load_payload(out.data());
  // Concurrent rewrite lands between the two generation reads.
  const uint32_t gen_rewrite = seqlock_write(slot, rewrite.data(), 3);
  const uint32_t seq_after = slot.generation.load(std::memory_order_relaxed);

  const bool accepted =
      seq_before == seq_after && (seq_before & 1u) == 0u && seq_before == generation && size > 0;
  REQUIRE_FALSE(accepted);
  REQUIRE(gen_rewrite != generation);
}

TEST_CASE("SysEx seqlock round-trips a multi-word payload exactly", "[engine][seqlock]") {
  // The relaxed word store must reconstruct a payload whose length is not a whole
  // number of 32-bit words (partial trailing word) across many words.
  SeqlockSlot slot;
  std::array<uint8_t, 130> data{};
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 7u + 1u) & 0xFFu);
  }
  const uint32_t generation = seqlock_write(slot, data.data(), static_cast<uint32_t>(data.size()));

  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  uint32_t out_size = 0;
  REQUIRE(seqlock_read(slot, generation, out.data(), &out_size));
  REQUIRE(out_size == data.size());
  REQUIRE(std::memcmp(out.data(), data.data(), data.size()) == 0);
}

TEST_CASE("SysEx seqlock rejects a never-written slot", "[engine][seqlock]") {
  SeqlockSlot slot;  // generation 0, size 0
  std::array<uint8_t, SeqlockSlot::kMaxBytes> out{};
  uint32_t out_size = 0;
  // No command carries generation 0, and a zero-length payload is refused too.
  REQUIRE_FALSE(seqlock_read(slot, 0u, out.data(), &out_size));
}

TEST_CASE("GraphRuntime freezes the adopted binding for the whole block",
          "[engine][graph_runtime]") {
  // A control-thread graph swap published mid-block must not take effect until
  // the next block. The binding is adopted only at the block's first sub-block
  // (offset 0); later sub-blocks reuse the frozen binding.
  auto first = make_gain_graph(2.0f);
  auto second = make_gain_graph(8.0f);

  sonare::engine::GraphRuntime runtime;
  REQUIRE(runtime.bind(first.get(), "in", "out", 1));

  std::array<float, 8> buffer{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  float* io[] = {buffer.data()};

  // Block head (offset 0): adopt the 2x graph and render the first sub-block.
  runtime.process(io, 1, 0, 4);
  REQUIRE(buffer[0] == 2.0f);
  REQUIRE(buffer[3] == 8.0f);

  // Publish the 8x graph mid-block.
  REQUIRE(runtime.swap(second.get(), "in", "out", 1) == first.get());

  // Mid-block sub-block (offset 4, not the block head): the binding stays frozen
  // on the 2x graph, so the swap is NOT yet audible. Re-acquiring per sub-block
  // would apply 8x here.
  runtime.process(io, 1, 4, 4);
  REQUIRE(buffer[4] == 10.0f);  // input 5 * 2
  REQUIRE(buffer[7] == 16.0f);  // input 8 * 2

  // Next block head (offset 0): the 8x graph is finally adopted.
  std::array<float, 4> next{1.0f, 2.0f, 3.0f, 4.0f};
  float* next_io[] = {next.data()};
  runtime.process(next_io, 1, 0, 4);
  REQUIRE(next[0] == 8.0f);
  REQUIRE(next[3] == 32.0f);
}
