/// @file gs_efx_realization_test.cpp
/// @brief How the SF2 player realises and runs a GS insertion unit: stages kept
///        at their chain positions, enable switches faded without a rebuild, the
///        classic realization's unit and its live byte edits, and the edits that
///        must rebuild (a realization switch, a changed stage shape, a lost
///        persistent update).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_processor.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/part_fx_stage.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/alloc_guard.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_classic/classic_unit.h"
#include "midi/synth/gs_classic/model_registry.h"
#endif

namespace {

namespace s = sonare::midi::synth;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr uint16_t kStereoDelay = 0x0150;
#if defined(SONARE_MIDI_WITH_FX)
constexpr uint16_t kOverdrive = 0x0110;
#endif

/// A framed GS DT1 write of @p value at 40 03 @p offset (offset 0x03 is EFX
/// PARAMETER 1), with the checksum.
std::array<uint8_t, 11> efx_write(uint8_t offset, uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset, value, 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// A write of EFX PARAMETER @p slot + 1.
std::array<uint8_t, 11> slot_write(uint8_t slot, uint8_t value) {
  return efx_write(static_cast<uint8_t>(0x03 + slot), value);
}

/// A framed EFX type selection.
std::array<uint8_t, 12> type_write(uint16_t type) {
  const auto msb = static_cast<uint8_t>(type >> 8);
  const auto lsb = static_cast<uint8_t>(type & 0x7F);
  std::array<uint8_t, 12> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9];
  m[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// A type write at the uniform extension address (40 30-3F 00). The spec
/// block uses 40 03, while 40 31 addresses unit 1.
#if defined(SONARE_MIDI_WITH_FX)
std::array<uint8_t, 12> type_write_at(uint8_t address, uint16_t type) {
  const auto msb = static_cast<uint8_t>(type >> 8);
  const auto lsb = static_cast<uint8_t>(type & 0x7F);
  std::array<uint8_t, 12> m = {0xF0,    0x41, 0x10, 0x42, 0x12, 0x40,
                               address, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9];
  m[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// Part 1 routed to an EFX assignment value. Value 2 selects extension unit 1.
std::array<uint8_t, 11> part_efx_assign(uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, value, 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}
#endif

/// A variable-length GS DT1 frame used for bulk runs that cross the reserved
/// addresses between the uniform EFX blocks. The checksum covers the exact
/// wire bytes, so the same frame exercises every public PartFxStage entry
/// point rather than a helper that bypasses framing.
std::vector<uint8_t> efx_bulk(uint32_t addr, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> m{0xF0,
                         0x41,
                         0x10,
                         0x42,
                         0x12,
                         static_cast<uint8_t>((addr >> 16) & 0x7Fu),
                         static_cast<uint8_t>((addr >> 8) & 0x7Fu),
                         static_cast<uint8_t>(addr & 0x7Fu)};
  m.insert(m.end(), data.begin(), data.end());
  uint32_t sum = m[5] + m[6] + m[7];
  for (const uint8_t value : data) sum += value & 0x7Fu;
  m.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  m.push_back(0xF7);
  return m;
}

/// Part 1 (channel 0) routed into the spec unit.
constexpr uint8_t kPartOn[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};

template <size_t N>
void send(s::Sf2Player& player, const std::array<uint8_t, N>& m) {
  player.on_control_sysex(m.data(), m.size());
}

/// A unit holding @p type with its own power-on parameters.
s::GsEfx efx_holding(uint16_t type) {
  s::GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = s::gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}

/// Lifecycle counts shared by the stand-in stages of one player.
struct Counters {
  int prepares = 0;
  int resets = 0;
  int processes = 0;
  bool log_set_params = true;
  int parameter_sets = 0;
  std::array<float, 128> last_parameter_by_id{};
  std::array<bool, 128> has_parameter_by_id{};
  std::vector<std::pair<std::string, float>> set_params;
  std::vector<float> probe;  ///< Left channel of the probe's last block.
};

/// A stand-in stage: writes a constant 1 over its input (so what reaches the
/// probe after it is its fade) or passes through recording what it saw.
class StandIn final : public sonare::rt::ProcessorBase {
 public:
  StandIn(std::shared_ptr<Counters> counters, std::string name, bool writer,
          std::vector<std::string> keys, int tail_samples = 0, int latency_samples_q8 = 0)
      : counters_(std::move(counters)),
        name_(std::move(name)),
        writer_(writer),
        keys_(std::move(keys)),
        tail_samples_(tail_samples),
        latency_samples_q8_(latency_samples_q8) {}
  void prepare(double, int) override { ++counters_->prepares; }
  void process(float* const* ch, int, int n) override {
    ++counters_->processes;
    if (writer_) {
      for (int i = 0; i < n; ++i) ch[0][i] = ch[1][i] = 1.0f;
      return;
    }
    counters_->probe.assign(ch[0], ch[0] + n);
  }
  void reset() override { ++counters_->resets; }
  int latency_samples_q8() const noexcept override { return latency_samples_q8_; }
  int tail_samples() const noexcept override { return tail_samples_; }
  bool set_parameter_impl(unsigned int id, float value) override {
    ++counters_->parameter_sets;
    if (id < counters_->last_parameter_by_id.size()) {
      counters_->last_parameter_by_id[id] = value;
      counters_->has_parameter_by_id[id] = true;
    }
    if (counters_->log_set_params)
      counters_->set_params.emplace_back(name_ + "." + keys_.at(id), value);
    return true;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (size_t i = 0; i < keys_.size(); ++i)
      out.push_back({keys_[i], static_cast<unsigned int>(i)});
    return out;
  }

 private:
  std::shared_ptr<Counters> counters_;
  std::string name_;
  bool writer_;
  std::vector<std::string> keys_;
  int tail_samples_;
  int latency_samples_q8_;
};

/// A finite, deterministic delay used by prepared-runtime tests. The stage
/// keeps exactly its declared number of samples, then returns to zero when its
/// input is silent; this makes a stale unit's drain and reactivation observable
/// without depending on a feedback effect's numerical decay.
class FiniteTailDelay final : public sonare::rt::ProcessorBase {
 public:
  FiniteTailDelay(std::shared_ptr<Counters> counters, int tail_samples)
      : counters_(std::move(counters)), tail_samples_(tail_samples) {}

  void prepare(double, int) override {
    ++counters_->prepares;
    delay_.assign(static_cast<size_t>(std::max(0, tail_samples_)), 0.0f);
    write_index_ = 0;
  }

  void process(float* const* channels, int num_channels, int n) override {
    ++counters_->processes;
    if (channels == nullptr || channels[0] == nullptr || n <= 0) return;
    if (delay_.empty()) return;
    float* right = num_channels > 1 ? channels[1] : nullptr;
    for (int i = 0; i < n; ++i) {
      const float delayed = delay_[write_index_];
      delay_[write_index_] = channels[0][i];
      channels[0][i] = delayed;
      if (right != nullptr) right[i] = delayed;
      write_index_ = (write_index_ + 1) % delay_.size();
    }
  }

  void reset() override {
    ++counters_->resets;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    write_index_ = 0;
  }

  int tail_samples() const noexcept override { return tail_samples_; }

  bool set_parameter_impl(unsigned int id, float value) override {
    ++counters_->parameter_sets;
    if (id < counters_->last_parameter_by_id.size()) {
      counters_->last_parameter_by_id[id] = value;
      counters_->has_parameter_by_id[id] = true;
    }
    return true;
  }

  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    out.reserve(s::kGsEfxRowKeys.size());
    for (size_t i = 0; i < s::kGsEfxRowKeys.size(); ++i) {
      out.push_back({std::string(s::kGsEfxRowKeys[i]), static_cast<unsigned int>(i)});
    }
    return out;
  }

 private:
  std::shared_ptr<Counters> counters_;
  int tail_samples_;
  std::vector<float> delay_;
  size_t write_index_ = 0;
};

/// A realtime-safe EFX stage whose declared tail can change when a queued
/// parameter reaches the audio thread. Construction-time parameter writes
/// leave the initial tail alone; tests opt into runtime updates explicitly so
/// a freshly built snapshot can be made low without consuming an old queue.
struct LiveTailState {
  int tail = 64;
  int next_tail = 64;
  bool runtime_update = false;
  int updates = 0;
};

class LiveTailDelay final : public sonare::rt::ProcessorBase {
 public:
  explicit LiveTailDelay(std::shared_ptr<LiveTailState> state) : state_(std::move(state)) {}

  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int tail_samples() const noexcept override { return state_->tail; }

  bool set_parameter_impl(unsigned int, float) override {
    ++state_->updates;
    if (state_->runtime_update) state_->tail = state_->next_tail;
    return true;
  }

  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    out.reserve(s::kGsEfxRowKeys.size());
    for (size_t i = 0; i < s::kGsEfxRowKeys.size(); ++i) {
      out.push_back({std::string(s::kGsEfxRowKeys[i]), static_cast<unsigned int>(i)});
    }
    return out;
  }

 private:
  std::shared_ptr<LiveTailState> state_;
};

s::PartFxStageConfig live_tail_config(const std::shared_ptr<LiveTailState>& state) {
  s::PartFxStageConfig cfg;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [state](std::string_view name, std::string_view) {
    if (name == "effects.delay.stereo") {
      return std::unique_ptr<sonare::rt::ProcessorBase>(new LiveTailDelay(state));
    }
    return std::unique_ptr<sonare::rt::ProcessorBase>(new StandIn(
        std::make_shared<Counters>(), std::string(name), false, std::vector<std::string>{}));
  };
  return cfg;
}

void prepare_live_tail_stage(s::PartFxStage& fx) {
  fx.prepare(kRate);
  fx.assign_part(0, 1);
  const auto type = type_write(kStereoDelay);
  REQUIRE(fx.apply_unit_sysex(type.data(), type.size()));
  fx.publish();
  fx.acquire();
  REQUIRE(fx.current() != nullptr);
}

struct LiveTailHost final : s::PartFxHost {
  float position = 1.0f;

  float part_controller_position(int, uint8_t) const noexcept override { return position; }
  float part_pan_units(int) const noexcept override { return 0.0f; }
};

void render_block(s::Sf2Player& player, std::vector<float>* left = nullptr) {
  std::vector<float> l(kBlock, 0.0f);
  std::vector<float> r(kBlock, 0.0f);
  float* chans[2] = {l.data(), r.data()};
  player.process(chans, 2, kBlock);
  if (left != nullptr) left->insert(left->end(), l.begin(), l.end());
}

/// Index of @p name in the generated row stage table.
uint16_t row_stage(std::string_view name) {
  for (size_t i = 0; i < s::kGsEfxRowStages.size(); ++i) {
    if (s::kGsEfxRowStages[i] == name) return static_cast<uint16_t>(i);
  }
  FAIL("no row stage " << name);
  return 0;
}

/// A slot of @p type that takes both 00 and 7F, which is what a switch byte in
/// these cases needs.
uint8_t switchable_slot(uint16_t type) {
  for (uint8_t slot = 0; slot < 20; ++slot) {
    if (s::gs_efx_parameter_takes(type, slot, 0x00) &&
        s::gs_efx_parameter_takes(type, slot, 0x7F)) {
      return slot;
    }
  }
  FAIL("no switchable slot on type " << type);
  return 0;
}

struct TailFactoryPlan {
  std::shared_ptr<Counters> counters = std::make_shared<Counters>();
  std::vector<int> tails;
  std::vector<int> latency_samples_q8;
  size_t next = 0;
};

std::shared_ptr<TailFactoryPlan> make_tail_factory_plan(
    const std::vector<int>& tails, const std::vector<int>& latency_samples_q8 = {}) {
  auto plan = std::make_shared<TailFactoryPlan>();
  plan->tails = tails;
  plan->latency_samples_q8 = latency_samples_q8;
  if (plan->latency_samples_q8.empty()) plan->latency_samples_q8.assign(plan->tails.size(), 0);
  REQUIRE(plan->latency_samples_q8.size() == plan->tails.size());
  return plan;
}

s::GsEfxStageFactory tail_factory(const std::shared_ptr<TailFactoryPlan>& plan) {
  return [plan](std::string_view name, std::string_view) {
    const size_t index = plan->next++;
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(plan->counters, std::string(name), false, {}, plan->tails.at(index),
                    plan->latency_samples_q8.at(index)));
  };
}

s::GsEfxStageFactory prepared_tail_factory(const std::shared_ptr<TailFactoryPlan>& plan) {
  return [plan](std::string_view name, std::string_view) {
    const size_t index = plan->next++;
    std::vector<std::string> keys;
    keys.reserve(s::kGsEfxRowKeys.size());
    for (const std::string_view key : s::kGsEfxRowKeys) keys.emplace_back(key);
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(plan->counters, std::string(name), false, std::move(keys),
                    plan->tails.at(index), plan->latency_samples_q8.at(index)));
  };
}

}  // namespace

