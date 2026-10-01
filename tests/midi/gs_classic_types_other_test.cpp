/// @file gs_classic_types_other_test.cpp
/// @brief GS classic realization, MSB 02-11 types: every printed byte is heard.
///
/// For each type and printed slot, the default configuration (p0 models with overlays) is
/// drawn at 32 kHz from the power-on bytes with that slot at the first and at the last byte
/// of its printed range. The two drawings must differ by at least 0.1 dB in some band of the
/// per-channel third-octave digest, or by a relative L2 of at least 1e-3. The printed
/// slots are the rows of `gs_classic_reference.tsv`, their printed ends the model's, and the
/// stimulus and digest are the ones its header states. The default run draws each type's
/// first printed slot; the full sweep is slow and runs under `[gs-classic-types-other-all]`.
/// Endpoint pairs are checked first; for a range of more than two bytes whose ends alias, the
/// shared sensitivity helper also checks the nearest accepted midpoint.
///
/// The carried cases hold what these overlays write by hand to its source: the cabinet
/// sections to `cab_voicing`, the ring modulator's balance ramps to the 05 00 candidate's
/// PS Bal, and the enhancer band to the 01 02 overlay.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "mastering/saturation/cab_voicing.h"
#include "midi/gs_classic_sensitivity.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "midi/synth/gs_classic/sections.h"
#include "util/constants.h"

namespace sonare::midi::synth::gs_classic {
#define GS_CLASSIC_SET_PREFIX gs_classic_models_raw_other
#include "midi/gs_classic_models_raw.inc"
#undef GS_CLASSIC_SET_PREFIX
}  // namespace sonare::midi::synth::gs_classic

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using gc::test::gs_classic_require_heard;
namespace sat = sonare::mastering::saturation;
using sonare::constants::kTwoPiD;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfxTypeDefaults;

constexpr uint16_t kFirstType = 0x0200;
constexpr uint16_t kLastType = 0x11FF;
constexpr const char* kReferencePath = "tests/midi/gs_classic_reference.tsv";

// Stimulus and digest as the reference TSV header states them.
constexpr std::size_t kHalf = 32000;
constexpr std::size_t kStimulusLength = 2 * kHalf;
constexpr uint32_t kSeedL = 0x12345678u;
constexpr uint32_t kSeedR = 0x9E3779B9u;
constexpr int64_t kSawHz = 220;
constexpr double kStimulusScale = 0.25;
constexpr std::size_t kWindow = 8000;
constexpr int kLowestBand = -13;
constexpr int kHighestBand = 11;
constexpr double kDigestFloorDb = -300.0;
constexpr double kPowerFloor = 1e-30;

// Success condition 3.
constexpr double kHeardDb = 0.1;
constexpr double kHeardRelativeL2 = 1e-3;
/// A band under this in both drawings is below anything the comparison should weigh.
constexpr double kBandFloorDb = -100.0;

constexpr double kCabToleranceDb = 0.5;
constexpr std::size_t kBlock = 4096;

struct Printed {
  uint8_t lo = 0;
  uint8_t hi = 0;
};

/// (type, slot) -> the first and last printed byte, for each printed slot the reference
/// TSV lists. A byte outside the printed range is a protocol question, not the model's.
std::map<std::pair<uint16_t, int>, Printed> printed_slots() {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  std::map<std::pair<uint16_t, int>, Printed> out;
  std::ifstream in(kReferencePath);
  REQUIRE(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("type", 0) == 0) continue;
    std::istringstream row(line);
    std::string type, slot;
    std::getline(row, type, '\t');
    std::getline(row, slot, '\t');
    if (slot == "-") continue;
    const auto number = static_cast<uint16_t>(std::stoi(type.substr(0, 2), nullptr, 16) << 8 |
                                              std::stoi(type.substr(3, 2), nullptr, 16));
    if (number < kFirstType || number > kLastType) continue;
    const gc::GsClassicType* model = registry.find(number);
    REQUIRE(model != nullptr);
    const auto at = static_cast<std::size_t>(std::stoi(slot));
    out.emplace(std::make_pair(number, std::stoi(slot)),
                Printed{model->printed_lo[at], model->printed_hi[at]});
  }
  return out;
}

