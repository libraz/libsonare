/// @file gs_efx_control_test.cpp
/// @brief EFX CONTROL SOURCE/DEPTH (40 03 1B-1E): a controller moves the type's
///        `+` / `#` slot per block without a rebuild, the depth's sign sets the
///        direction, and a MIDI 2.0 value reads at full width.
///
/// The printed marks reach the player through the binding rows. Until the
/// generated tables carry them, the marks of the four types used here are set
/// by hand on copies of the generated rows (01 00 `+` 19; 01 22 `+` 10, `#` 19;
/// 01 23 `+` 3, `#` 5; 01 26 `+` 1) and handed over the GsEfxRowView seam. 01 22
/// slot 10 (Speed, 00/7F) has no generated row, so it is added as a two-state
/// row and exercised under the classic realization only, which reads the wire
/// byte rather than the row's stage and key.

#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/control_value.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"

namespace {

namespace s = sonare::midi::synth;
namespace m = sonare::midi;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kListenBlocks = 24;

constexpr uint16_t kLofi1 = 0x0100;  // `+` 19 (Level); no `#`
constexpr uint16_t kRotary = 0x0122;
constexpr uint16_t kStepFlanger = 0x0123;
constexpr uint16_t kAutoPan = 0x0126;

/// A framed GS DT1 write of @p value at 40 03 @p offset.
std::array<uint8_t, 11> efx_write(uint8_t offset, uint8_t value) {
  std::array<uint8_t, 11> msg = {0xF0, 0x41,   0x10,  0x42, 0x12, 0x40,
                                 0x03, offset, value, 0x00, 0xF7};
  const uint32_t sum = msg[5] + msg[6] + msg[7] + msg[8];
  msg[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return msg;
}

/// A framed EFX type selection.
std::array<uint8_t, 12> type_write(uint16_t type) {
  const auto msb = static_cast<uint8_t>(type >> 8);
  const auto lsb = static_cast<uint8_t>(type & 0x7F);
  std::array<uint8_t, 12> msg = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                 0x03, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = msg[5] + msg[6] + msg[7] + msg[8] + msg[9];
  msg[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return msg;
}

/// Part 1 (channel 0) routed into the spec unit.
constexpr std::array<uint8_t, 11> kPartOn = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                             0x41, 0x22, 0x01, 0x5C, 0xF7};

/// EFX PARAMETER @p slot + 1 sits at 40 03 (03 + slot); CONTROL n's source and
/// depth at 40 03 1B/1C and 1D/1E.
uint8_t slot_offset(uint8_t slot) { return static_cast<uint8_t>(0x03 + slot); }
uint8_t source_offset(size_t control) { return static_cast<uint8_t>(0x1B + 2 * control); }
uint8_t depth_offset(size_t control) { return static_cast<uint8_t>(0x1C + 2 * control); }

/// @name CONTROL SOURCE and DEPTH bytes
/// @{
constexpr uint8_t kSourceCc1 = 0x01;
constexpr uint8_t kSourceCc95 = 0x5F;
constexpr uint8_t kSourceAftertouch = 0x60;
constexpr uint8_t kSourceBend = 0x61;
constexpr uint8_t kDepthPlus = 0x7F;
constexpr uint8_t kDepthZero = 0x40;
constexpr uint8_t kDepthMinus = 0x00;
/// @}

/// The base byte every modulated slot starts from, so both directions have room.
constexpr uint8_t kBase = 0x40;

/// The hand-marked rows, built once and kept for the life of the test binary
/// (a player holds the view by pointer).
const std::vector<s::GsEfxBindingRow>& marked_rows() {
  static const std::vector<s::GsEfxBindingRow> rows = [] {
    std::vector<s::GsEfxBindingRow> out(s::kGsEfxBindingRows.begin(), s::kGsEfxBindingRows.end());
    const struct {
      uint16_t type;
      uint8_t slot;
      uint8_t mark;
    } marks[] = {{kLofi1, 19, '+'},
                 {kRotary, 19, '#'},
                 {kStepFlanger, 3, '+'},
                 {kStepFlanger, 5, '#'},
                 {kAutoPan, 1, '+'}};
    for (const auto& mark : marks) {
      bool found = false;
      for (s::GsEfxBindingRow& row : out) {
        if (row.type != mark.type || row.slot != mark.slot) continue;
        row.printed_mark = mark.mark;
        found = true;
      }
      REQUIRE(found);
    }
    // Rotary Speed, printed 00/7F: two states at the two ends of the byte.
    s::GsEfxBindingRow speed{};
    speed.type = kRotary;
    speed.slot = 10;
    speed.kind = s::kGsEfxRowDesigned;
    speed.law = {s::kGsEfxFormEnum, 0.0f, 1.0f, 2};
    speed.byte_lo = 0x00;
    speed.byte_hi = 0x7F;
    speed.out = s::GsEfxOut::kValue;
    for (const s::GsEfxBindingRow& row : out) {
      if (row.type == kRotary) {
        speed.stage = row.stage;
        speed.key = row.key;
        break;
      }
    }
    speed.printed_mark = '+';
    out.push_back(speed);
    return out;
  }();
  return rows;
}

const s::GsEfxRowView& marked_view() {
  static const s::GsEfxRowView view{marked_rows().data(), marked_rows().size(),
                                    s::kGsEfxEnables.data(), s::kGsEfxEnables.size()};
  return view;
}

/// The marked row of @p type carrying @p mark, or null where the type has none.
const s::GsEfxBindingRow* find_marked(uint16_t type, uint8_t mark) {
  for (const s::GsEfxBindingRow& row : marked_rows()) {
    if (row.type == type && row.printed_mark == mark) return &row;
  }
  return nullptr;
}

/// The marked row of @p type carrying @p mark.
const s::GsEfxBindingRow& marked_row(uint16_t type, uint8_t mark) {
  const s::GsEfxBindingRow* row = find_marked(type, mark);
  if (row == nullptr) FAIL("no row marks " << static_cast<char>(mark) << " on type " << type);
  return *row;
}

/// Every set_parameter a recording stage took, in order.
struct Log {
  struct Entry {
    std::string stage;
    std::string key;
    float value;
  };
  std::vector<Entry> entries;

  /// How many writes (stage, key) took.
  size_t count(std::string_view stage, std::string_view key) const {
    size_t n = 0;
    for (const Entry& e : entries) n += (e.stage == stage && e.key == key) ? 1 : 0;
    return n;
  }
  /// The last value written to (stage, key).
  float last(std::string_view stage, std::string_view key) const {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if (it->stage == stage && it->key == key) return it->value;
    }
    FAIL("nothing wrote " << stage << "." << key);
    return 0.0f;
  }
};

/// A pass-through stage publishing every row key as a realtime-safe control and
/// logging what it is set to.
class Recorder final : public sonare::rt::ProcessorBase {
 public:
  Recorder(std::shared_ptr<Log> log, std::string stage)
      : log_(std::move(log)), stage_(std::move(stage)) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  bool set_parameter_impl(unsigned int id, float value) override {
    log_->entries.push_back({stage_, std::string(s::kGsEfxRowKeys.at(id)), value});
    return true;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (size_t i = 0; i < s::kGsEfxRowKeys.size(); ++i) {
      out.push_back({std::string(s::kGsEfxRowKeys[i]), static_cast<unsigned int>(i)});
    }
    return out;
  }