TEST_CASE("an enable switch fades its stage and resets it on return", "[gs-efx-realization]") {
  // The stereo delay's own stage is the switched one; every stage after it is a
  // probe, so what the probe reads is 1 x the delay stage's fade over silence.
  const uint8_t slot = switchable_slot(kStereoDelay);
  s::GsEfxEnable enable{};
  enable.type = kStereoDelay;
  enable.slot = slot;
  enable.mode = s::kGsEfxEnableStages;
  enable.stages[0] = row_stage("effects.delay.stereo");
  enable.ordinals[0] = 0;
  enable.n_stages = 1;
  enable.on_mask[2] = 0xFFFFFFFFu;  // bytes 40-7F turn it on
  enable.on_mask[3] = 0xFFFFFFFFu;
  const s::GsEfxRowView rows{s::kGsEfxBindingRows.data(), s::kGsEfxBindingRows.size(), &enable, 1};

  auto counters = std::make_shared<Counters>();
  s::Sf2PlayerConfig cfg;
  cfg.dc_block = false;
  cfg.insert_factory = [counters](std::string_view name, std::string_view) {
    const bool writer = name == "effects.delay.stereo";
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(counters, std::string(name), writer, {}));
  };
  s::Sf2Player player(cfg);
  player.set_gs_efx_rows(&rows);
  player.prepare(kRate, kBlock);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  send(player, type_write(kStereoDelay));
  send(player, slot_write(slot, 0x7F));
  render_block(player);
  render_block(player);
  REQUIRE(counters->probe.back() == 1.0f);
  const uint32_t generation = player.gs_efx_generation();
  const int prepares = counters->prepares;

  // Off: faded out over 5 ms inside the next block, then passed over.
  send(player, slot_write(slot, 0x00));
  REQUIRE(player.gs_efx_generation() == generation);
  render_block(player);
  REQUIRE(counters->probe.front() > 0.0f);
  REQUIRE(counters->probe.front() < 1.0f);
  const auto fade_len = static_cast<size_t>(std::ceil(s::kSf2EfxFadeMs * 1e-3 * kRate));
  REQUIRE(fade_len < counters->probe.size());
  REQUIRE(counters->probe[fade_len] == 0.0f);
  const int processes_off = counters->processes;
  render_block(player);
  // Only the probes ran: the switched stage's state is frozen.
  const size_t probes = s::gs_efx_insert_chain(efx_holding(kStereoDelay), rows).size() - 1;
  REQUIRE(static_cast<size_t>(counters->processes - processes_off) == probes);
  REQUIRE(counters->probe.back() == 0.0f);
  const int resets = counters->resets;

  // On again: reset first, then faded in.
  send(player, slot_write(slot, 0x7F));
  REQUIRE(player.gs_efx_generation() == generation);
  render_block(player);
  REQUIRE(counters->resets == resets + 1);
  REQUIRE(counters->probe.front() > 0.0f);
  REQUIRE(counters->probe.front() < 0.5f);
  REQUIRE(counters->probe.back() == 1.0f);
  REQUIRE(counters->prepares == prepares);  // never rebuilt
}