double noise_sample(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return static_cast<double>(static_cast<int16_t>(s >> 16)) / 32768.0;
}

std::vector<double> make_stimulus(uint32_t seed) {
  std::vector<double> x(kStimulusLength);
  for (std::size_t i = 0; i < kHalf; ++i) x[i] = kStimulusScale * noise_sample(seed);
  for (std::size_t i = 0; i < kHalf; ++i) {
    const int64_t phase = (static_cast<int64_t>(i) * kSawHz * 65536 / 32000) % 65536;
    x[kHalf + i] = kStimulusScale * static_cast<double>(phase - 32768) / 32768.0;
  }
  return x;
}

const std::vector<double>& stimulus(int channel) {
  static const std::vector<double> left = make_stimulus(kSeedL);
  static const std::vector<double> right = make_stimulus(kSeedR);
  return channel == 0 ? left : right;
}

struct Drawing {
  std::vector<double> ch[2];
};

Drawing draw(const gc::GsClassicModelSet& models, const gc::GsClassicType& type,
             const std::array<uint8_t, 20>& bytes) {
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(models, type, kBlock, &gc::gs_classic_extension_kernels()));
  graph.reset();
  for (std::size_t slot = 0; slot < bytes.size(); ++slot) graph.set_byte(slot, bytes[slot]);
  Drawing out;
  out.ch[0].resize(kStimulusLength);
  out.ch[1].resize(kStimulusLength);
  graph.process(stimulus(0).data(), stimulus(1).data(), out.ch[0].data(), out.ch[1].data(),
                kStimulusLength);
  return out;
}

/// [channel][window][band] in dB.
std::vector<double> digest(const Drawing& d) {
  sonare::FFT fft(static_cast<int>(kWindow));
  std::vector<float> frame(kWindow);
  std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.n_bins()));
  const double hz_per_bin = 32000.0 / static_cast<double>(kWindow);
  std::vector<double> out;
  for (int c = 0; c < 2; ++c) {
    for (std::size_t start = 0; start + kWindow <= kStimulusLength; start += kWindow) {
      for (std::size_t i = 0; i < kWindow; ++i) frame[i] = static_cast<float>(d.ch[c][start + i]);
      fft.forward(frame.data(), bins.data());
      for (int n = kLowestBand; n <= kHighestBand; ++n) {
        const double centre = 1000.0 * std::pow(2.0, n / 3.0);
        const double lo = centre * std::pow(2.0, -1.0 / 6.0);
        const double hi = centre * std::pow(2.0, 1.0 / 6.0);
        double power = 0.0;
        for (std::size_t k = 0; k < bins.size(); ++k) {
          const double f = static_cast<double>(k) * hz_per_bin;
          if (f < lo || f >= hi) continue;
          const double weight = (k == 0 || k + 1 == bins.size()) ? 1.0 : 2.0;
          power += weight * std::norm(std::complex<double>(bins[k])) /
                   (static_cast<double>(kWindow) * static_cast<double>(kWindow));
        }
        out.push_back(power < kPowerFloor ? kDigestFloorDb : 10.0 * std::log10(power));
      }
    }
  }
  return out;
}

double largest_band_difference(const std::vector<double>& a, const std::vector<double>& b) {
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] < kBandFloorDb && b[i] < kBandFloorDb) continue;
    worst = std::max(worst, std::fabs(a[i] - b[i]));
  }
  return worst;
}