 private:
  std::shared_ptr<Log> log_;
  std::string stage_;
};

std::shared_ptr<s::Sf2File> sine_fixture() {
  static const std::shared_ptr<s::Sf2File> cached = [] {
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
  }();
  return cached;
}

/// One configuration of the unit and of how the player is driven.
struct Setup {
  uint16_t type = kStepFlanger;
  bool classic = false;
  bool live = false;  ///< Control-thread SysEx and published snapshots, else inline.
  size_t control = 0;
  uint8_t source = kSourceCc1;
  uint8_t depth = kDepthPlus;
};

/// A player running @p setup's unit over a sustained sine note on part 1.
class Rig {
 public:
  explicit Rig(const Setup& setup) : setup_(setup), log_(std::make_shared<Log>()) {
    s::Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.dc_block = false;
    cfg.realize_efx_inline = !setup.live;
    cfg.gs_efx_realization =
        setup.classic ? s::GsEfxRealization::kClassic : s::GsEfxRealization::kModern;
    std::shared_ptr<Log> log = log_;
    cfg.insert_factory = [log](std::string_view name, std::string_view) {
      return std::unique_ptr<sonare::rt::ProcessorBase>(
          std::make_unique<Recorder>(log, std::string(name)));
    };
    player_ = std::make_unique<s::Sf2Player>(cfg);
    player_->set_gs_efx_rows(&marked_view());
    player_->set_soundfont(sine_fixture());
    player_->prepare(kRate, kBlock);
    sysex(kPartOn);
    sysex(type_write(setup.type));
    sysex(efx_write(0x17, 0x00));  // no reverb send, so only the unit is heard
    // A CONTROL whose type marks no slot has no base to set.
    if (const s::GsEfxBindingRow* row = find_marked(setup.type, mark())) {
      REQUIRE(s::gs_efx_parameter_takes(setup.type, row->slot, kBase));
      sysex(efx_write(slot_offset(row->slot), kBase));
    }
    sysex(efx_write(source_offset(setup.control), setup.source));
    sysex(efx_write(depth_offset(setup.control), setup.depth));
    rest();
    player_->on_event(0, sonare::test::event(m::make_midi1_note_on(0, 0, 60, 100)));
    render(2);
  }