TEST_CASE("queued EFX enable updates survive a unit reset", "[gs-efx-realization]") {
  const uint8_t slot = switchable_slot(kStereoDelay);
  s::GsEfxEnable enable{};
  enable.type = kStereoDelay;
  enable.slot = slot;
  enable.mode = s::kGsEfxEnableStages;
  enable.stages[0] = row_stage("effects.delay.stereo");
  enable.n_stages = 1;
  enable.on_mask[2] = enable.on_mask[3] = 0xFFFFFFFFu;
  const s::GsEfxRowView rows{nullptr, 0, &enable, 1};
  auto counters = std::make_shared<Counters>();
  s::PartFxStageConfig cfg;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [counters](std::string_view name, std::string_view) {
    return std::make_unique<StandIn>(counters, std::string(name), true, std::vector<std::string>{});
  };
  s::PartFxStage fx(cfg);
  fx.set_rows(&rows);
  fx.prepare(kRate);
  fx.assign_part(0, 1);
  const auto type = type_write(kStereoDelay);
  REQUIRE(fx.apply_unit_sysex(type.data(), type.size()));
  const auto initial = slot_write(slot, 0x7F);
  REQUIRE(fx.apply_unit_sysex(initial.data(), initial.size()));
  fx.publish();
  fx.acquire();
  const uint32_t generation = fx.generation();
  REQUIRE(fx.current() != nullptr);
  // This test owns the quiescent snapshot; reset mutates only its audio state.
  auto& unit = const_cast<s::Sf2EfxUnitRt&>(fx.current()->units[0]);
  const int index = s::sf2_find_efx_stage(unit, "effects.delay.stereo", 0);
  REQUIRE(index >= 0);
  auto& stage = unit.stages[static_cast<size_t>(index)];
  REQUIRE(stage.enabled_target);
  for (const bool on : {false, true}) {
    const auto update = slot_write(slot, on ? 0x7F : 0x00);
    REQUIRE_FALSE(fx.apply_control_sysex(update.data(), update.size()));
    fx.settle_block(LiveTailHost{});
    CHECK(fx.generation() == generation);
    CHECK(stage.enabled_now == on);
    CHECK(stage.enabled_target == on);
    s::sf2_reset_efx_unit(unit);
    CHECK(stage.enabled_now == on);
    CHECK(stage.fade == (on ? 1.0f : 0.0f));
  }
}

TEST_CASE("GS EFX tails follow serial and parallel graph topology", "[gs-efx-realization]") {
  struct TailCase {
    uint16_t type;
    std::vector<int> tails;
    std::vector<int> latency_samples_q8;
    int expected;
  };

  std::vector<int> parallel_fractional_latency(17, 0);
  parallel_fractional_latency[0] = 0x180;  // 1.5 samples: compensation has a 2-sample support tail.
  const std::vector<TailCase> cases = {
      // 8 stages in each half and one common back stage: A sums to 98, B to 72,
      // and the back is 21, so the graph tail is 119.
      {0x1103, {3, 5, 7, 11, 13, 17, 19, 23, 2, 4, 6, 8, 10, 12, 14, 16, 21}, {}, 119},
      // The serial OD graph has five front stages (40) and three back stages (75).
      {0x0110, {4, 6, 8, 10, 12, 20, 25, 30}, {}, 115},
      // The same parallel graph also checks that fractional alignment support is
      // included in both public tail surfaces.
      {0x1103,
       {3, 5, 7, 11, 13, 17, 19, 23, 2, 4, 6, 8, 10, 12, 14, 16, 21},
       parallel_fractional_latency,
       121},
  };

  for (const TailCase& test : cases) {
    INFO("EFX type " << test.type);
    const s::GsEfx state = efx_holding(test.type);
    const std::vector<s::GsEfxStage> chain = s::gs_efx_insert_chain(state);
    REQUIRE(chain.size() == test.tails.size());
    if (test.type == 0x1103) {
      REQUIRE(chain.size() == 17);
      for (size_t i = 0; i < 8; ++i) REQUIRE(chain[i].branch == s::kGsEfxBranchHalfA);
      for (size_t i = 8; i < 16; ++i) REQUIRE(chain[i].branch == s::kGsEfxBranchHalfB);
      REQUIRE(chain.back().branch == s::kGsEfxBranchBack);
    } else {
      REQUIRE(chain.size() == 8);
      for (size_t i = 0; i < 5; ++i) REQUIRE(chain[i].branch == s::kGsEfxBranchFront);
      for (size_t i = 5; i < chain.size(); ++i) REQUIRE(chain[i].branch == s::kGsEfxBranchBack);
    }

    const std::shared_ptr<TailFactoryPlan> facade_plan =
        make_tail_factory_plan(test.tails, test.latency_samples_q8);
    s::GsEfxProcessor facade(state, s::GsEfxRealization::kModern, tail_factory(facade_plan));
    REQUIRE(facade_plan->next == chain.size());
    facade.prepare(kRate, kBlock);
    REQUIRE(facade.tail_samples() == test.expected);

    const std::shared_ptr<TailFactoryPlan> part_plan =
        make_tail_factory_plan(test.tails, test.latency_samples_q8);
    s::PartFxStageConfig cfg;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = tail_factory(part_plan);
    s::PartFxStage fx(cfg);
    fx.prepare(kRate);
    fx.assign_part(0, 1);  // Part 0 feeds spec unit 0.
    const auto type = type_write(test.type);
    REQUIRE(fx.apply_unit_sysex(type.data(), type.size()));
    fx.publish();
    fx.acquire();
    REQUIRE(fx.current() != nullptr);
    REQUIRE(fx.current()->unit_fed[0]);
    REQUIRE(part_plan->next == chain.size());
    REQUIRE(fx.tail_samples() == test.expected);
    REQUIRE(fx.tail_samples() == facade.tail_samples());
  }
}