double relative_l2(const Drawing& a, const Drawing& b) {
  double diff = 0.0, ea = 0.0, eb = 0.0;
  for (int c = 0; c < 2; ++c) {
    for (std::size_t i = 0; i < kStimulusLength; ++i) {
      const double d = a.ch[c][i] - b.ch[c][i];
      diff += d * d;
      ea += a.ch[c][i] * a.ch[c][i];
      eb += b.ch[c][i] * b.ch[c][i];
    }
  }
  const double scale = std::max(ea, eb);
  return scale > 0.0 ? std::sqrt(diff / scale) : 0.0;
}

/// 02 0C's bytes are filed under the manual's other number for it, which this lookup asks.
std::array<uint8_t, 20> power_on(uint16_t type) {
  const GsEfxTypeDefaults* entry = gs_efx_type_defaults(type);
  if (entry == nullptr) FAIL("no power-on bytes for type " << std::hex << type);
  return entry->params;
}

/// Whether the slot at its two ends changes the default configuration's drawing, with the
/// other bytes at `context`.
bool heard_in(uint16_t type, int slot, const Printed& ends,
              const std::array<uint8_t, 20>& context) {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicType* model = registry.find(type);
  REQUIRE(model != nullptr);
  std::array<uint8_t, 20> low = context;
  std::array<uint8_t, 20> high = context;
  low[static_cast<std::size_t>(slot)] = ends.lo;
  high[static_cast<std::size_t>(slot)] = ends.hi;
  const Drawing a = draw(registry.models(), *model, low);
  const Drawing b = draw(registry.models(), *model, high);
  const double band_db = largest_band_difference(digest(a), digest(b));
  const double l2 = relative_l2(a, b);
  UNSCOPED_INFO("largest band difference " << band_db << " dB, relative L2 " << l2);
  return band_db >= kHeardDb || l2 >= kHeardRelativeL2;
}

}  // namespace

TEST_CASE("GS classic types 02 00-11 08: each type's first printed byte is heard",
          "[gs-classic-types-other]") {
  gs_classic_require_heard(
      printed_slots(), [](bool first_of_type) { return first_of_type; }, power_on, heard_in);
}

TEST_CASE("GS classic types 02 00-11 08: every printed byte is heard",
          "[gs-classic-types-other-all][.][slow]") {
  gs_classic_require_heard(printed_slots(), [](bool) { return true; }, power_on, heard_in);
}

TEST_CASE("GS classic 11 05: OD Pan is heard at its interior", "[gs-classic-types-other]") {
  constexpr uint16_t kType = 0x1105;
  constexpr std::size_t kOdPanSlot = 15;
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicType* type = registry.find(kType);
  REQUIRE(type != nullptr);
  const gc::GsClassicModelRegistry raw(gc::gs_classic_models_raw_other());
  REQUIRE(raw.valid());
  const gc::GsClassicType* raw_type = raw.find(kType);
  REQUIRE(raw_type != nullptr);
  const auto power = power_on(kType);
  auto end = power;
  auto interior = power;
  end[kOdPanSlot] = 0;
  interior[kOdPanSlot] = 64;
  const double l2 =
      relative_l2(draw(registry.models(), *type, end), draw(registry.models(), *type, interior));
  INFO("11 05 OD Pan relative L2 at 0 versus 64: " << l2);
  CHECK(l2 >= kHeardRelativeL2);
  const double raw_l2 =
      relative_l2(draw(raw.models(), *raw_type, end), draw(raw.models(), *raw_type, interior));
  INFO("11 05 raw OD Pan relative L2 at 0 versus 64: " << raw_l2);
  CHECK(raw_l2 >= kHeardRelativeL2);
}