  uint8_t mark() const { return setup_.control == 0 ? '+' : '#'; }

  template <size_t N>
  void sysex(const std::array<uint8_t, N>& msg) {
    if (setup_.live) {
      player_->on_control_sysex(msg.data(), msg.size());
    } else {
      REQUIRE(player_->handle_sysex(msg.data(), msg.size()));
    }
  }

  /// Puts the source at 7-bit position @p v7 (the bend at v7 << 7, so 64 is its
  /// centre), sent as MIDI 1.0 or widened to MIDI 2.0 through from7 / from14.
  void move_source(uint8_t v7, bool midi2) {
    const auto v14 = static_cast<uint16_t>(v7 << 7);
    const uint32_t wide = m::Control32::from7(v7).raw;
    switch (setup_.source) {
      case kSourceAftertouch:
        send(midi2 ? m::make_midi2_channel_pressure(0, 0, wide)
                   : m::make_midi1_channel_pressure(0, 0, v7));
        return;
      case kSourceBend:
        send(midi2 ? m::make_midi2_pitch_bend(0, 0, m::Bend32::from14(v14).raw)
                   : m::make_midi1_pitch_bend(0, 0, v14));
        return;
      default:
        send(midi2 ? m::make_midi2_control_change(0, 0, setup_.source, wide)
                   : m::make_midi1_control_change(0, 0, setup_.source, v7));
        return;
    }
  }

  /// The source at rest: the bend centred, anything else at 0.
  void rest() { move_source(setup_.source == kSourceBend ? 64 : 0, false); }

  /// The source at a full-width position (a raw 32-bit CC / pressure / bend).
  void move_source_raw(uint32_t raw) {
    if (setup_.source == kSourceAftertouch) {
      send(m::make_midi2_channel_pressure(0, 0, raw));
    } else if (setup_.source == kSourceBend) {
      send(m::make_midi2_pitch_bend(0, 0, raw));
    } else {
      send(m::make_midi2_control_change(0, 0, setup_.source, raw));
    }
  }

  void send(const m::Ump& ump) { player_->on_event(0, sonare::test::event(ump)); }

  /// Renders @p blocks blocks, appending the left channel to @p out.
  void render(int blocks, std::vector<float>* out = nullptr) {
    std::vector<float> l(kBlock);
    std::vector<float> r(kBlock);
    for (int b = 0; b < blocks; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      float* chans[2] = {l.data(), r.data()};
      player_->process(chans, 2, kBlock);
      if (out != nullptr) out->insert(out->end(), l.begin(), l.end());
    }
  }

  int byte() const { return player_->gs_efx_control_byte(setup_.control); }
  uint32_t generation() const { return player_->gs_efx_generation(); }
  s::Sf2Player& player() { return *player_; }
  const Log& log() const { return *log_; }

 private:
  Setup setup_;
  std::shared_ptr<Log> log_;
  std::unique_ptr<s::Sf2Player> player_;
};

double max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double out = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    out = std::max(out, std::abs(static_cast<double>(a[i]) - b[i]));
  }
  return out;
}