TEST_CASE("a live EFX tail raises metadata without rebuilding its generation",
          "[gs-efx-realization][gs-efx-tail]") {
  const auto state = std::make_shared<LiveTailState>();
  s::PartFxStage fx(live_tail_config(state));
  prepare_live_tail_stage(fx);
  REQUIRE(fx.tail_samples() == 64);
  const uint32_t generation = fx.generation();

  state->runtime_update = true;
  state->next_tail = 8192;
  const int updates_before_raise = state->updates;
  const auto raise = slot_write(2, 0x71);
  REQUIRE_FALSE(fx.apply_control_sysex(raise.data(), raise.size()));
  REQUIRE(fx.generation() == generation);
  fx.settle_block(LiveTailHost{});
  REQUIRE(state->updates > updates_before_raise);
  REQUIRE(fx.tail_samples() == 8192);

  // The same graph can report a smaller current tail after the parameter is
  // returned, but the active bound stays high until a fresh snapshot replaces
  // the graph.
  state->next_tail = 64;
  const auto lower = slot_write(2, 0x40);
  REQUIRE_FALSE(fx.apply_control_sysex(lower.data(), lower.size()));
  REQUIRE(fx.generation() == generation);
  fx.settle_block(LiveTailHost{});
  REQUIRE(fx.tail_samples() == 8192);
}

TEST_CASE("a live EFX CONTROL tail raises metadata without rebuilding its generation",
          "[gs-efx-realization][gs-efx-tail]") {
  const auto state = std::make_shared<LiveTailState>();
  s::PartFxStage fx(live_tail_config(state));
  fx.prepare(kRate);
  fx.assign_part(0, 1);
  const auto type = type_write(kStereoDelay);
  REQUIRE(fx.apply_unit_sysex(type.data(), type.size()));
  const auto source = efx_write(0x1B, 0x01);  // CONTROL 1 <- CC1.
  REQUIRE(fx.apply_unit_sysex(source.data(), source.size()));
  const auto depth = efx_write(0x1C, 0x7F);  // Full positive modulation.
  REQUIRE(fx.apply_unit_sysex(depth.data(), depth.size()));
  fx.publish();
  fx.acquire();
  REQUIRE(fx.tail_samples() == 64);
  const uint32_t generation = fx.generation();

  state->runtime_update = true;
  state->next_tail = 8192;
  const int updates_before_control = state->updates;
  LiveTailHost host;
  fx.settle_block(host);
  REQUIRE(state->updates > updates_before_control);
  REQUIRE(fx.generation() == generation);
  REQUIRE(fx.tail_samples() == 8192);
}

TEST_CASE("published EFX tails are visible before adoption and reset on a fresh graph",
          "[gs-efx-realization][gs-efx-tail]") {
  const auto state = std::make_shared<LiveTailState>();
  s::PartFxStage fx(live_tail_config(state));
  prepare_live_tail_stage(fx);
  REQUIRE(fx.tail_samples() == 64);

  state->tail = 8192;
  state->next_tail = 8192;
  fx.publish();
  REQUIRE(fx.tail_samples() == 8192);
  fx.acquire();
  REQUIRE(fx.tail_samples() == 8192);

  state->tail = 64;
  state->next_tail = 64;
  fx.publish();
  // The high active graph remains the bound until the low replacement is
  // adopted by the audio side.
  REQUIRE(fx.tail_samples() == 8192);
  fx.acquire();
  REQUIRE(fx.tail_samples() == 64);
}

TEST_CASE("a queued EFX update from a stale generation cannot raise the new graph",
          "[gs-efx-realization][gs-efx-tail]") {
  const auto state = std::make_shared<LiveTailState>();
  s::PartFxStage fx(live_tail_config(state));
  prepare_live_tail_stage(fx);
  REQUIRE(fx.tail_samples() == 64);
  const uint32_t old_generation = fx.generation();

  state->next_tail = 8192;
  const auto raise = slot_write(2, 0x71);
  REQUIRE_FALSE(fx.apply_control_sysex(raise.data(), raise.size()));
  REQUIRE(fx.generation() == old_generation);

  // Publish and adopt a low replacement before the queued old-generation
  // record is drained. The record must be discarded by its generation check.
  state->tail = 64;
  state->next_tail = 64;
  fx.publish();
  fx.acquire();
  state->runtime_update = true;
  const int updates_before_drain = state->updates;
  fx.settle_block(LiveTailHost{});
  REQUIRE(state->updates == updates_before_drain);
  REQUIRE(fx.tail_samples() == 64);
}

TEST_CASE("all PartFxStage SysEx paths walk uniform EFX blocks in wire order",
          "[gs-efx-realization][gs-efx-bulk]") {
  // A 40 30 run has one real EFX block at 00-1F, reserved bytes at 20-7F,
  // then the next unit at 40 31 00. Poisoning the reserved region catches a
  // decoder that treats every 32-byte slice as another unit or folds it back
  // into unit 0.
  std::vector<uint8_t> bulk_data(130, 0x00);
  bulk_data[0] = 0x01;
  bulk_data[1] = 0x10;  // unit 0: Overdrive
  bulk_data[0x20] = 0x01;
  bulk_data[0x21] = 0x50;  // reserved: must not become a second type write
  bulk_data[128] = 0x01;
  bulk_data[129] = 0x50;  // unit 1: Stereo Delay
  const std::vector<uint8_t> bulk = efx_bulk(0x403000, bulk_data);

  const auto check_units = [](const s::PartFxStage& fx) {
    REQUIRE(fx.efx()[0].type == 0x0110);
    REQUIRE(fx.efx()[1].type == 0x0150);
    REQUIRE(fx.efx()[0].assigned);
    REQUIRE(fx.efx()[1].assigned);
  };

  SECTION("audio/offline unit application") {
    s::PartFxStage fx;
    REQUIRE(fx.apply_unit_sysex(bulk.data(), bulk.size()));
    check_units(fx);
  }

  SECTION("control mirror") {
    s::PartFxStage fx;
    fx.mirror_sysex(bulk.data(), bulk.size());
    check_units(fx);
  }

  SECTION("control application") {
    s::PartFxStage fx;
    REQUIRE(fx.apply_control_sysex(bulk.data(), bulk.size()));
    check_units(fx);
  }

  // A run starting in the reserved tail of unit 0 must still enter unit 1 at
  // 40 31 00. This is the boundary a first-byte classifier cannot see.
  const std::vector<uint8_t> entering_next = efx_bulk(0x40307F, {0x01, 0x01, 0x10});
  for (const int path : {0, 1, 2}) {
    CAPTURE(path);
    s::PartFxStage fx;
    if (path == 0) {
      REQUIRE(fx.apply_unit_sysex(entering_next.data(), entering_next.size()));
    } else if (path == 1) {
      fx.mirror_sysex(entering_next.data(), entering_next.size());
    } else {
      REQUIRE(fx.apply_control_sysex(entering_next.data(), entering_next.size()));
    }
    REQUIRE(fx.efx()[0].type == 0);
    REQUIRE(fx.efx()[1].type == 0x0110);
  }

  // 40 03 and 40 30 are aliases for unit 0. A long valid DT1 run reaches the
  // alias after the spec block; the later bytes must win in every path.
  std::vector<uint8_t> alias_data(5762, 0x00);
  alias_data[0] = 0x01;
  alias_data[1] = 0x10;
  alias_data[5760] = 0x01;
  alias_data[5761] = 0x50;
  const std::vector<uint8_t> alias = efx_bulk(0x400300, alias_data);
  for (const int path : {0, 1, 2}) {
    CAPTURE(path);
    s::PartFxStage fx;
    if (path == 0) {
      REQUIRE(fx.apply_unit_sysex(alias.data(), alias.size()));
    } else if (path == 1) {
      fx.mirror_sysex(alias.data(), alias.size());
    } else {
      REQUIRE(fx.apply_control_sysex(alias.data(), alias.size()));
    }
    REQUIRE(fx.efx()[0].type == 0x0150);
  }
}

