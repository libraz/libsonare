/// @file bowed_string_force_window_test.cpp
/// @brief The bow-force window: where multiple slip begins as a function of bow
///        position, swept over the friction law's two branches. Cases carry
///        [bowed][window] and are invoked as an AND, because [window] alone
///        reaches other tests.
///
/// Schelleng's upper limit falls as 1/beta, so a bow near the bridge should
/// break into multiple slip at a force a bow over the fingerboard still holds.
/// This sweep asks whether the model has that breakdown at all and whether its
/// threshold moves with position. The answer is a BRANCH for the force law
/// rather than a pass condition: the predicate's truth value is reported and
/// what is asserted is that it was evaluated over the whole grid, so a sweep
/// that never ran, or one whose every cell returns the same number, is the
/// failure rather than a false predicate.
///
/// polarization is pinned to 0 throughout. Elasto-plastic friction and the
/// second polarization together double-slip in the low register for reasons
/// that have nothing to do with bow force, and this is exactly the measurement
/// that would read that as Schelleng's limit.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"

namespace {

using sonare::midi::synth::BowedStringPatchParams;
using sonare::midi::synth::BowedStringVoiceCore;
using sonare::test::bowed::kHelmholtzHi;
using sonare::test::bowed::slips_per_period;
using sonare::test::bowed::time_to_helmholtz;

constexpr double kSr = 48000.0;
/// One second, so the trailing half the slip rate is read over sits well past
/// the 0.236 s this patch takes to establish Helmholtz motion.
constexpr int kSweepSamples = 48000;
constexpr uint8_t kSweepNote = 60;
constexpr uint8_t kSweepVelocity = 100;
constexpr uint64_t kSweepSeed = 0x5011ADE5ull;

constexpr float kBetaGrid[] = {0.04f, 0.08f, 0.13f, 0.18f, 0.25f, 0.32f, 0.40f};
constexpr int kBetaCount = 7;
constexpr int kForceCount = 11;

/// A force one step past the top of the grid stands in for "no force on the
/// grid reached multiple slip", so the spread below is one arithmetic over
/// every case including the mixed one.
constexpr double kForceOffGrid = 1.1;
/// The predicate's move threshold: two force grid steps.
constexpr double kForceMoveMin = 0.2;
/// A spectral peak below this multiple of the played fundamental means the
/// string is sounding the note it was given. Above it the waveform's period is
/// a fraction of the assumed one, and a slip rate read against the assumed
/// period counts that as multiple slip.
constexpr double kPlayedPitchHi = 1.5;

/// One swept cell. `reached` is the reach: false means the slip rate was never
/// counted and the numbers beside it carry no verdict.
struct Cell {
  double per_period = 0.0;
  bool helmholtz = false;
  double level_db = sonare::constants::kFloorDbD;
  double peak_ratio = 0.0;
  bool reached = false;
};

struct Sheet {
  Cell cells[kBetaCount][kForceCount];
  int reached = 0;
  double f_star[kBetaCount] = {};
  int f_star_defined = 0;
  double rate_lo = 0.0;
  double rate_hi = 0.0;
  double f_star_spread = 0.0;
  bool exists = false;
  bool moves = false;
  bool predicate = false;
  // The upper limit read strictly: the first force to break an ALREADY
  // established Helmholtz regime. P's F* cannot tell that from a grid whose
  // bottom force is already in multiple slip, which is Schelleng's lower limit.
  double f_up[kBetaCount] = {};
  int f_up_defined = 0;
  double f_up_spread = 0.0;
  // Schelleng's upper limit as he states it: multiple slip WITHIN the played
  // period, entered from a Helmholtz regime at the same pitch. This is the
  // population a force law would have to move, and it is the one F* conflates
  // with a string that simply locked onto a shorter period.
  double f_schelleng[kBetaCount] = {};
  int f_schelleng_defined = 0;
  double f_schelleng_spread = 0.0;
  int played_cells = 0;
  int played_multi_cells = 0;
  double peak_lo = 0.0;
  double peak_hi = 0.0;
};

double force_at(int index) noexcept { return static_cast<double>(index) / 10.0; }

/// Dominant spectral peak of @p steady as a multiple of the played fundamental.
/// slips_per_period is GIVEN f0 rather than measuring it, so a string locked
/// onto a shorter period reads as multiple slip; this is what separates them.
double peak_ratio(const std::vector<float>& steady, double f0) {
  const int fft = sonare::test::bowed::probe_fft_size(steady.size());
  const std::vector<double> power = sonare::test::bowed::probe_power_spectrum(
      steady, sonare::test::bowed::probe_window_start(steady.size(), fft), fft);
  std::size_t peak = 0;
  double top = 0.0;
  for (std::size_t b = 1; b < power.size(); ++b) {
    if (power[b] > top) {
      top = power[b];
      peak = b;
    }
  }
  if (!(top > 0.0)) return 0.0;
  return static_cast<double>(peak) * kSr / static_cast<double>(fft) / f0;
}

/// The shipped violin patch (GM program 40, bank 0) — every field the sweep
/// does not drive is held at its value.
const BowedStringPatchParams& violin_params() {
  return sonare::midi::synth::gm_fallback_patch(0, 40).bowed_string;
}

std::vector<float> render_core(const BowedStringPatchParams& params, uint8_t note, int samples) {
  BowedStringVoiceCore core;
  const int per_line = sonare::midi::synth::bowed_string_buffer_capacity(kSr);
  std::vector<float> slab(
      static_cast<std::size_t>(sonare::midi::synth::bowed_string_slab_capacity(kSr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, kSr, note, kSweepVelocity, kSweepSeed);
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<std::size_t>(i)] = core.render(1.0f);
  return out;
}

/// RMS of the steady region (the trailing half the slip rate is read over), in
/// dB — the column that tells a cell that slips zero times from one that is
/// silent.
double steady_level_db(const std::vector<float>& span) {
  const std::size_t from = span.size() / 2;
  double acc = 0.0;
  for (std::size_t i = from; i < span.size(); ++i) {
    acc += static_cast<double>(span[i]) * static_cast<double>(span[i]);
  }
  const double rms = std::sqrt(acc / static_cast<double>(span.size() - from));
  return 20.0 * std::log10(std::max(rms, static_cast<double>(sonare::constants::kAmpEpsilon)));
}

/// One cell: render the patch and read the steady slip rate, whether Helmholtz
/// motion ever established, and the level and sounding pitch it was read at.
Cell measure_cell(const BowedStringPatchParams& params, uint8_t note, int samples) {
  Cell cell;
  const double f0 = static_cast<double>(sonare::midi::synth::note_to_hz(note));
  const std::vector<float> render = render_core(params, note, samples);
  const sonare::test::bowed::SlipRate rate = slips_per_period(render, f0, kSr);
  if (!(rate.periods > 0.0)) return cell;
  cell.per_period = rate.per_period;
  cell.helmholtz = time_to_helmholtz(render, f0, kSr).established;
  cell.level_db = steady_level_db(render);
  const std::vector<float> steady(render.begin() + static_cast<long>(render.size() / 2),
                                  render.end());
  cell.peak_ratio = peak_ratio(steady, f0);
  cell.reached = true;
  return cell;
}

/// One sheet of the sweep: beta x force on a single friction branch, with the
/// three thresholds read off it — P's F*, the strict F_up, and F_schelleng.
Sheet sweep(bool elasto_plastic) {
  Sheet sheet;
  bool first = true;
  for (int bi = 0; bi < kBetaCount; ++bi) {
    sheet.f_star[bi] = kForceOffGrid;
    sheet.f_up[bi] = kForceOffGrid;
    sheet.f_schelleng[bi] = kForceOffGrid;
    bool held_helmholtz = false;
    bool held_played_helmholtz = false;
    for (int fi = 0; fi < kForceCount; ++fi) {
      BowedStringPatchParams params = violin_params();
      params.polarization = 0.0f;
      params.elasto_plastic = elasto_plastic;
      params.bow_position = kBetaGrid[bi];
      params.bow_force = static_cast<float>(force_at(fi));
      const Cell cell = measure_cell(params, kSweepNote, kSweepSamples);
      sheet.cells[bi][fi] = cell;
      if (!cell.reached) continue;
      ++sheet.reached;
      if (first) {
        sheet.rate_lo = cell.per_period;
        sheet.rate_hi = cell.per_period;
        sheet.peak_lo = cell.peak_ratio;
        sheet.peak_hi = cell.peak_ratio;
        first = false;
      }
      sheet.rate_lo = std::min(sheet.rate_lo, cell.per_period);
      sheet.rate_hi = std::max(sheet.rate_hi, cell.per_period);
      sheet.peak_lo = std::min(sheet.peak_lo, cell.peak_ratio);
      sheet.peak_hi = std::max(sheet.peak_hi, cell.peak_ratio);
      // F* is the FIRST force to break down, scanning upward: the rate need not
      // be monotone in force for the window's lower edge to be the one Schelleng
      // names.
      if (sheet.f_star[bi] == kForceOffGrid && cell.per_period > kHelmholtzHi) {
        sheet.f_star[bi] = force_at(fi);
        ++sheet.f_star_defined;
      }
      if (cell.per_period > kHelmholtzHi) {
        if (held_helmholtz && sheet.f_up[bi] == kForceOffGrid) {
          sheet.f_up[bi] = force_at(fi);
          ++sheet.f_up_defined;
        }
      } else if (cell.per_period >= sonare::test::bowed::kHelmholtzLo) {
        held_helmholtz = true;
      }
      if (cell.peak_ratio >= kPlayedPitchHi) continue;
      ++sheet.played_cells;
      if (cell.per_period > kHelmholtzHi) {
        ++sheet.played_multi_cells;
        if (held_played_helmholtz && sheet.f_schelleng[bi] == kForceOffGrid) {
          sheet.f_schelleng[bi] = force_at(fi);
          ++sheet.f_schelleng_defined;
        }
      } else if (cell.per_period >= sonare::test::bowed::kHelmholtzLo) {
        held_played_helmholtz = true;
      }
    }
  }
  const double* lo = std::min_element(sheet.f_star, sheet.f_star + kBetaCount);
  const double* hi = std::max_element(sheet.f_star, sheet.f_star + kBetaCount);
  sheet.f_star_spread = *hi - *lo;
  sheet.f_up_spread = *std::max_element(sheet.f_up, sheet.f_up + kBetaCount) -
                      *std::min_element(sheet.f_up, sheet.f_up + kBetaCount);
  sheet.f_schelleng_spread = *std::max_element(sheet.f_schelleng, sheet.f_schelleng + kBetaCount) -
                             *std::min_element(sheet.f_schelleng, sheet.f_schelleng + kBetaCount);
  sheet.exists = sheet.f_star_defined > 0;
  sheet.moves = sheet.f_star_spread >= kForceMoveMin - 1e-9;
  sheet.predicate = sheet.exists && sheet.moves;
  return sheet;
}

/// Column header shared by the three tables.
void force_header(std::ostringstream& o) {
  o << "  beta \\ force";
  for (int fi = 0; fi < kForceCount; ++fi)
    o << std::setw(7) << std::setprecision(1) << force_at(fi);
  o << "\n";
}

/// The three per-cell readings, the two thresholds derived from them, and the
/// predicate. Printed rather than asserted: the predicate is a branch.
std::string format_sheet(const Sheet& sheet, const char* name) {
  std::ostringstream o;
  o << std::fixed;
  o << "\nsheet: " << name << " (polarization pinned to 0, violin note " << int{kSweepNote} << ")";
  o << "\n  slips per period   h = Helmholtz established, - = never, x = not reached\n";
  force_header(o);
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << "  " << std::setw(10) << std::setprecision(3) << kBetaGrid[bi] << "  ";
    for (int fi = 0; fi < kForceCount; ++fi) {
      const Cell& c = sheet.cells[bi][fi];
      o << std::setw(6) << std::setprecision(2) << c.per_period
        << (!c.reached ? 'x' : (c.helmholtz ? 'h' : '-'));
    }
    o << "\n";
  }
  o << "  steady level (dB)\n";
  force_header(o);
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << "  " << std::setw(10) << std::setprecision(3) << kBetaGrid[bi] << "  ";
    for (int fi = 0; fi < kForceCount; ++fi) {
      o << std::setw(7) << std::setprecision(1) << sheet.cells[bi][fi].level_db;
    }
    o << "\n";
  }
  o << "  spectral peak / played f0   1 = sounding the note as played\n";
  force_header(o);
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << "  " << std::setw(10) << std::setprecision(3) << kBetaGrid[bi] << "  ";
    for (int fi = 0; fi < kForceCount; ++fi) {
      o << std::setw(7) << std::setprecision(2) << sheet.cells[bi][fi].peak_ratio;
    }
    o << "\n";
  }
  o << "  F*(beta), P's threshold: the first force whose rate exceeds " << std::setprecision(1)
    << kHelmholtzHi << " (" << std::setprecision(1) << kForceOffGrid
    << " = none on the grid):\n   ";
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << " " << std::setprecision(2) << kBetaGrid[bi] << ":" << std::setprecision(1)
      << sheet.f_star[bi];
  }
  o << "\n  F_up(beta), the strict upper limit: the first force to break an ALREADY established"
       " Helmholtz regime:\n   ";
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << " " << std::setprecision(2) << kBetaGrid[bi] << ":" << std::setprecision(1)
      << sheet.f_up[bi];
  }
  o << "\n  F_schelleng(beta), multiple slip WITHIN the played period, entered from Helmholtz at"
       " the same pitch:\n   ";
  for (int bi = 0; bi < kBetaCount; ++bi) {
    o << " " << std::setprecision(2) << kBetaGrid[bi] << ":" << std::setprecision(1)
      << sheet.f_schelleng[bi];
  }
  o << "\n  cells sounding the played note " << sheet.played_cells << "/"
    << kBetaCount * kForceCount << ", of which multiple slip " << sheet.played_multi_cells
    << "; peak/f0 " << std::setprecision(2) << sheet.peak_lo << ".." << sheet.peak_hi;
  o << "\n  cells reached " << sheet.reached << "/" << kBetaCount * kForceCount << ", rate "
    << std::setprecision(3) << sheet.rate_lo << ".." << sheet.rate_hi << "\n  F* defined on "
    << sheet.f_star_defined << "/" << kBetaCount << " beta, spread " << std::setprecision(2)
    << sheet.f_star_spread << "; F_up defined on " << sheet.f_up_defined << "/" << kBetaCount
    << " beta, spread " << std::setprecision(2) << sheet.f_up_spread << "; F_schelleng defined on "
    << sheet.f_schelleng_defined << "/" << kBetaCount << " beta, spread " << std::setprecision(2)
    << sheet.f_schelleng_spread << "\n  P = exists(" << (sheet.exists ? "true" : "false")
    << ") AND moves(" << (sheet.moves ? "true" : "false")
    << ") = " << (sheet.predicate ? "TRUE" : "FALSE") << "\n";
  return o.str();
}

