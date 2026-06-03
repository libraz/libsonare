#include "rt/spsc_queue.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>
#include <type_traits>

#include "rt/command.h"
#include "util/exception.h"

namespace {

struct QueueRecord {
  uint64_t sequence = 0;
  uint32_t payload = 0;
};

static_assert(std::is_trivially_copyable_v<QueueRecord>);

}  // namespace

TEST_CASE("SpscQueue rejects invalid capacities", "[rt][spsc]") {
  sonare::rt::SpscQueue<QueueRecord> queue;

  REQUIRE_THROWS_AS(queue.reserve(0), sonare::SonareException);
  REQUIRE_THROWS_AS(queue.reserve(3), sonare::SonareException);
}

TEST_CASE("SpscQueue preserves FIFO order and full/empty states", "[rt][spsc]") {
  sonare::rt::SpscQueue<QueueRecord> queue;
  queue.reserve(4);

  QueueRecord out{};
  REQUIRE(queue.capacity() == 4);
  REQUIRE(queue.empty());
  REQUIRE_FALSE(queue.pop(out));

  REQUIRE(queue.push({0, 10}));
  REQUIRE(queue.push({1, 11}));
  REQUIRE(queue.push({2, 12}));
  REQUIRE(queue.push({3, 13}));
  REQUIRE(queue.size_approx() == 4);
  REQUIRE_FALSE(queue.push({4, 14}));

  for (uint64_t i = 0; i < 4; ++i) {
    REQUIRE(queue.pop(out));
    REQUIRE(out.sequence == i);
    REQUIRE(out.payload == 10 + i);
  }
  REQUIRE(queue.empty());
  REQUIRE_FALSE(queue.pop(out));
}

TEST_CASE("SpscQueue reuses reserved storage across wraparound", "[rt][spsc]") {
  sonare::rt::SpscQueue<QueueRecord> queue;
  queue.reserve(8);
  const size_t capacity = queue.capacity();

  QueueRecord out{};
  for (uint64_t i = 0; i < 1024; ++i) {
    REQUIRE(queue.push({i, static_cast<uint32_t>(i ^ 0x55u)}));
    REQUIRE(queue.pop(out));
    REQUIRE(out.sequence == i);
    REQUIRE(out.payload == static_cast<uint32_t>(i ^ 0x55u));
    REQUIRE(queue.capacity() == capacity);
  }
}

TEST_CASE("SpscQueue handles producer consumer stress", "[rt][spsc]") {
  constexpr uint64_t kCount = 200000;

  sonare::rt::SpscQueue<QueueRecord> queue;
  queue.reserve(1024);

  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (uint64_t i = 0; i < kCount; ++i) {
      QueueRecord item{i, static_cast<uint32_t>(i * 2654435761u)};
      while (!queue.push(item)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    uint64_t expected = 0;
    QueueRecord item{};
    while (expected < kCount) {
      if (!queue.pop(item)) {
        if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
        continue;
      }
      if (item.sequence != expected ||
          item.payload != static_cast<uint32_t>(expected * 2654435761u)) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      ++expected;
    }
  });

  producer.join();
  consumer.join();

  REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
  REQUIRE(queue.empty());
}

TEST_CASE("Realtime command records keep POD ABI properties", "[rt][command]") {
  using sonare::rt::Command;
  using sonare::rt::CommandArg;
  using sonare::rt::CommandType;

  static_assert(std::is_trivially_copyable_v<CommandArg>);
  static_assert(std::is_trivially_copyable_v<Command>);

  Command command{};
  command.type = CommandType::kTransportSeekSample;
  command.target_id = 7;
  command.sample_time = 128;
  command.arg.i = 48000;

  REQUIRE(sonare::rt::kEngineAbiVersion == 3);
  REQUIRE(command.type == CommandType::kTransportSeekSample);
  REQUIRE(command.target_id == 7);
  REQUIRE(command.sample_time == 128);
  REQUIRE(command.arg.i == 48000);

  // The Command record is a fixed 24-byte POD: a 16-bit CommandType + 32-bit
  // target_id (padded), a 64-bit sample_time, and the 64-bit CommandArg union.
  // The new MIDI commands reuse those existing fields, so the layout/ABI is
  // unchanged. Pin the size so adding command types never silently grows it.
  static_assert(sizeof(Command) == 24, "Command must stay 24-byte POD");
}

TEST_CASE("Queueable scalar MIDI commands survive the SPSC queue", "[rt][command][midi]") {
  using sonare::rt::Command;
  using sonare::rt::CommandType;

  sonare::rt::SpscQueue<Command> queue;
  queue.reserve(4);

  // Immediate CC: pack group/channel/controller/value7 into arg.i (matching the
  // encoding documented in command.h and decoded by apply_command).
  constexpr uint8_t kGroup = 0x0;
  constexpr uint8_t kChannel = 0x3;
  constexpr uint8_t kController = 74;  // filter cutoff
  constexpr uint8_t kValue7 = 100;
  const int64_t packed = static_cast<int64_t>(
      static_cast<uint64_t>(kValue7) | (static_cast<uint64_t>(kController) << 8) |
      (static_cast<uint64_t>(kChannel) << 16) | (static_cast<uint64_t>(kGroup) << 24));

  Command cc{};
  cc.type = CommandType::kMidiCcImmediate;
  cc.target_id = 42;  // MIDI destination id
  cc.sample_time = 256;
  cc.arg.i = packed;

  Command panic{};
  panic.type = CommandType::kMidiAllNotesOff;
  panic.target_id = 0;
  panic.sample_time = 512;

  REQUIRE(queue.push(cc));
  REQUIRE(queue.push(panic));

  Command out{};
  REQUIRE(queue.pop(out));
  REQUIRE(out.type == CommandType::kMidiCcImmediate);
  REQUIRE(out.target_id == 42);
  REQUIRE(out.sample_time == 256);
  REQUIRE(out.arg.i == packed);
  const uint64_t round = static_cast<uint64_t>(out.arg.i);
  REQUIRE(static_cast<uint8_t>(round & 0x7Fu) == kValue7);
  REQUIRE(static_cast<uint8_t>((round >> 8) & 0x7Fu) == kController);
  REQUIRE(static_cast<uint8_t>((round >> 16) & 0x0Fu) == kChannel);
  REQUIRE(static_cast<uint8_t>((round >> 24) & 0x0Fu) == kGroup);

  REQUIRE(queue.pop(out));
  REQUIRE(out.type == CommandType::kMidiAllNotesOff);
  REQUIRE(out.sample_time == 512);

  REQUIRE(queue.empty());
}