TEST_CASE("the modern Stereo-EQ realises at host rates below its corners", "[gs-efx-realization]") {
  // Its default high corner (10960.9 Hz) lies past Nyquist at 8 and 16 kHz.
  constexpr uint16_t kStereoEq = 0x0100;
  for (const double rate : {8000.0, 16000.0, 22050.0, 48000.0}) {
    INFO("rate " << rate);
    s::Sf2PlayerConfig cfg;
    cfg.insert_factory = [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    cfg.realize_efx_inline = true;
    s::Sf2Player player(cfg);
    player.prepare(rate, kBlock);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));

    const auto message = type_write(kStereoEq);
    std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
    REQUIRE(player.prepare_sysex(message.data(), message.size(), token));
    REQUIRE(token != nullptr);

    send(player, message);
    REQUIRE(player.gs_efx().type == kStereoEq);
    std::vector<float> l(kBlock, 0.25f);
    std::vector<float> r(kBlock, 0.25f);
    float* io[] = {l.data(), r.data()};
    player.process(io, 2, kBlock);
    REQUIRE(player.gs_efx().type == kStereoEq);
    for (int i = 0; i < kBlock; ++i) {
      REQUIRE(std::isfinite(l[static_cast<size_t>(i)]));
      REQUIRE(std::isfinite(r[static_cast<size_t>(i)]));
    }
  }
}

TEST_CASE("a stage the factory cannot build keeps its position", "[gs-efx-realization]") {
  const s::GsEfx efx = efx_holding(kStereoDelay);
  const std::vector<s::GsEfxStage> chain = s::gs_efx_insert_chain(efx);
  REQUIRE(chain.size() >= 2);
  REQUIRE(chain.front().name == "effects.delay.stereo");
  // The output level sits behind the null stage.
  size_t gain_at = chain.size();
  for (size_t i = 0; i < chain.size(); ++i) {
    if (chain[i].name == "utility.gain") gain_at = i;
  }
  REQUIRE(gain_at < chain.size());
  REQUIRE(gain_at > 0);

  auto counters = std::make_shared<Counters>();
  const auto factory = [counters](std::string_view name, std::string_view) {
    if (name == "effects.delay.stereo") return std::unique_ptr<sonare::rt::ProcessorBase>();
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(counters, std::string(name), false, {"levelDb", "balance"}));
  };

  SECTION("the realised unit has one stage per chain position") {
    const s::Sf2EfxUnitRt unit =
        s::sf2_build_efx_unit(efx, chain, s::GsEfxRealization::kModern, factory, kRate, kBlock);
    REQUIRE(unit.stages.size() == chain.size());
    REQUIRE(unit.stages.front().proc == nullptr);
    for (size_t i = 0; i < chain.size(); ++i) {
      REQUIRE(unit.stages[i].name == chain[i].name);
      REQUIRE(unit.stages[i].branch == chain[i].branch);
      REQUIRE(unit.stages[i].ordinal == chain[i].ordinal);
    }
    // Running over the null stage leaves the signal to the stages after it.
    std::vector<float> l(kBlock, 0.25f);
    std::vector<float> r(kBlock, 0.25f);
    s::sf2_run_efx_unit(unit, l.data(), r.data(), kBlock);
    REQUIRE(counters->probe.back() == 0.25f);
  }

  SECTION("a live edit reaches the stage behind it") {
    s::Sf2PlayerConfig cfg;
    cfg.insert_factory = factory;
    s::Sf2Player player(cfg);
    player.prepare(kRate, kBlock);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    send(player, type_write(kStereoDelay));
    const uint32_t generation = player.gs_efx_generation();
    counters->set_params.clear();
    send(player, slot_write(19, 0x20));  // Level (40 03 16)
    render_block(player);
    REQUIRE(player.gs_efx_generation() == generation);
    // The value the gain stage received is the one its own position carries.
    s::GsEfx edited = efx;
    edited.params[19] = 0x20;
    const std::string json = s::gs_efx_insert_chain(edited)[gain_at].params_json;
    const size_t at = json.find("\"levelDb\":");
    REQUIRE(at != std::string::npos);
    const float expected = std::stof(json.substr(at + 10));
    bool level_written = false;
    for (const auto& [key, value] : counters->set_params) {
      if (key != "utility.gain.levelDb") continue;
      level_written = true;
      REQUIRE(value == expected);
    }
    REQUIRE(level_written);
  }
}

TEST_CASE("a realization switch and a changed stage shape rebuild", "[gs-efx-realization]") {
  auto counters = std::make_shared<Counters>();
  s::Sf2PlayerConfig cfg;
  cfg.insert_factory = [counters](std::string_view name, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(counters, std::string(name), false, {"levelDb"}));
  };
  s::Sf2Player player(cfg);
  REQUIRE(player.gs_efx_realization() == s::GsEfxRealization::kModern);
  player.prepare(kRate, kBlock);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  send(player, type_write(kStereoDelay));

  SECTION("switching the realization rebuilds every unit") {
    const uint32_t generation = player.gs_efx_generation();
    player.set_gs_efx_realization(s::GsEfxRealization::kModern);  // no change
    REQUIRE(player.gs_efx_generation() == generation);
    player.set_gs_efx_realization(s::GsEfxRealization::kClassic);
    REQUIRE(player.gs_efx_generation() == generation + 1);
    player.set_gs_efx_realization(s::GsEfxRealization::kModern);
    REQUIRE(player.gs_efx_generation() == generation + 2);
  }

  SECTION("a stage list shaped other than the published one rebuilds") {
    // The same edit twice: in place over the rows the unit was built from, and
    // a rebuild once the rows bring an extra stage into the list.
    send(player, slot_write(19, 0x30));
    const uint32_t generation = player.gs_efx_generation();
    send(player, slot_write(19, 0x31));
    REQUIRE(player.gs_efx_generation() == generation);

    std::vector<s::GsEfxBindingRow> extended(s::kGsEfxBindingRows.begin(),
                                             s::kGsEfxBindingRows.end());
    s::GsEfxBindingRow extra{};
    bool found = false;
    for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
      if (row.type == kStereoDelay && s::kGsEfxRowStages[row.stage] == "utility.gain") {
        extra = row;
        found = true;
      }
    }
    REQUIRE(found);
    extra.ordinal = 1;  // a second gain stage, appended behind the first
    extended.push_back(extra);
    const s::GsEfxRowView rows{extended.data(), extended.size(), nullptr, 0};
    player.set_gs_efx_rows(&rows);
    send(player, slot_write(19, 0x32));
    REQUIRE(player.gs_efx_generation() == generation + 1);
    player.set_gs_efx_rows(nullptr);
  }
}