/// Asserts what the sweep OWES regardless of the predicate's truth value: every
/// cell reached, and a grid that is not one constant a predicate could not have
/// distinguished.
void check_sheet(const Sheet& sheet, const char* name) {
  const std::string report = format_sheet(sheet, name);
  {
    INFO(report);
    REQUIRE(sheet.reached == kBetaCount * kForceCount);
  }
  INFO(report);
  CHECK(sheet.rate_hi > sheet.rate_lo);
  INFO(report);
  CHECK(sheet.peak_hi > sheet.peak_lo);
}

/// A shipped bowed patch and the three notes it is read at. The notes are the
/// instrument's own range — bottom open string, middle, top of normal position
/// — because beta is a fraction of string length and a failure can be
/// register-dependent. The ensembles take the section's span rather than one
/// instrument's.
struct ShippedVoice {
  const char* name;
  uint16_t bank;
  uint8_t program;
  uint8_t notes[3];
};

const ShippedVoice kShipped[] = {
    {"violin", 0, 40, {55, 72, 88}},      // G3 open string .. E6
    {"viola", 0, 41, {48, 67, 84}},       // C3 open string .. C6
    {"cello", 0, 42, {36, 55, 72}},       // C2 open string .. C5
    {"contrabass", 0, 43, {28, 45, 55}},  // E1 sounding .. G3
    {"fiddle", 0, 110, {55, 72, 88}},     // the violin's range
    {"string_ensemble_1", 0, 48, {36, 60, 84}},
    {"string_ensemble_2", 0, 49, {36, 60, 84}},
    {"violin_slow", 8, 40, {55, 72, 88}},  // GS variation of the violin
};
constexpr int kShippedCount = 8;