/// The 7-bit position each pairwise row moves its source to: off rest for every
/// source and inside the range for both depth signs.
constexpr uint8_t kStimulus = 40;

}  // namespace

// Pairwise rows over six parameters (coverwise, strength 2, seed 0, 92/92 pairs,
// 12 rows): source {cc1, cc95, caf, bend} x depth {+100, 0, -100} x realization
// {modern, classic} x mark {+, #} x path {inline, live} x transport {midi1, midi2}.
// The type is Step Flanger (01 23), whose `+` is Rate (slot 3) and `#` Feedback
// (slot 5), both started at 40. Each row moves the source from rest to 40 (the
// bend to 40 << 7, below its centre, so it reads negative) and checks the
// direction against the depth's sign times the source's.
TEST_CASE("an EFX CONTROL moves its slot by the depth's sign: pairwise table",
          "[midi][synth][gs][gs-efx-control]") {
  enum class Transport { kMidi1, kMidi2 };
  struct Row {
    uint8_t source;
    uint8_t depth;
    bool classic;
    size_t control;
    bool live;
    Transport transport;
  };
  constexpr bool kModern = false;
  constexpr bool kClassic = true;
  constexpr bool kInline = false;
  constexpr bool kLive = true;
  const std::array<Row, 12> rows = {{
      {kSourceCc1, kDepthZero, kClassic, 1, kLive, Transport::kMidi1},
      {kSourceCc1, kDepthMinus, kModern, 0, kInline, Transport::kMidi2},
      {kSourceAftertouch, kDepthPlus, kModern, 0, kLive, Transport::kMidi1},
      {kSourceCc95, kDepthPlus, kClassic, 1, kInline, Transport::kMidi1},
      {kSourceBend, kDepthZero, kModern, 1, kInline, Transport::kMidi2},
      {kSourceAftertouch, kDepthMinus, kModern, 1, kInline, Transport::kMidi2},
      {kSourceCc95, kDepthMinus, kClassic, 0, kLive, Transport::kMidi2},
      {kSourceCc95, kDepthZero, kModern, 1, kInline, Transport::kMidi2},
      {kSourceBend, kDepthMinus, kClassic, 0, kLive, Transport::kMidi1},
      {kSourceAftertouch, kDepthZero, kClassic, 0, kInline, Transport::kMidi2},
      {kSourceBend, kDepthPlus, kModern, 0, kLive, Transport::kMidi2},
      {kSourceCc1, kDepthPlus, kModern, 1, kInline, Transport::kMidi1},
  }};
  REQUIRE(s::gs_classic::gs_classic_default_registry().find(kStepFlanger) != nullptr);

  for (const Row& row : rows) {
    Setup setup;
    setup.classic = row.classic;
    setup.live = row.live;
    setup.control = row.control;
    setup.source = row.source;
    setup.depth = row.depth;
    const bool midi2 = row.transport == Transport::kMidi2;
    INFO("source " << int(row.source) << " depth " << int(row.depth) << " classic " << row.classic
                   << " control " << row.control << " live " << row.live << " midi2 " << midi2);

    Rig rig(setup);
    REQUIRE(rig.byte() == kBase);
    const uint32_t generation = rig.generation();
    const s::GsEfxBindingRow& dest = marked_row(kStepFlanger, rig.mark());
    const std::string stage(s::kGsEfxRowStages[dest.stage]);
    const std::string key(s::kGsEfxRowKeys[dest.key]);
    const size_t writes_before = rig.log().count(stage, key);

    rig.move_source(kStimulus, midi2);
    std::vector<float> heard;
    rig.render(kListenBlocks, &heard);
    REQUIRE(rig.generation() == generation);

    // The bend reads below its centre at 40 << 7; every other source reads up.
    const int source_sign = row.source == kSourceBend ? -1 : 1;
    const int depth_sign = row.depth > kDepthZero ? 1 : row.depth < kDepthZero ? -1 : 0;
    const int moved = rig.byte() - kBase;
    const int direction = moved > 0 ? 1 : moved < 0 ? -1 : 0;
    CHECK(direction == source_sign * depth_sign);

    if (!row.classic) {
      if (depth_sign == 0) {
        CHECK(rig.log().count(stage, key) == writes_before);
      } else {
        CHECK(rig.log().last(stage, key) ==
              s::gs_efx_binding_value(dest, static_cast<uint8_t>(rig.byte())));
      }
    } else if (depth_sign != 0) {
      // The classic unit sounds the moved byte: the same run at depth 0 differs.
      Setup still = setup;
      still.depth = kDepthZero;
      Rig reference(still);
      reference.move_source(kStimulus, midi2);
      std::vector<float> unmoved;
      reference.render(kListenBlocks, &unmoved);
      CHECK(max_abs_difference(heard, unmoved) > 1e-4);
    }

    if (midi2) {
      // from7 / from14 widen a MIDI 1.0 value exactly, so the same value sent
      // as MIDI 1.0 renders the same samples and the same byte.
      Rig narrow(setup);
      narrow.move_source(kStimulus, false);
      std::vector<float> narrow_heard;
      narrow.render(kListenBlocks, &narrow_heard);
      CHECK(narrow.byte() == rig.byte());
      CHECK(max_abs_difference(heard, narrow_heard) == 0.0);
    }
  }
}