namespace {

const gc::GsClassicModelRegistry& raw_registry() {
  static const gc::GsClassicModelRegistry raw(gc::gs_classic_models_raw_other());
  return raw;
}

TEST_CASE("GS classic 11 07: Separate anchor preserves the p0 power-on drawing",
          "[gs-classic-types-other]") {
  constexpr uint16_t kType = 0x1107;
  constexpr double kSameDrawing = 1e-9;
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  const gc::GsClassicType* raw_type = raw.find(kType);
  const gc::GsClassicType* default_type = overlaid.find(kType);
  REQUIRE(raw_type != nullptr);
  REQUIRE(default_type != nullptr);
  const double l2 = relative_l2(draw(raw.models(), *raw_type, power_on(kType)),
                                draw(overlaid.models(), *default_type, power_on(kType)));
  INFO("11 07 raw/default power-on relative L2: " << l2);
  CHECK(l2 <= kSameDrawing);
}

/// The byte tables every value of a type reads `slot` through.
std::vector<const gc::GsClassicLut*> byte_luts(const gc::GsClassicModelSet& m,
                                               const gc::GsClassicType& t, uint8_t slot) {
  std::vector<const gc::GsClassicLut*> out;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    for (uint8_t k = 0; k < m.nodes[i].value_count; ++k) {
      const gc::GsClassicValue& v = m.values[m.nodes[i].value_begin + k];
      if (v.kind == gc::GsClassicValueKind::kByte && v.slot == slot) {
        out.push_back(&m.luts[v.table]);
      }
    }
  }
  return out;
}

/// A table's values over a slot's printed bytes.
std::vector<float> over_printed(const gc::GsClassicLut& lut, const gc::GsClassicType& t,
                                uint8_t slot) {
  return {lut.v + t.printed_lo[slot], lut.v + t.printed_hi[slot] + 1};
}

/// Values of a type's nodes that read a byte slot.
std::size_t byte_reads(const gc::GsClassicModelSet& m, const gc::GsClassicType& t) {
  std::size_t n = 0;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    for (uint8_t k = 0; k < m.nodes[i].value_count; ++k) {
      n += m.values[m.nodes[i].value_begin + k].kind == gc::GsClassicValueKind::kByte ? 1 : 0;
    }
  }
  return n;
}

double constant_of(const gc::GsClassicModelSet& m, const gc::GsClassicNode& node, uint8_t offset) {
  const gc::GsClassicValue& v = m.values[node.value_begin + offset];
  REQUIRE(v.kind == gc::GsClassicValueKind::kConst);
  return v.constant;
}

double biquad_db(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z1 = std::polar(1.0, -kTwoPiD * hz / gc::kGsClassicSampleRateHz);
  const std::complex<double> z2 = z1 * z1;
  return 20.0 * std::log10(std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)));
}

double coeffs_db(const sonare::rt::BiquadCoeffs& c, double hz) {
  return biquad_db(c.b0, c.b1, c.b2, c.a1, c.a2, hz);
}

double section_db(const gc::GsClassicModelSet& m, const gc::GsClassicNode& node, double hz) {
  const gc::GsClassicSection& s = m.sections[node.aux];
  gc::GsClassicSectionValues values;
  if (s.corner != gc::kGsClassicAbsent) values.corner_hz = constant_of(m, node, s.corner);
  if (s.gain != gc::kGsClassicAbsent) values.gain_db = constant_of(m, node, s.gain);
  if (s.q != gc::kGsClassicAbsent) {
    values.q = constant_of(m, node, s.q);
    values.has_q = true;
  }
  if (s.count != gc::kGsClassicAbsent) values.count = constant_of(m, node, s.count);
  if (s.mix != gc::kGsClassicAbsent) values.mix = constant_of(m, node, s.mix);
  const gc::GsClassicReachedBy* reached =
      s.reached_by == gc::kGsClassicNone ? nullptr : &m.reached_by[s.reached_by];
  constexpr std::size_t kRows = 8;
  double rows[kRows * gc::kGsClassicSectionRowSize];
  const std::size_t n = gc::gs_classic_design_section(s, reached, values, rows, kRows);
  REQUIRE(n > 0);
  double db = 0.0;
  for (std::size_t r = 0; r < n; ++r) {
    const double* p = rows + r * gc::kGsClassicSectionRowSize;
    db += biquad_db(p[0], p[1], p[2], p[3], p[4], hz);
  }
  return db;
}