/// Long enough that the trailing half the readings are taken over sits past the
/// slowest shipped bow ramp (viola's attack_ms is 857).
constexpr int kShippedSamples = 144000;

/// Bow-speed grid. The engine's bow velocity is kBowVelocityBase +
/// kBowVelocitySpan * speed, and `speed` is the ONLY thing the patch's
/// bow_speed, vel_to_speed and the note velocity reach — so sweeping speed
/// directly sweeps the whole bow-velocity axis and nothing else.
constexpr int kSpeedCount = 21;

double speed_at(int index) noexcept { return static_cast<double>(index) / 20.0; }

/// The effective speed a patch starts at, in the engine's own float arithmetic
/// (bowed_string_voice.cpp:118-120), so a render reparameterized onto it is
/// bit-identical rather than merely close.
float shipped_speed(const BowedStringPatchParams& p) noexcept {
  const float vel01 = static_cast<float>(kSweepVelocity & 0x7Fu) / 127.0f;
  const float vts = std::clamp(p.vel_to_speed, 0.0f, 1.0f);
  return std::clamp((1.0f - vts) * p.bow_speed + vts * vel01, 0.0f, 1.0f);
}

/// The same patch with the speed blend collapsed onto bow_speed. vel_to_speed
/// at its shipped value pins the blend into a narrow band around the note
/// velocity, so the axis cannot be swept through bow_speed alone; zeroing it is
/// an exact reparameterization rather than a second change, since velocity
/// reaches the engine through this blend and nowhere else.
BowedStringPatchParams at_speed(const BowedStringPatchParams& p, double speed) {
  BowedStringPatchParams out = p;
  out.vel_to_speed = 0.0f;
  out.bow_speed = static_cast<float>(speed);
  return out;
}