#if defined(SONARE_MIDI_WITH_FX)
namespace {

/// Program 0: a looped sine, so a routed note keeps the unit fed.
std::shared_ptr<s::Sf2File> sine_fixture() {
  constexpr double kTwoPi = 6.28318530717958647692;
  sonare::test::Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int id = b.add_sample("sine", sine, 32000, 60, 32, 96);
  sonare::test::Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = id;
  const int inst = b.add_instrument("sine", {looped});
  sonare::test::Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Sine", 0, 0, {pz});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<s::Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

/// Program 0: a deterministic one-shot impulse with a short zero tail. It
/// excites a prepared finite-delay stage, then leaves that stage unfed when
/// the prepared EFX assignment is removed.
std::shared_ptr<s::Sf2File> impulse_fixture() {
  sonare::test::Sf2Builder b;
  std::vector<float> impulse(64, 0.0f);
  impulse.front() = 1.0f;
  const int id = b.add_sample("impulse", impulse, 48000, 60, 0, impulse.size());
  sonare::test::Sf2Builder::ZoneSpec zone;
  zone.target = id;
  const int inst = b.add_instrument("impulse", {zone});
  sonare::test::Sf2Builder::ZoneSpec preset_zone;
  preset_zone.target = inst;
  b.add_preset("Impulse", 0, 0, {preset_zone});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<s::Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

float rms(const std::vector<float>& x, size_t from) {
  double acc = 0.0;
  for (size_t i = from; i < x.size(); ++i) acc += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(acc / static_cast<double>(x.size() - from)));
}

}  // namespace

TEST_CASE("prepared EFX node tails are included before the first process", "[gs-efx-realization]") {
  s::Sf2PlayerConfig classic_cfg;
  classic_cfg.synth_fallback = false;
  classic_cfg.gs_efx_realization = s::GsEfxRealization::kClassic;
  classic_cfg.effects.enable_reverb = false;
  classic_cfg.effects.enable_chorus = false;
  classic_cfg.effects.enable_delay = false;
  classic_cfg.insert_factory = [](std::string_view, std::string_view) {
    // Keep PartFxStage enabled so the prepared assignment is a real route;
    // Classic units own their DSP and do not need a modern child factory.
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  s::Sf2Player classic(classic_cfg);
  classic.prepare(kRate, kBlock);
  const int classic_before = classic.tail_samples();

  const auto classic_type_payload = type_write(kOverdrive);
  const auto assign = part_efx_assign(1);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> classic_type;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> classic_assign;
  REQUIRE(classic.prepare_sysex(classic_type_payload.data(), classic_type_payload.size(),
                                classic_type));
  REQUIRE(classic.prepare_sysex(assign.data(), assign.size(), classic_assign));
  const auto dispatch = [](s::Sf2Player& player, const uint8_t* data, size_t size,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    sonare::midi::MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = data;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  dispatch(classic, classic_type_payload.data(), classic_type_payload.size(), classic_type);
  dispatch(classic, assign.data(), assign.size(), classic_assign);
  CHECK(classic.tail_samples() - classic_before >= static_cast<int>(10.0 * kRate));

  const auto state = efx_holding(kOverdrive);
  const auto chain = s::gs_efx_insert_chain(state);
  constexpr int kDeclaredTail = 173;
  std::vector<int> tails(chain.size(), 0);
  REQUIRE_FALSE(tails.empty());
  tails.front() = kDeclaredTail;
  const auto plan = make_tail_factory_plan(tails);
  s::Sf2PlayerConfig modern_cfg;
  modern_cfg.synth_fallback = false;
  modern_cfg.effects.enable_reverb = false;
  modern_cfg.effects.enable_chorus = false;
  modern_cfg.effects.enable_delay = false;
  modern_cfg.insert_factory = prepared_tail_factory(plan);
  s::Sf2Player modern(modern_cfg);
  modern.prepare(kRate, kBlock);
  const int modern_before = modern.tail_samples();
  const auto modern_type_payload = type_write(kOverdrive);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> modern_type;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> modern_assign;
  REQUIRE(
      modern.prepare_sysex(modern_type_payload.data(), modern_type_payload.size(), modern_type));
  REQUIRE(modern.prepare_sysex(assign.data(), assign.size(), modern_assign));
  dispatch(modern, modern_type_payload.data(), modern_type_payload.size(), modern_type);
  dispatch(modern, assign.data(), assign.size(), modern_assign);
  REQUIRE(modern.tail_samples() - modern_before >= kDeclaredTail);
}

TEST_CASE("speculative prepared EFX work does not raise an unrouted tail", "[gs-efx-realization]") {
  s::Sf2PlayerConfig cfg;
  cfg.synth_fallback = false;
  cfg.gs_efx_realization = s::GsEfxRealization::kClassic;
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  cfg.insert_factory = [](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  s::Sf2Player player(cfg);
  player.prepare(kRate, kBlock);
  const int before = player.tail_samples();
  const auto type = type_write(kOverdrive);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
  REQUIRE(player.prepare_sysex(type.data(), type.size(), token));
  CHECK(player.tail_samples() == before);

  sonare::midi::MidiEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = type.data();
  event.sysex_payload_size = type.size();
  event.prepared_sysex = token.get();
  player.on_event(0, event);
  // Selecting a node without routing a part into it cannot contribute a tail.
  CHECK(player.tail_samples() == before);
}

TEST_CASE("prepared EFX metadata adds a selected unit after its host rig", "[gs-efx-realization]") {
  constexpr int kHostTail = 400;
  constexpr int kLongHostTail = 900;
  constexpr int kUnitTail = 173;
  const auto state = efx_holding(kOverdrive);
  const auto unit_chain = s::gs_efx_insert_chain(state);
  REQUIRE_FALSE(unit_chain.empty());
  const std::string unit_head = unit_chain.front().name;
  auto counters = std::make_shared<Counters>();

  s::Sf2PlayerConfig cfg;
  cfg.synth_fallback = false;
  cfg.bank_rig_binding = false;
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  for (sonare::midi::PartRig& rig : cfg.part_rigs) rig.mode = sonare::midi::PartRigMode::kNone;
  cfg.part_rigs[0].mode = sonare::midi::PartRigMode::kChain;
  cfg.part_rigs[0].stages = {{"test.host", "{}"}};
  cfg.insert_factory = [counters, unit_head](std::string_view name, std::string_view) {
    std::vector<std::string> keys;
    if (name != "test.host") {
      keys.reserve(s::kGsEfxRowKeys.size());
      for (const std::string_view key : s::kGsEfxRowKeys) keys.emplace_back(key);
    }
    const int tail = name == "test.host"        ? kHostTail
                     : name == "test.host.long" ? kLongHostTail
                     : name == unit_head        ? kUnitTail
                                                : 0;
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new StandIn(counters, std::string(name), false, std::move(keys), tail));
  };
  s::Sf2Player player(cfg);
  player.prepare(kRate, kBlock);
  const int before = player.tail_samples();

  const auto type = type_write(kOverdrive);
  const auto assign = part_efx_assign(1);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  REQUIRE(player.prepare_sysex(type.data(), type.size(), type_token));
  REQUIRE(player.prepare_sysex(assign.data(), assign.size(), assign_token));
  const auto dispatch = [](s::Sf2Player& target, const auto& payload,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    sonare::midi::MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    event.prepared_sysex = token.get();
    target.on_event(0, event);
  };
  dispatch(player, type, type_token);
  dispatch(player, assign, assign_token);
  // The prepared unit is serial after the host chain, so its tail is additive.
  const int after = player.tail_samples();
  CAPTURE(before, after, kHostTail, kUnitTail);
  REQUIRE(after - before == kUnitTail);

  // Fill the publication ring with the original host-chain shape, then leave
  // a longer chain as the newest snapshot. A quiescent reset must adopt that
  // newest snapshot before its first prepared event; otherwise a plain acquire
  // can stop at the full retire ring and retain a stale shorter topology.
  const sonare::midi::PartRig host_rig = cfg.part_rigs[0];
  sonare::midi::PartRig longer_host_rig = host_rig;
  longer_host_rig.stages = {{"test.host.long", "{}"}};
  for (int publication = 0; publication < 64; ++publication) {
    REQUIRE(player.set_part_rig(0, host_rig));
  }
  REQUIRE(player.set_part_rig(0, longer_host_rig));

  // reset() republishes the host chain at another quiescent boundary. The
  // prepared metadata must be available before that boundary's first block.
  player.reset();
  const int reset_before = player.tail_samples();
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> reset_type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> reset_assign_token;
  REQUIRE(player.prepare_sysex(type.data(), type.size(), reset_type_token));
  REQUIRE(player.prepare_sysex(assign.data(), assign.size(), reset_assign_token));
  dispatch(player, type, reset_type_token);
  dispatch(player, assign, reset_assign_token);
  const int reset_after = player.tail_samples();
  CAPTURE(reset_before, reset_after, kLongHostTail, kUnitTail);
  REQUIRE(reset_after - reset_before == kUnitTail);
}

TEST_CASE("prepared EFX assignment drains a finite old unit before reactivation",
          "[gs-efx-realization]") {
  constexpr int kTail = 2 * kBlock;
  const auto state = efx_holding(kOverdrive);
  const auto chain = s::gs_efx_insert_chain(state);
  REQUIRE_FALSE(chain.empty());
  std::vector<int> tails(chain.size(), 0);
  tails.front() = kTail;
  const auto plan = make_tail_factory_plan(tails);
  auto counters = plan->counters;

  s::Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = false;
  cfg.dc_block = false;
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  cfg.insert_factory = [plan](std::string_view, std::string_view) {
    const size_t index = plan->next++;
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new FiniteTailDelay(plan->counters, plan->tails.at(index)));
  };
  s::Sf2Player player(cfg);
  player.set_soundfont(impulse_fixture());
  player.prepare(kRate, kBlock);

  const auto type = type_write(kOverdrive);
  const auto assign = part_efx_assign(1);
  const auto unassign = part_efx_assign(0);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> unassign_token;
  REQUIRE(player.prepare_sysex(type.data(), type.size(), type_token));
  REQUIRE(player.prepare_sysex(assign.data(), assign.size(), assign_token));
  REQUIRE(player.prepare_sysex(unassign.data(), unassign.size(), unassign_token));
  const auto dispatch = [&](const auto& payload,
                            const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    sonare::midi::MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  dispatch(type, type_token);
  dispatch(assign, assign_token);

  player.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  render_block(player);
  const int after_excitation = counters->processes;
  REQUIRE(after_excitation > 0);

  dispatch(unassign, unassign_token);
  std::vector<float> first_drain;
  render_block(player, &first_drain);
  const int after_first_drain = counters->processes;
  REQUIRE(after_first_drain > after_excitation);

  std::vector<float> second_drain;
  render_block(player, &second_drain);
  const int after_second_drain = counters->processes;
  REQUIRE(after_second_drain > after_first_drain);
  const auto max_abs = [](const std::vector<float>& samples) {
    float out = 0.0f;
    for (const float sample : samples) out = std::max(out, std::abs(sample));
    return out;
  };
  REQUIRE(max_abs(first_drain) + max_abs(second_drain) > 1e-5f);

  render_block(player);
  REQUIRE(counters->processes == after_second_drain);

  dispatch(assign, assign_token);
  std::vector<float> reactivated;
  render_block(player, &reactivated);
  REQUIRE(max_abs(reactivated) < 1e-8f);
}

TEST_CASE("a classic unit is the type's graph and takes byte edits live", "[gs-efx-realization]") {
  const auto& registry = s::gs_classic::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const s::gs_classic::GsClassicType* model = registry.find(kOverdrive);
  REQUIRE(model != nullptr);

  SECTION("the unit is one classic stage holding the wire bytes") {
    s::GsEfx efx = efx_holding(kOverdrive);
    efx.params[3] = 0x21;
    int factory_calls = 0;
    const auto factory = [&factory_calls](std::string_view, std::string_view) {
      ++factory_calls;
      return std::unique_ptr<sonare::rt::ProcessorBase>();
    };
    const s::Sf2EfxUnitRt unit =
        s::sf2_build_efx_unit(efx, {}, s::GsEfxRealization::kClassic, factory, kRate, kBlock);
    REQUIRE(factory_calls == 0);
    REQUIRE(unit.realization == s::GsEfxRealization::kClassic);
    REQUIRE(unit.stages.size() == 1);
    const auto* classic =
        dynamic_cast<const s::gs_classic::GsClassicUnit*>(unit.stages[0].proc.get());
    REQUIRE(classic != nullptr);
    for (size_t slot = 0; slot < efx.params.size(); ++slot) {
      REQUIRE(classic->byte(slot) == efx.params[slot]);
    }
  }

  SECTION("Thru realises nothing in either realization") {
    const s::GsEfx thru = efx_holding(0x0000);
    for (const auto realization : {s::GsEfxRealization::kModern, s::GsEfxRealization::kClassic}) {
      const s::Sf2EfxUnitRt unit = s::sf2_build_efx_unit(thru, s::gs_efx_insert_chain(thru),
                                                         realization, nullptr, kRate, kBlock);
      REQUIRE(unit.stages.empty());
    }
  }

  SECTION("a level edit silences the running unit without a rebuild") {
    int factory_calls = 0;
    s::Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.dc_block = false;
    cfg.gs_efx_realization = s::GsEfxRealization::kClassic;
    cfg.insert_factory = [&factory_calls](std::string_view, std::string_view) {
      ++factory_calls;
      return std::unique_ptr<sonare::rt::ProcessorBase>();
    };
    s::Sf2Player player(cfg);
    player.set_soundfont(sine_fixture());
    player.prepare(kRate, kBlock);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    send(player, type_write(kOverdrive));
    send(player, efx_write(0x17, 0x00));  // no reverb send, so no tail outlives the unit
    // The output level (40 03 16), the classic graph's last gain.
    constexpr uint8_t kLevelSlot = 19;
    REQUIRE(model->printed_lo[kLevelSlot] < model->printed_hi[kLevelSlot]);
    send(player, slot_write(kLevelSlot, model->printed_hi[kLevelSlot]));
    player.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    const uint32_t generation = player.gs_efx_generation();

    std::vector<float> before;
    for (int b = 0; b < 40; ++b) render_block(player, &before);
    send(player, slot_write(kLevelSlot, model->printed_lo[kLevelSlot]));
    REQUIRE(player.gs_efx_generation() == generation);
    std::vector<float> after;
    for (int b = 0; b < 40; ++b) render_block(player, &after);
    REQUIRE(player.gs_efx_generation() == generation);
    REQUIRE(factory_calls == 0);

    const float loud = rms(before, before.size() / 2);
    const float quiet = rms(after, after.size() / 2);
    INFO("loud " << loud << " quiet " << quiet);
    REQUIRE(loud > 1e-3f);
    REQUIRE(quiet < loud * 1e-2f);
  }

  SECTION("a long direct edit burst keeps its final raw and realtime values") {
    auto counters = std::make_shared<Counters>();
    s::Sf2PlayerConfig cfg;
    cfg.insert_factory = [counters](std::string_view name, std::string_view) {
      std::vector<std::string> keys;
      keys.reserve(s::kGsEfxRowKeys.size());
      for (const std::string_view key : s::kGsEfxRowKeys) keys.emplace_back(key);
      return std::unique_ptr<sonare::rt::ProcessorBase>(
          new StandIn(counters, std::string(name), true, std::move(keys)));
    };
    s::Sf2Player player(cfg);
    player.prepare(kRate, kBlock);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    send(player, type_write(kOverdrive));
    const uint8_t slot = switchable_slot(kOverdrive);
    const uint32_t generation = player.gs_efx_generation();
    // No block renders, so every accepted edit stays queued in publication
    // order and none of them forces a rebuild of the snapshot.
    constexpr uint8_t kFinal = 0x00;
    for (uint32_t edit = 1; edit <= 1000; ++edit) {
      send(player, slot_write(slot, (edit == 1000 || (edit & 1u) != 0) ? kFinal : 0x7F));
    }
    REQUIRE(player.gs_efx_generation() == generation);
    REQUIRE(player.gs_efx(0).params[slot] == kFinal);

    const s::GsEfxBindingRow* final_row = nullptr;
    for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
      if (row.type == kOverdrive && row.slot == slot) {
        final_row = &row;
        break;
      }
    }
    REQUIRE(final_row != nullptr);
    const float expected = s::gs_efx_binding_value(*final_row, kFinal);
    counters->log_set_params = false;
    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    float* channels[2] = {left.data(), right.data()};
    sonare::test::AllocationGuard guard;
    player.process(channels, 2, kBlock);
    REQUIRE(guard.count() == 0);
    REQUIRE(player.gs_efx_generation() == generation);
    REQUIRE(counters->parameter_sets > 0);
    REQUIRE(counters->has_parameter_by_id[final_row->key]);
    REQUIRE(std::abs(counters->last_parameter_by_id[final_row->key] - expected) < 1e-6f);
  }
}

TEST_CASE("a classic direct parameter survives another unit's legacy fallback",
          "[gs-efx-realization]") {
  const auto& registry = s::gs_classic::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const s::gs_classic::GsClassicType* model = registry.find(kOverdrive);
  REQUIRE(model != nullptr);
  constexpr uint8_t kLevelSlot = 19;
  REQUIRE(model->printed_lo[kLevelSlot] < model->printed_hi[kLevelSlot]);

  auto custom_calls = std::make_shared<int>(0);
  auto custom_counters = std::make_shared<Counters>();
  s::Sf2PlayerConfig fallback_config;
  fallback_config.gain = 1.0f;
  fallback_config.dc_block = false;
  fallback_config.insert_factory = [custom_calls, custom_counters](std::string_view name,
                                                                   std::string_view) {
    if (name == "saturation.ampSim") {
      ++*custom_calls;
      // The modern amp-sim graph deliberately lacks the complete GS metadata,
      // which makes unit 1 take the direct legacy snapshot path.
      return std::unique_ptr<sonare::rt::ProcessorBase>(
          new StandIn(custom_counters, std::string(name), false, {"levelDb"}));
    }
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  s::Sf2Player player(fallback_config);
  player.set_soundfont(sine_fixture());
  player.prepare(kRate, kBlock);
  send(player, part_efx_assign(2));
  send(player, type_write_at(0x31, kOverdrive));
  REQUIRE(*custom_calls > 0);

  // Leave unit 1's fallback marker alive, then switch the active part to the
  // classic spec unit. Its following direct parameter edit must be committed
  // through the classic byte queue rather than silently returning before the
  // batch publication.
  player.set_gs_efx_realization(s::GsEfxRealization::kClassic);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  send(player, type_write(kOverdrive));
  send(player, efx_write(0x17, 0x00));
  send(player, slot_write(kLevelSlot, model->printed_hi[kLevelSlot]));
  player.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  std::vector<float> loud_signal;
  for (int b = 0; b < 40; ++b) render_block(player, &loud_signal);
  const uint32_t generation = player.gs_efx_generation();
  send(player, slot_write(kLevelSlot, model->printed_lo[kLevelSlot]));
  REQUIRE(player.gs_efx_generation() == generation);
  std::vector<float> quiet_signal;
  for (int b = 0; b < 40; ++b) render_block(player, &quiet_signal);

  s::Sf2PlayerConfig reference_config;
  reference_config.gain = 1.0f;
  reference_config.dc_block = false;
  reference_config.gs_efx_realization = s::GsEfxRealization::kClassic;
  reference_config.insert_factory = [](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  s::Sf2Player reference(reference_config);
  reference.set_soundfont(sine_fixture());
  reference.prepare(kRate, kBlock);
  reference.on_control_sysex(kPartOn, sizeof(kPartOn));
  send(reference, type_write(kOverdrive));
  send(reference, efx_write(0x17, 0x00));
  send(reference, slot_write(kLevelSlot, model->printed_hi[kLevelSlot]));
  reference.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  std::vector<float> reference_loud;
  for (int b = 0; b < 40; ++b) render_block(reference, &reference_loud);
  send(reference, slot_write(kLevelSlot, model->printed_lo[kLevelSlot]));
  std::vector<float> reference_quiet;
  for (int b = 0; b < 40; ++b) render_block(reference, &reference_quiet);

  const float loud = rms(loud_signal, loud_signal.size() / 2);
  const float quiet = rms(quiet_signal, quiet_signal.size() / 2);
  const float reference_quiet_rms = rms(reference_quiet, reference_quiet.size() / 2);
  INFO("loud " << loud << " quiet " << quiet << " reference " << reference_quiet_rms);
  REQUIRE(loud > 1e-3f);
  REQUIRE(quiet < loud * 1e-2f);
  REQUIRE(std::abs(quiet - reference_quiet_rms) < 1e-5f);
}
#endif  // SONARE_MIDI_WITH_FX