TEST_CASE("an EFX CONTROL reads a MIDI 2.0 value at full width",
          "[midi][synth][gs][gs-efx-control]") {
  for (const uint8_t source : {kSourceCc1, kSourceBend}) {
    Setup setup;
    setup.live = true;
    setup.source = source;
    INFO("source " << int(source));
    const auto byte_at = [&setup](uint32_t raw) {
      Rig rig(setup);
      rig.move_source_raw(raw);
      rig.render(1);
      return rig.byte();
    };
    uint32_t lo_raw = 0;
    uint32_t hi_raw = 0;
    if (source == kSourceBend) {
      lo_raw = m::Bend32::from14(70u << 7).raw;
      hi_raw = m::Bend32::from14(71u << 7).raw;
    } else {
      lo_raw = m::Control32::from7(kStimulus).raw;
      hi_raw = m::Control32::from7(kStimulus + 1).raw;
    }
    const int lo = byte_at(lo_raw);
    const int hi = byte_at(hi_raw);
    // Adjacent 7-bit positions land on different bytes here, so the value
    // between them has somewhere to fall.
    REQUIRE(lo < hi);
    const int between = byte_at(lo_raw + (hi_raw - lo_raw) / 2);
    CHECK(between >= lo);
    CHECK(between <= hi);
  }
}

TEST_CASE("an EFX CONTROL whose type marks no slot is inert", "[midi][synth][gs][gs-efx-control]") {
  // 01 00 prints a `+` (Level) and no `#`, so CONTROL 2 has nothing to move.
  Setup setup;
  setup.type = kLofi1;
  setup.live = true;
  setup.control = 1;
  Rig rig(setup);
  REQUIRE(rig.byte() == -1);
  const size_t writes = rig.log().entries.size();
  rig.move_source(127, false);
  rig.render(4);
  CHECK(rig.byte() == -1);
  CHECK(rig.log().entries.size() == writes);

  // CONTROL 1 on the same type does reach its `+`.
  setup.control = 0;
  Rig plus(setup);
  REQUIRE(plus.byte() == kBase);
  plus.move_source(127, false);
  plus.render(1);
  CHECK(plus.byte() > kBase);
}

TEST_CASE("an EFX CONTROL over a two-state slot switches it at half",
          "[midi][synth][gs][gs-efx-control]") {
  // Rotary Speed (01 22 slot 10) prints 00/7F. From 00 at full depth, the wheel
  // below half leaves it slow and above half makes it fast.
  REQUIRE(s::gs_classic::gs_classic_default_registry().find(kRotary) != nullptr);
  Setup setup;
  setup.type = kRotary;
  setup.classic = true;
  setup.live = true;
  Rig rig(setup);
  rig.sysex(efx_write(slot_offset(10), 0x00));
  rig.render(1);
  REQUIRE(rig.byte() == 0x00);
  const uint32_t generation = rig.generation();
  rig.move_source(63, false);
  rig.render(1);
  CHECK(rig.byte() == 0x00);
  rig.move_source(66, false);
  rig.render(1);
  CHECK(rig.byte() == 0x7F);
  rig.rest();
  rig.render(1);
  CHECK(rig.byte() == 0x00);
  CHECK(rig.generation() == generation);
}