/// Sounding the note it was asked for: one slip per played period, Helmholtz
/// established, and the spectral peak on the played fundamental.
bool cell_works(const Cell& c) noexcept {
  return c.reached && c.helmholtz && c.per_period < kHelmholtzHi && c.peak_ratio < kPlayedPitchHi;
}

/// One shipped (patch, note) scanned over the bow-speed axis.
struct SpeedScan {
  const char* patch = nullptr;
  uint8_t note = 0;
  double f0 = 0.0;
  float beta = 0.0f;
  float force = 0.0f;
  float shipped = 0.0f;
  bool works[kSpeedCount] = {};
  double v_top = -1.0;
  double v_bot = -1.0;
  bool censored_high = false;
  bool censored_low = false;
  bool shipped_works = false;
  double shipped_rate = 0.0;
  double repar_rate = 0.0;
};

}  // namespace

TEST_CASE("the bow-force window sweep reads the patch it says it does",
          "[midi][synth][bowed][window]") {
  // Every field the sweep holds rather than drives. A moved patch value is told
  // apart from a moved engine here, the same way the control hashes do it.
  const BowedStringPatchParams& v = violin_params();
  CHECK(v.bow_speed == 0.630748f);
  CHECK(v.vel_to_speed == 0.312461f);  // was 0.6, the clamp default the field
                                       // inherited before the loop-loss re-fit
  CHECK(v.brightness == 0.228986f);    // was 0.47
  CHECK(v.damping == 0.0822536f);
  CHECK(v.attack_ms == 41.9837f);  // was 47.142
  CHECK(v.release_ms == 147.15f);  // was 165.23
  CHECK(v.rosin == 0.0875388f);    // was 0.1
  CHECK(v.stribeck == 0.7f);
  CHECK(v.sympathetic == 0.495379f);  // was 0.08
  // The onset mechanisms are still at their identity defaults, so this sweep
  // measures the engine as it stands rather than one already changed.
  CHECK(v.attack_noise == 0.0f);
  CHECK(v.bow_accel_ms == 0.0f);
  // Driven by the sweep, and pinned: recorded so the grid's own edges are read
  // against the shipped operating point rather than guessed.
  CHECK(v.bow_position == 0.169028f);  // was 0.17016
  CHECK(v.bow_force == 0.0643318f);
  CHECK(v.polarization == 0.15f);
  CHECK(v.elasto_plastic);
}

