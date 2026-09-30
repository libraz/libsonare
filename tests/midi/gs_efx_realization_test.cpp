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

#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
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
constexpr uint16_t kOverdrive = 0x0110;

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
  std::vector<std::pair<std::string, float>> set_params;
  std::vector<float> probe;  ///< Left channel of the probe's last block.
};

/// A stand-in stage: writes a constant 1 over its input (so what reaches the
/// probe after it is its fade) or passes through recording what it saw.
class StandIn final : public sonare::rt::ProcessorBase {
 public:
  StandIn(std::shared_ptr<Counters> counters, std::string name, bool writer,
          std::vector<std::string> keys)
      : counters_(std::move(counters)),
        name_(std::move(name)),
        writer_(writer),
        keys_(std::move(keys)) {}
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
  bool set_parameter_impl(unsigned int id, float value) override {
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

float rms(const std::vector<float>& x, size_t from) {
  double acc = 0.0;
  for (size_t i = from; i < x.size(); ++i) acc += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(acc / static_cast<double>(x.size() - from)));
}

}  // namespace

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

  SECTION("a byte edit the queue cannot hold rebuilds") {
    s::Sf2PlayerConfig cfg;
    cfg.gs_efx_realization = s::GsEfxRealization::kClassic;
    cfg.insert_factory = [](std::string_view, std::string_view) {
      return std::unique_ptr<sonare::rt::ProcessorBase>();
    };
    s::Sf2Player player(cfg);
    player.prepare(kRate, kBlock);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    send(player, type_write(kOverdrive));
    const uint8_t slot = switchable_slot(kOverdrive);
    const uint32_t generation = player.gs_efx_generation();
    // No block renders, so nothing drains: each edit holds one queue entry until
    // the ring is full, and the edit that finds it full is rebuilt instead.
    uint32_t rebuilt_at = 0;
    for (uint32_t edit = 1; edit <= 1000 && rebuilt_at == 0; ++edit) {
      send(player, slot_write(slot, (edit & 1u) != 0 ? 0x7F : 0x00));
      if (player.gs_efx_generation() != generation) rebuilt_at = edit;
    }
    REQUIRE(rebuilt_at > 1);
    REQUIRE(player.gs_efx_generation() == generation + 1);
    REQUIRE(player.gs_efx(0).params[slot] == ((rebuilt_at & 1u) != 0 ? 0x7F : 0x00));
  }
}
#endif  // SONARE_MIDI_WITH_FX
