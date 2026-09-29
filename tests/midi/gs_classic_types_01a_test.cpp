/// @file gs_classic_types_01a_test.cpp
/// @brief GS classic realization, MSB 01 types 01 00-01 31: every printed byte is heard.
///
/// For each type and printed slot, the default configuration (p0 models with overlays) is
/// drawn at 32 kHz from the power-on bytes with that slot at the first and at the last byte
/// of its printed range. The two drawings must differ by at least 0.1 dB in some band of the
/// per-channel third-octave digest, or by a relative L2 of at least 1e-3. The printed
/// slots are the rows of `gs_classic_reference.tsv`, their printed ends the model's, and the
/// stimulus and digest are the ones its header states. The default run draws each type's
/// first printed slot; the full sweep is slow and runs under `[gs-classic-types-01a-all]`.
///
/// The amp-section case holds the cabinet sections the 01 10 / 01 11 overlays write to
/// `cab_voicing`'s design of the same model: the overlays carry those numbers by hand, so
/// this is what keeps the two sources one.

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
#define GS_CLASSIC_SET_PREFIX gs_classic_models_raw_01a
#include "midi/gs_classic_models_raw.inc"
#undef GS_CLASSIC_SET_PREFIX
}  // namespace sonare::midi::synth::gs_classic

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using gc::test::gs_classic_require_heard;
namespace sat = sonare::mastering::saturation;
using sonare::constants::kTwoPiD;
using sonare::midi::synth::kGsEfxTypeDefaults;

constexpr uint16_t kFirstType = 0x0100;
constexpr uint16_t kLastType = 0x0131;
constexpr uint16_t kHumanizer = 0x0103;
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
    // 01 03 is left out until the vowel filter insert exists: its vowels and Accel have
    // nothing to bind to before then.
    if (number == kHumanizer) continue;
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

std::array<uint8_t, 20> power_on(uint16_t type) {
  for (const auto& entry : kGsEfxTypeDefaults) {
    if (entry.type == type) return entry.params;
  }
  FAIL("no power-on bytes for type " << std::hex << type);
  return {};
}

// -- amp sections --

double biquad_db(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z1 = std::polar(1.0, -kTwoPiD * hz / gc::kGsClassicSampleRateHz);
  const std::complex<double> z2 = z1 * z1;
  return 20.0 * std::log10(std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)));
}

double coeffs_db(const sonare::rt::BiquadCoeffs& c, double hz) {
  return biquad_db(c.b0, c.b1, c.b2, c.a1, c.a2, hz);
}

double constant_of(const gc::GsClassicModelSet& m, const gc::GsClassicNode& node, uint8_t offset) {
  const gc::GsClassicValue& v = m.values[node.value_begin + offset];
  REQUIRE(v.kind == gc::GsClassicValueKind::kConst);
  return v.constant;
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

/// Amp Type's selecting mix: model index -> the sections of that model's chain.
std::map<int, std::vector<const gc::GsClassicNode*>> cab_chains(const gc::GsClassicModelSet& m,
                                                                const gc::GsClassicType& t) {
  constexpr uint8_t kAmpTypeSlot = 1;
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
      if (w.kind != gc::GsClassicValueKind::kByte || w.slot != kAmpTypeSlot) continue;
      const gc::GsClassicLut& lut = m.luts[w.table];
      int model = -1;
      for (int state = 0; state < 2; ++state) {
        if (lut.v[state] == 1.0f) model = state;
      }
      REQUIRE(model >= 0);
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

namespace {

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

TEST_CASE("GS classic types 01 00-01 31: each type's first printed byte is heard",
          "[gs-classic-types-01a]") {
  gs_classic_require_heard(
      printed_slots(), [](bool first_of_type) { return first_of_type; }, power_on, heard_in);
}

TEST_CASE("GS classic types 01 00-01 31: every printed byte is heard",
          "[gs-classic-types-01a-all][.][slow]") {
  gs_classic_require_heard(printed_slots(), [](bool) { return true; }, power_on, heard_in);
}

TEST_CASE("GS classic types 01a: the amp overlay's cabinet sections are cab_voicing's",
          "[gs-classic-types-01a]") {
  const gc::GsClassicModelRegistry& registry = gc::gs_classic_default_registry();
  REQUIRE(registry.valid());
  const gc::GsClassicModelSet& m = registry.models();
  // The amp types' cabinets, one chain per cab_model the overlay draws.
  constexpr uint16_t kAmpTypes[] = {0x0110, 0x0111};
  constexpr sat::CabModel kModels[] = {sat::CabModel::kGuitar4x12, sat::CabModel::kBass8x10};
  constexpr std::size_t kSectionsPerCab = 4;
  constexpr int kLowestCentre = -13;
  constexpr int kHighestCentre = 10;  // 24 third-octave centres, 49.6 Hz .. 10.1 kHz
  constexpr double kShapedDb = 3.0;
  double worst = 0.0;
  for (uint16_t type : kAmpTypes) {
    const gc::GsClassicType* t = registry.find(type);
    REQUIRE(t != nullptr);
    const auto chains = cab_chains(m, *t);
    REQUIRE(chains.size() == std::size(kModels));
    for (const auto& [index, chain] : chains) {
      INFO("type " << std::hex << type << std::dec << " cab_model " << index);
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

namespace {

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

}  // namespace

TEST_CASE("GS classic types 01a: anchored overlays leave the power-on drawing as p0 draws it",
          "[gs-classic-types-01a]") {
  // These overlays either add paths their power-on bytes keep silent (Sens 0, Pre Filter
  // Off) or replace a fitted constant with a law anchored to it at the power-on byte (D33).
  constexpr uint16_t kUntouchedAtPowerOn[] = {0x0121, 0x0122, 0x0123, 0x0130, 0x0131};
  constexpr double kSameDrawing = 1e-9;
  static const gc::GsClassicModelRegistry raw(gc::gs_classic_models_raw_01a());
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