TEST_CASE("the bow-force window on the static friction table",
          "[midi][synth][bowed][window][.][slow]") {
  // The control sheet: the memoryless bow table, which no shipped bowed patch
  // runs on. Its threshold is the one Schelleng's law is stated for.
  check_sheet(sweep(false), "elasto_plastic off");
}

TEST_CASE("the bow-force window on the elasto-plastic friction",
          "[midi][synth][bowed][window][.][slow]") {
  // The shipped sheet: every bowed patch has elasto_plastic true, so this is
  // the branch a force law would have to be written against.
  check_sheet(sweep(true), "elasto_plastic on");
}

TEST_CASE("the bow-force window reading is not an artefact of the sympathetic halo",
          "[midi][synth][bowed][window][.][slow]") {
  // The halo is a one-way bank summed into the output, so it adds smooth energy
  // to the very waveform the slip detector thresholds against its own RMS. At
  // both ends of the force axis the multiple-slip verdict must not depend on it.
  for (int fi : {0, kForceCount - 1}) {
    BowedStringPatchParams params = violin_params();
    params.polarization = 0.0f;
    params.bow_position = 0.18f;
    params.bow_force = static_cast<float>(force_at(fi));
    const Cell shipped = measure_cell(params, kSweepNote, kSweepSamples);
    params.sympathetic = 0.0f;
    const Cell bare = measure_cell(params, kSweepNote, kSweepSamples);
    INFO("beta 0.18, force " << force_at(fi) << ": halo on " << shipped.per_period << " ("
                             << shipped.level_db << " dB), halo off " << bare.per_period << " ("
                             << bare.level_db << " dB)");
    REQUIRE(shipped.reached);
    REQUIRE(bare.reached);
    CHECK((shipped.per_period > kHelmholtzHi) == (bare.per_period > kHelmholtzHi));
  }
}

TEST_CASE("the shipped bowed patches at their own operating points",
          "[midi][synth][bowed][window][.][slow]") {
  // Every field exactly as shipped, polarization included: this asks what the
  // bank actually does, not what a clean sweep does. The control column repeats
  // the reading with polarization alone at 0, because elasto-plastic friction
  // and the second polarization have a known double-slip interaction in the low
  // register that would otherwise be read as a bow-parameter failure.
  std::ostringstream o;
  o << std::fixed;
  o << "\n  shipped patch readings (h = Helmholtz established, - = never)\n";
  o << "  patch                note   beta   force   rate  pk/f0   dB   |  rate(pol=0) pk/f0\n";
  int sounding = 0;
  int cells = 0;
  for (int vi = 0; vi < kShippedCount; ++vi) {
    const ShippedVoice& v = kShipped[vi];
    const BowedStringPatchParams& params =
        sonare::midi::synth::gm_fallback_patch(v.bank, v.program).bowed_string;
    for (int ni = 0; ni < 3; ++ni) {
      const Cell shipped = measure_cell(params, v.notes[ni], kShippedSamples);
      BowedStringPatchParams unpolarized = params;
      unpolarized.polarization = 0.0f;
      const Cell control = measure_cell(unpolarized, v.notes[ni], kShippedSamples);
      REQUIRE(shipped.reached);
      REQUIRE(control.reached);
      ++cells;
      // Sounding the note it was asked for: one slip per played period, and the
      // spectrum's peak on the played fundamental rather than a shorter period.
      if (shipped.helmholtz && shipped.per_period < kHelmholtzHi &&
          shipped.peak_ratio < kPlayedPitchHi) {
        ++sounding;
      }
      o << "  " << std::setw(18) << std::left << v.name << std::right << std::setw(6)
        << int{v.notes[ni]} << std::setw(7) << std::setprecision(3) << params.bow_position
        << std::setw(8) << std::setprecision(3) << params.bow_force << std::setw(7)
        << std::setprecision(2) << shipped.per_period << (shipped.helmholtz ? 'h' : '-')
        << std::setw(6) << std::setprecision(2) << shipped.peak_ratio << std::setw(7)
        << std::setprecision(1) << shipped.level_db << "  | " << std::setw(7)
        << std::setprecision(2) << control.per_period << (control.helmholtz ? 'h' : '-')
        << std::setw(7) << std::setprecision(2) << control.peak_ratio << "\n";
    }
  }
  o << "  cells sounding the played note with Helmholtz established: " << sounding << "/" << cells
    << "\n";
  const std::string report = o.str();
  INFO(report);
  CHECK(cells == kShippedCount * 3);
}

TEST_CASE("the bow-speed threshold of the shipped bowed patches",
          "[midi][synth][bowed][window][.][slow]") {
  // Bow velocity is injected per SAMPLE while the loop loss is per traversal, so
  // a longer period accumulates more drive against the same loss. If that is
  // what puts the low register into a higher mode, the largest bow speed a note
  // survives must fall as its period rises — and a note that currently works
  // must break when the speed is raised onto the same boundary.
  std::vector<SpeedScan> scans;
  for (int vi = 0; vi < kShippedCount; ++vi) {
    const ShippedVoice& v = kShipped[vi];
    const BowedStringPatchParams& params =
        sonare::midi::synth::gm_fallback_patch(v.bank, v.program).bowed_string;
    for (int ni = 0; ni < 3; ++ni) {
      SpeedScan scan;
      scan.patch = v.name;
      scan.note = v.notes[ni];
      scan.f0 = static_cast<double>(sonare::midi::synth::note_to_hz(v.notes[ni]));
      scan.beta = params.bow_position;
      scan.force = params.bow_force;
      scan.shipped = shipped_speed(params);
      // Positive control for the reparameterization: the shipped patch and the
      // same patch re-expressed on the speed axis must render the same note.
      const Cell direct = measure_cell(params, scan.note, kShippedSamples);
      const Cell repar = measure_cell(at_speed(params, scan.shipped), scan.note, kShippedSamples);
      scan.shipped_rate = direct.per_period;
      scan.repar_rate = repar.per_period;
      scan.shipped_works = cell_works(direct);
      for (int si = 0; si < kSpeedCount; ++si) {
        const Cell cell = measure_cell(at_speed(params, speed_at(si)), scan.note, kShippedSamples);
        REQUIRE(cell.reached);
        scan.works[si] = cell_works(cell);
        if (scan.works[si]) {
          scan.v_top = speed_at(si);
          if (scan.v_bot < 0.0) scan.v_bot = speed_at(si);
        }
      }
      scan.censored_high = scan.works[kSpeedCount - 1];
      scan.censored_low = scan.works[0];
      scans.push_back(scan);
    }
  }

  std::ostringstream o;
  o << std::fixed;
  o << "\n  bow-speed scan, 0.00 .. 1.00 in 0.05 steps.  # = sounds the played note with"
       "\n  Helmholtz, . = does not; the shipped speed is @ (working) or o (not).\n";
  for (const SpeedScan& scan : scans) {
    std::string pattern(kSpeedCount, '.');
    for (int si = 0; si < kSpeedCount; ++si) {
      pattern[static_cast<std::size_t>(si)] = scan.works[si] ? '#' : '.';
    }
    const std::size_t at = static_cast<std::size_t>(
        std::min(kSpeedCount - 1, static_cast<int>(std::lround(scan.shipped * 20.0))));
    pattern[at] = scan.works[at] ? '@' : 'o';
    o << "  " << std::setw(18) << std::left << scan.patch << std::right << " n" << std::setw(3)
      << int{scan.note} << "  f0" << std::setw(7) << std::setprecision(1) << scan.f0 << "  b"
      << std::setprecision(3) << scan.beta << "  F" << std::setprecision(3) << scan.force << "\n";
    o << "    " << pattern << "  v_bot ";
    if (scan.v_bot < 0.0) {
      o << "none ";
    } else {
      o << std::setprecision(2) << scan.v_bot << (scan.censored_low ? "-" : " ");
    }
    o << " v_top ";
    if (scan.v_top < 0.0) {
      o << "none";
    } else {
      o << std::setprecision(2) << scan.v_top << (scan.censored_high ? "+" : "");
    }
    o << "\n";
  }

  // Two fits on the same scans. A threshold at a grid edge is a bound rather
  // than a value, so an edge-censored cell is held out of its own fit instead of
  // entering it as the edge — which would drag the exponent toward zero.
  struct Fit {
    double k = 0.0;
    double scatter = 0.0;
    std::size_t n = 0;
    int censored = 0;
    int absent = 0;
  };
  auto fit_threshold = [&scans](bool bottom) {
    Fit f;
    std::vector<double> lx;
    std::vector<double> ly;
    for (const SpeedScan& scan : scans) {
      const double v = bottom ? scan.v_bot : scan.v_top;
      const bool edge = bottom ? scan.censored_low : scan.censored_high;
      if (v < 0.0) {
        ++f.absent;
        continue;
      }
      if (edge || !(v > 0.0)) {
        ++f.censored;
        continue;
      }
      lx.push_back(std::log10(scan.f0));
      ly.push_back(std::log10(v));
    }
    f.n = lx.size();
    if (f.n < 2) return f;
    double mx = 0.0;
    double my = 0.0;
    for (std::size_t i = 0; i < f.n; ++i) {
      mx += lx[i];
      my += ly[i];
    }
    mx /= static_cast<double>(f.n);
    my /= static_cast<double>(f.n);
    double num = 0.0;
    double den = 0.0;
    for (std::size_t i = 0; i < f.n; ++i) {
      num += (lx[i] - mx) * (ly[i] - my);
      den += (lx[i] - mx) * (lx[i] - mx);
    }
    if (!(den > 0.0)) return f;
    f.k = num / den;
    const double c = my - f.k * mx;
    double acc = 0.0;
    for (std::size_t i = 0; i < f.n; ++i) {
      const double r = ly[i] - (f.k * lx[i] + c);
      acc += r * r;
    }
    f.scatter = std::sqrt(acc / static_cast<double>(f.n));
    return f;
  };
  const Fit top = fit_threshold(false);
  const Fit bot = fit_threshold(true);
  o << "  v_top ~ f0^k (the ceiling the per-period energy argument predicts):\n    k = "
    << std::setprecision(3) << top.k << ", scatter " << std::setprecision(3) << top.scatter
    << " decades (x" << std::setprecision(2) << std::pow(10.0, top.scatter) << ") over " << top.n
    << " cells; held out " << top.censored << " censored, " << top.absent << " with no working"
    << " speed\n";
  o << "  v_bot ~ f0^k (the floor the scan actually shows):\n    k = " << std::setprecision(3)
    << bot.k << ", scatter " << std::setprecision(3) << bot.scatter << " decades (x"
    << std::setprecision(2) << std::pow(10.0, bot.scatter) << ") over " << bot.n
    << " cells; held out " << bot.censored << " censored, " << bot.absent << " with no working"
    << " speed\n";

  // Same pitch, different patch: if the threshold is pitch alone, these agree.
  for (int note : {36, 55}) {
    o << "  note " << note << ":\n";
    for (const SpeedScan& scan : scans) {
      if (scan.note != note) continue;
      o << "    " << std::setw(18) << std::left << scan.patch << std::right << " beta "
        << std::setprecision(3) << scan.beta << "  force " << std::setprecision(3) << scan.force
        << "  v_bot ";
      if (scan.v_bot < 0.0) {
        o << "none";
      } else {
        o << std::setprecision(2) << scan.v_bot << (scan.censored_low ? "-" : "");
      }
      o << "\n";
    }
  }

  const std::string report = o.str();
  {
    INFO(report);
    REQUIRE(scans.size() == static_cast<std::size_t>(kShippedCount * 3));
  }
  // The reparameterization is exact, so the two renders of the shipped point are
  // the same render; a difference here invalidates every scan above it.
  for (const SpeedScan& scan : scans) {
    INFO(report);
    INFO(scan.patch << " note " << int{scan.note});
    CHECK(scan.shipped_rate == scan.repar_rate);
  }
  INFO(report);
  // Sensitivity: a scan whose every cell answers the same way measured nothing.
  int moved = 0;
  for (const SpeedScan& scan : scans) {
    if (scan.works[0] != scan.works[kSpeedCount - 1]) ++moved;
  }
  INFO("scans whose verdict differs between the bottom and top of the speed axis: " << moved);
  CHECK(moved > 0);
}