/// Amp Type's selecting mix on `amp_slot`, read over its `printed` states: model index -> the
/// sections of that model's chain.
std::map<int, std::vector<const gc::GsClassicNode*>> cab_chains(const gc::GsClassicModelSet& m,
                                                                const gc::GsClassicType& t,
                                                                uint8_t amp_slot, int printed) {
  std::map<uint16_t, const gc::GsClassicNode*> by_signal;
  uint16_t signal = 2;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    by_signal[signal] = &m.nodes[i];
    signal += m.nodes[i].kind == gc::GsClassicNodeKind::kPan ? 2 : 1;
  }
  std::map<int, std::vector<const gc::GsClassicNode*>> chains;
  for (uint16_t i = t.node_begin; i < t.node_end; ++i) {
    const gc::GsClassicNode& node = m.nodes[i];
    if (node.kind != gc::GsClassicNodeKind::kMix) continue;
    for (uint8_t k = 0; k < node.n_inputs; ++k) {
      const gc::GsClassicValue& w = m.values[node.value_begin + k];
      if (w.kind != gc::GsClassicValueKind::kByte || w.slot != amp_slot) continue;
      const gc::GsClassicLut& lut = m.luts[w.table];
      int model = -1;
      int states = 0;
      for (int state = 0; state < printed; ++state) {
        if (lut.v[state] == 1.0f) {
          model = state;
          ++states;
        }
      }
      // Each Amp Type state selects exactly one cabinet chain.
      REQUIRE(states == 1);
      std::vector<const gc::GsClassicNode*> chain;
      uint16_t at = m.inputs[node.input_begin + k].signal;
      while (by_signal.count(at) != 0 && by_signal[at]->kind == gc::GsClassicNodeKind::kSection) {
        chain.push_back(by_signal[at]);
        at = m.inputs[by_signal[at]->input_begin].signal;
      }
      chains[model] = chain;
    }
  }
  return chains;
}

}  // namespace

TEST_CASE("GS classic types other: the amp overlays' cabinet sections are cab_voicing's",
          "[gs-classic-types-other]") {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicModelSet& m = registry.models();
  struct Amp {
    uint16_t type;
    uint8_t amp_slot;
    int states;  ///< printed Amp Type states, each its own cab_model
  };
  // Every Amp Type slot these overlays bind; 11 03 has one per half, 04 05 prints three.
  constexpr Amp kAmps[] = {{0x0200, 2, 4}, {0x0201, 2, 4}, {0x0202, 2, 4}, {0x0203, 2, 4},
                           {0x0204, 2, 4}, {0x0205, 2, 4}, {0x0400, 6, 4}, {0x0401, 6, 4},
                           {0x0402, 6, 4}, {0x0405, 6, 3}, {0x1103, 2, 4}, {0x1103, 7, 4},
                           {0x1104, 2, 4}, {0x1105, 2, 4}, {0x1106, 2, 4}};
  constexpr sat::CabModel kModels[] = {sat::CabModel::kGuitar4x12, sat::CabModel::kBass8x10,
                                       sat::CabModel::kGuitar1x12Combo,
                                       sat::CabModel::kGuitar2x12Open};
  constexpr std::size_t kSectionsPerCab = 4;
  constexpr int kLowestCentre = -13;
  constexpr int kHighestCentre = 10;  // 24 third-octave centres, 49.6 Hz .. 10.1 kHz
  constexpr double kShapedDb = 3.0;
  double worst = 0.0;
  for (const Amp& amp : kAmps) {
    const gc::GsClassicType* t = registry.find(amp.type);
    REQUIRE(t != nullptr);
    const auto chains = cab_chains(m, *t, amp.amp_slot, amp.states);
    INFO("type " << std::hex << amp.type << std::dec << " slot " << int(amp.amp_slot));
    REQUIRE(chains.size() == static_cast<std::size_t>(amp.states));
    for (const auto& [index, chain] : chains) {
      INFO("cab_model " << index);
      REQUIRE(chain.size() == kSectionsPerCab);
      const sat::CabDesign design = sat::design_cab_stage(
          kModels[index], sat::MicModel::kNone, 0.0f, 0.0f, 0.0f, gc::kGsClassicSampleRateHz);
      bool shaped = false;
      for (int n = kLowestCentre; n <= kHighestCentre; ++n) {
        const double hz = 1000.0 * std::pow(2.0, n / 3.0);
        double got = 0.0;
        for (const gc::GsClassicNode* node : chain) got += section_db(m, *node, hz);
        const double want = coeffs_db(design.hp, hz) + coeffs_db(design.bump, hz) +
                            coeffs_db(design.presence, hz) + coeffs_db(design.lp1, hz) +
                            coeffs_db(design.lp2, hz);
        INFO(hz << " Hz: overlay " << got << " dB, cab_voicing " << want << " dB");
        CHECK(std::fabs(got - want) <= kCabToleranceDb);
        worst = std::max(worst, std::fabs(got - want));
        shaped = shaped || std::fabs(want) > kShapedDb;
      }
      // The comparison is only worth something against a response that is not flat.
      CHECK(shaped);
    }
  }
  INFO("largest deviation " << worst << " dB");
  CHECK(worst <= kCabToleranceDb);
}

TEST_CASE("GS classic types other: the rotary acceleration gaps match the 01 22 candidate's",
          "[gs-classic-types-other]") {
  constexpr uint16_t kRotary = 0x0122;
  constexpr uint8_t kRotaryLowAccel = 2;
  constexpr uint8_t kRotaryHighAccel = 6;
  struct Accel {
    uint16_t type;
    uint8_t low;
    uint8_t high;
  };
  constexpr Accel kCarried[] = {{0x020C, 9, 13}, {0x1104, 7, 11}, {0x1107, 7, 11}};
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  const gc::GsClassicType* rotary = raw.find(kRotary);
  REQUIRE(rotary != nullptr);
  for (const Accel& a : kCarried) {
    const gc::GsClassicType* t = overlaid.find(a.type);
    REQUIRE(t != nullptr);
    for (const auto& [slot, source] :
         {std::make_pair(a.low, kRotaryLowAccel), std::make_pair(a.high, kRotaryHighAccel)}) {
      INFO("type " << std::hex << a.type << std::dec << " slot " << int(slot));
      const auto gaps = byte_luts(overlaid.models(), *t, slot);
      const auto measured = byte_luts(raw.models(), *rotary, source);
      // One gap a rotor, in the carrying type and in the candidate it is carried from.
      REQUIRE(gaps.size() == 1);
      REQUIRE(measured.size() == 1);
      REQUIRE(t->printed_lo[slot] == rotary->printed_lo[source]);
      REQUIRE(t->printed_hi[slot] == rotary->printed_hi[source]);
      CHECK(over_printed(*gaps.front(), *t, slot) ==
            over_printed(*measured.front(), *rotary, source));
    }
  }
}

TEST_CASE("GS classic types other: RM Bal reads the 05 00 candidate's own balance ramps",
          "[gs-classic-types-other]") {
  constexpr uint16_t kType = 0x0500;
  constexpr uint8_t kRmBal = 1;
  constexpr uint8_t kPsBal = 10;
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(overlaid.valid());
  const gc::GsClassicType* t = overlaid.find(kType);
  REQUIRE(t != nullptr);
  REQUIRE(t->printed_lo[kRmBal] == t->printed_lo[kPsBal]);
  REQUIRE(t->printed_hi[kRmBal] == t->printed_hi[kPsBal]);
  auto ramps = [&](uint8_t slot) {
    std::vector<std::vector<float>> out;
    for (const gc::GsClassicLut* lut : byte_luts(overlaid.models(), *t, slot)) {
      out.push_back(over_printed(*lut, *t, slot));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  };
  const auto rm = ramps(kRmBal);
  // The dry ramp and the effect ramp.
  REQUIRE(rm.size() == 2);
  CHECK(rm == ramps(kPsBal));
}

TEST_CASE("GS classic types other: the enhancer band is the 01 02 overlay's",
          "[gs-classic-types-other]") {
  constexpr uint16_t kSource = 0x0102;
  constexpr uint16_t kEnhancers[] = {0x0206, 0x0207, 0x0208, 0x0406};
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(overlaid.valid());
  const gc::GsClassicModelSet& m = overlaid.models();
  // (centre, gain, q) of every peaking section whose values are all constants.
  auto fixed_peaks = [&](uint16_t type) {
    const gc::GsClassicType* t = overlaid.find(type);
    REQUIRE(t != nullptr);
    std::vector<std::tuple<double, double, double>> out;
    for (uint16_t i = t->node_begin; i < t->node_end; ++i) {
      const gc::GsClassicNode& node = m.nodes[i];
      if (node.kind != gc::GsClassicNodeKind::kSection) continue;
      const gc::GsClassicSection& s = m.sections[node.aux];
      if (s.stage != gc::GsClassicStage::kPeaking) continue;
      bool fixed = true;
      for (uint8_t k = 0; k < node.value_count; ++k) {
        fixed = fixed && m.values[node.value_begin + k].kind == gc::GsClassicValueKind::kConst;
      }
      if (!fixed) continue;
      out.emplace_back(constant_of(m, node, s.corner), constant_of(m, node, s.gain),
                       constant_of(m, node, s.q));
    }
    return out;
  };
  const auto source = fixed_peaks(kSource);
  // One band a side.
  REQUIRE(source.size() == 2);
  for (uint16_t type : kEnhancers) {
    INFO("type " << std::hex << type);
    CHECK(fixed_peaks(type) == source);
  }
}

TEST_CASE("GS classic types other: overlays silent at power-on leave the drawing as p0 draws it",
          "[gs-classic-types-other]") {
  // Amp Sw off at power-on (02 00-02 05, and 04 05 whose detector times are also anchored
  // (D33)); Sens 0 at power-on (11 08).
  constexpr uint16_t kUntouchedAtPowerOn[] = {0x0200, 0x0201, 0x0202, 0x0203,
                                              0x0204, 0x0205, 0x0405, 0x1108};
  constexpr double kSameDrawing = 1e-9;
  const gc::GsClassicModelRegistry& raw = raw_registry();
  const gc::GsClassicModelRegistry& overlaid = gc::gs_classic_default_registry();
  REQUIRE(raw.valid());
  REQUIRE(overlaid.valid());
  double worst = 0.0;
  for (uint16_t type : kUntouchedAtPowerOn) {
    INFO("type " << std::hex << type);
    const gc::GsClassicType* r = raw.find(type);
    const gc::GsClassicType* d = overlaid.find(type);
    REQUIRE(r != nullptr);
    REQUIRE(d != nullptr);
    // Each overlay binds a slot the candidate left unread, so the two graphs differ.
    REQUIRE(byte_reads(overlaid.models(), *d) > byte_reads(raw.models(), *r));
    const std::array<uint8_t, 20> bytes = power_on(type);
    const double l2 =
        relative_l2(draw(raw.models(), *r, bytes), draw(overlaid.models(), *d, bytes));
    INFO("relative L2 " << l2);
    CHECK(l2 <= kSameDrawing);
    worst = std::max(worst, l2);
  }
  WARN("largest power-on relative L2 against the raw configuration " << worst);
}