TEST_CASE("an edit to a CONTROL's slot keeps the modulation", "[midi][synth][gs][gs-efx-control]") {
  Setup setup;
  setup.live = true;
  Rig rig(setup);
  const s::GsEfxBindingRow& dest = marked_row(kStepFlanger, '+');
  const std::string stage(s::kGsEfxRowStages[dest.stage]);
  const std::string key(s::kGsEfxRowKeys[dest.key]);
  rig.move_source(kStimulus, false);
  rig.render(1);
  const int modulated = rig.byte();
  REQUIRE(modulated > kBase);
  const uint32_t generation = rig.generation();

  SECTION("an unrelated slot's edit re-sends the base, and the same block restores it") {
    // Every live edit re-sends all of the unit's keys at their base values; the
    // drain marks the CONTROL's destination and the apply writes it back.
    const size_t writes = rig.log().count(stage, key);
    rig.sysex(efx_write(slot_offset(5), 0x50));
    rig.render(1);
    REQUIRE(rig.generation() == generation);
    REQUIRE(rig.log().count(stage, key) > writes);
    CHECK(rig.log().last(stage, key) ==
          s::gs_efx_binding_value(dest, static_cast<uint8_t>(modulated)));
    CHECK(rig.byte() == modulated);
  }

  SECTION("the slot's own edit moves the base under the modulation") {
    rig.sysex(efx_write(slot_offset(dest.slot), 0x20));
    rig.render(1);
    REQUIRE(rig.generation() == generation);
    CHECK(rig.byte() > 0x20);
    CHECK(rig.byte() < modulated);
    CHECK(rig.log().last(stage, key) ==
          s::gs_efx_binding_value(dest, static_cast<uint8_t>(rig.byte())));
  }

  SECTION("a classic unit takes the slot's edit the same way") {
    Setup classic = setup;
    classic.classic = true;
    Rig unit(classic);
    unit.move_source(kStimulus, false);
    unit.render(1);
    const int before = unit.byte();
    REQUIRE(before > kBase);
    const uint32_t classic_generation = unit.generation();
    unit.sysex(efx_write(slot_offset(dest.slot), 0x20));
    unit.render(1);
    REQUIRE(unit.generation() == classic_generation);
    CHECK(unit.byte() > 0x20);
    CHECK(unit.byte() < before);
  }

  SECTION("a source or depth edit is relative, and the live unit stays modulated") {
    rig.sysex(efx_write(depth_offset(0), 0x60));
    // Live CONTROL edits are relative audio deltas. They update the selected
    // node in place, so unrelated tails and the published generation survive.
    REQUIRE(rig.generation() == generation);
    rig.render(1);
    CHECK(rig.byte() > kBase);
    CHECK(rig.byte() < modulated);
  }
}

TEST_CASE("every EFX CONTROL destination here is a realtime-safe control",
          "[midi][synth][gs][gs-efx-control]") {
  // The classic-only Speed row names a stage and key the modern chain does not
  // realise, so it is not a modern destination.
  for (const s::GsEfxBindingRow& row : marked_rows()) {
    if (row.printed_mark == 0 || (row.type == kRotary && row.slot == 10)) continue;
    const std::string stage(s::kGsEfxRowStages[row.stage]);
    const std::string key(s::kGsEfxRowKeys[row.key]);
    INFO(stage << "." << key);
    const auto proc = sonare::mastering::api::make_insert(stage, "{}");
    REQUIRE(proc != nullptr);
    bool found = false;
    for (const sonare::rt::ParamDescriptor& d : proc->parameter_descriptors()) {
      if (d.key != key) continue;
      found = true;
      CHECK(proc->parameter_is_realtime_safe(d.id));
    }
    CHECK(found);
  }
}

#else

TEST_CASE("the EFX CONTROL tests need the FX suite and the insert factory", "[gs-efx-control]") {
  SKIP("built without SONARE_MIDI_WITH_FX / SONARE_WITH_MASTERING");
}

#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING
