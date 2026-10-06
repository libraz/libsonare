/// @file native_synth_live_params_test.cpp
/// @brief NativeSynth automation reaching sounding voices: every automatable
///        parameter moved on a held note must sound like a note struck at the
///        new value, while an unmoved parameter, a GM voice and a choked voice
///        stay bit-identical and the audio path stays allocation-free.
///
/// Equivalence oracle (SC1/SC3): a note held at A, moved to B, is compared over
/// one window with a note struck at B (reference) and a note left at A
/// (control). Tolerances: spectrum 1.0 dB RMS over bins within 60 dB of either
/// spectrum's peak, period / deviation 5 %, time constant 10 %, level and L/R
/// ratio 0.5 dB. The control must sit at least 3x the tolerance away. Envelope
/// times are read relative to a carrier run with the envelope taken out.
///
/// Pairwise model (coverwise, strength 2, seed 20261005):
///   param      {detune, drive, keyTrack, velToCutoff, bodyMix, hpCutoff,
///               converter, stereoSpread, lfoRate, lfo2Rate, drift, envTime,
///               sustain, glide}
///   transition {off_on, on_off, on_on}   stage {attack, decay, sustain, release}
///   unison {1, 3}                        engine {subtractive, modal}
///   IF transition != on_on THEN param IN {hpCutoff, converter, drive, bodyMix}
///   IF param = detune THEN engine = subtractive AND unison = 3
///   IF stage != sustain THEN param IN {envTime, sustain}
///   IF param = envTime THEN stage != sustain
///   IF param = sustain THEN stage IN {decay, sustain}
///   IF engine = modal THEN unison = 1      (osc-less engines ignore unison)
/// 39 rows, listed in kPairwiseRows. envTime expands to the amp and filter time
/// of its stage, sustain to amp and filter sustain, converter to sampleHoldHz
/// and bitDepth, so all 21 note-on-latched ids are reached. One row sits
/// outside the model: lfoRate A -> 0. The modal engine is a single long-ringing
/// mode (a sine); its glide runs on a mono retrigger, since it declines legato.
///
/// A / B values:
///   detuneCents 25 -> 100 (saw, note 81)
///   keyTrack 0.25 -> 1 (note 96; cutoff 2 kHz, modal 500 Hz)
///   velToCutoffCents 600 -> 3600 (vel 40, 2 kHz; note 48, modal note 91)
///   drive  off_on 0 -> 0.6   on_off 0.6 -> 0   on_on 0.2 -> 0.8 (saw)
///   bodyMix off_on 0 -> 0.8  on_off 0.8 -> 0   on_on 0.3 -> 1 (wood tube, sine)
///   hpCutoffHz off_on 0 -> 800  on_off 800 -> 0  on_on 100 -> 800 (note 36)
///   sampleHoldHz off_on 0 -> 3000  on_off 3000 -> 0  on_on 1500 -> 6000 (sine)
///   bitDepth off_on 0 -> 3  on_off 3 -> 0  on_on 2 -> 6 (sine)
///   stereoSpread 0.2 -> 1           lfoRateHz / lfo2RateHz 5.859375 -> 11.71875
///   driftCents 10 -> 30 (rate 40 Hz, note 84)        glideMs 400 -> 100
///   envelope times 400 -> 200 ms (attack), 600 -> 300 ms (decay, release)
///   sustain 0.5 -> 0.25

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "midi/articulation_mode.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::ArticulationMode;
using sonare::midi::synth::BodyType;
using sonare::midi::synth::ModDestination;
using sonare::midi::synth::ModSource;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthParamId;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::SynthRetrigger;
using sonare::midi::synth::VaWaveform;
using sonare::test::event;
using P = NativeSynthParamId;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr size_t kSettle = 8192;
constexpr size_t kWindow = 2 * 8192;

// Tolerances (K20) and the control contrast factor.
constexpr double kSpectrumTolDb = 1.0;
constexpr double kRelTol = 0.05;
constexpr double kTauTol = 0.10;
constexpr double kLevelTolDb = 0.5;
constexpr double kContrast = 3.0;

// Integer LFO periods in an 8192-sample span: k * 48000 / 8192 Hz.
constexpr float kLfoA = 5.859375f;
constexpr float kLfoB = 11.71875f;
constexpr float kLfoDepthCents = 50.0f;
// DahdsrEnvelope's attack aims above full scale and stops at 1.0.
constexpr double kAttackTarget = 1.3;

unsigned pid(P p) { return static_cast<unsigned>(p); }

struct Stereo {
  std::vector<float> l;
  std::vector<float> r;
};

void render(NativeSynth& s, Stereo& out, size_t n, int block = kBlock) {
  const size_t base = out.l.size();
  out.l.resize(base + n, 0.0f);
  out.r.resize(base + n, 0.0f);
  for (size_t done = 0; done < n; done += static_cast<size_t>(block)) {
    const int m = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - done));
    float* ch[2] = {out.l.data() + base + done, out.r.data() + base + done};
    s.process(ch, 2, m);
  }
}

void note_on(NativeSynth& s, uint8_t note, uint8_t vel = 100) {
  s.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, vel)));
}

void note_off(NativeSynth& s, uint8_t note) {
  s.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, note, 0)));
}

// --- measurement -----------------------------------------------------------

/// Mean Hann-4096 magnitude spectrum over [from, from + len), hop 1024.
std::vector<double> avg_spectrum(const std::vector<float>& x, size_t from, size_t len) {
  constexpr int kFft = 4096;
  constexpr size_t kHop = 1024;
  sonare::FFT fft(kFft);
  std::vector<float> frame(kFft);
  std::vector<std::complex<float>> spec(static_cast<size_t>(fft.n_bins()));
  std::vector<double> acc(static_cast<size_t>(fft.n_bins()), 0.0);
  int frames = 0;
  for (size_t start = from; start + kFft <= from + len && start + kFft <= x.size(); start += kHop) {
    for (int i = 0; i < kFft; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (kFft - 1));
      frame[static_cast<size_t>(i)] = x[start + static_cast<size_t>(i)] * static_cast<float>(w);
    }
    fft.forward(frame.data(), spec.data());
    for (size_t b = 0; b < acc.size(); ++b) acc[b] += std::abs(spec[b]);
    ++frames;
  }
  for (double& v : acc) v /= std::max(frames, 1);
  return acc;
}

/// RMS dB difference over the bins within 60 dB of either spectrum's peak, so
/// content present on one side only (images, harmonics) is counted.
double spectral_diff_db(const std::vector<double>& ref, const std::vector<double>& other) {
  const double ref_floor = *std::max_element(ref.begin(), ref.end()) * 1.0e-3;
  const double other_floor = *std::max_element(other.begin(), other.end()) * 1.0e-3;
  double sum = 0.0;
  size_t count = 0;
  for (size_t b = 0; b < ref.size(); ++b) {
    if (ref[b] < ref_floor && other[b] < other_floor) continue;
    const double d = 20.0 * std::log10(std::max(other[b], 1.0e-12) / std::max(ref[b], 1.0e-12));
    sum += d * d;
    ++count;
  }
  return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

double rms(const std::vector<float>& x, size_t from, size_t len) {
  double sum = 0.0;
  const size_t end = std::min(x.size(), from + len);
  for (size_t i = from; i < end; ++i) sum += static_cast<double>(x[i]) * x[i];
  return end > from ? std::sqrt(sum / static_cast<double>(end - from)) : 0.0;
}

double db(double v) { return 20.0 * std::log10(std::max(v, 1.0e-30)); }

/// Per-frame sine amplitude from the Teager-Kaiser energy (proportional to A).
struct Frames {
  std::vector<double> t;  // frame centre, samples relative to `from`
  std::vector<double> a;
};

Frames tkeo_frames(const std::vector<float>& x, size_t from, size_t len, size_t frame = 64) {
  Frames out;
  for (size_t s = std::max<size_t>(from, 1); s + frame + 1 <= std::min(x.size(), from + len);
       s += frame) {
    double e = 0.0;
    for (size_t i = s; i < s + frame; ++i) {
      e += static_cast<double>(x[i]) * x[i] - static_cast<double>(x[i - 1]) * x[i + 1];
    }
    out.t.push_back(static_cast<double>(s - from) + 0.5 * static_cast<double>(frame));
    out.a.push_back(std::sqrt(std::max(e / static_cast<double>(frame), 0.0)));
  }
  return out;
}

/// Time constant (samples) of a least-squares line through (t, ln y).
double fit_tau(const std::vector<double>& t, const std::vector<double>& y) {
  if (t.size() < 4) return 0.0;
  double st = 0.0, sy = 0.0, stt = 0.0, sty = 0.0;
  const double n = static_cast<double>(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const double ly = std::log(y[i]);
    st += t[i];
    sy += ly;
    stt += t[i] * t[i];
    sty += t[i] * ly;
  }
  const double slope = (n * sty - st * sy) / (n * stt - st * st);
  return slope < 0.0 ? -1.0 / slope : 0.0;
}

/// One period per upward zero crossing: time (samples from `from`) and Hz.
struct Cycles {
  std::vector<double> t;
  std::vector<double> hz;
};

Cycles cycles(const std::vector<float>& x, size_t from, size_t len) {
  std::vector<double> cross;
  const size_t end = std::min(x.size(), from + len);
  for (size_t i = std::max<size_t>(from, 1); i < end; ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double d = static_cast<double>(x[i]) - x[i - 1];
      cross.push_back(static_cast<double>(i - 1 - from) + (d != 0.0 ? -x[i - 1] / d : 0.0));
    }
  }
  Cycles out;
  for (size_t i = 1; i < cross.size(); ++i) {
    out.t.push_back(0.5 * (cross[i] + cross[i - 1]));
    out.hz.push_back(kRate / (cross[i] - cross[i - 1]));
  }
  return out;
}

std::vector<double> to_cents(const std::vector<double>& hz, double ref_hz) {
  std::vector<double> c(hz.size());
  for (size_t i = 0; i < hz.size(); ++i) c[i] = 1200.0 * std::log2(hz[i] / ref_hz);
  return c;
}

double mean(const std::vector<double>& v) {
  double s = 0.0;
  for (double x : v) s += x;
  return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

/// Half the peak-to-peak pitch excursion over the window (cents).
double pitch_deviation(const std::vector<float>& x, size_t from, size_t len) {
  const Cycles c = cycles(x, from, len);
  if (c.hz.empty()) return 0.0;
  const std::vector<double> cents = to_cents(c.hz, mean(c.hz));
  const auto [lo, hi] = std::minmax_element(cents.begin(), cents.end());
  return 0.5 * (*hi - *lo);
}

/// Period (samples) of the pitch modulation: twice the mean spacing of the
/// hysteresis-gated crossings of the cents series through its mean.
double modulation_period(const std::vector<float>& x, size_t from, size_t len) {
  const Cycles c = cycles(x, from, len);
  if (c.hz.size() < 8) return 0.0;
  std::vector<double> cents = to_cents(c.hz, mean(c.hz));
  const double m = mean(cents);
  for (double& v : cents) v -= m;
  const auto [lo, hi] = std::minmax_element(cents.begin(), cents.end());
  const double h = 0.25 * 0.5 * (*hi - *lo);
  std::vector<double> crossings;
  int side = 0;  // -1 below -h, +1 above +h
  for (size_t i = 1; i < cents.size(); ++i) {
    const int now = cents[i] > h ? 1 : (cents[i] < -h ? -1 : 0);
    if (now == 0) continue;
    if (side != 0 && now != side) {
      // Interpolate the zero crossing inside the run that just ended.
      size_t k = i;
      while (k > 1 && (cents[k - 1] > 0.0) == (cents[i] > 0.0)) --k;
      const double f = -cents[k - 1] / (cents[k] - cents[k - 1]);
      crossings.push_back(c.t[k - 1] + f * (c.t[k] - c.t[k - 1]));
    }
    side = now;
  }
  if (crossings.size() < 2) return 0.0;
  return 2.0 * (crossings.back() - crossings.front()) / static_cast<double>(crossings.size() - 1);
}

// --- scenarios ---------------------------------------------------------------

enum class Engine { kSubtractive, kModal };

enum class Obs {
  kSpectrum,
  kPanRatio,
  kLevel,
  kLfoPeriod,
  kLfoStop,
  kPitchDeviation,
  kAmpTau,
  kFilterTau,
  kGlideTau,
};

/// Where the change lands and which windows are compared.
enum class Timeline { kSustain, kInDecay, kAttack, kDecay, kRelease, kGlide };

struct Scenario {
  std::string name;
  NativeSynthConfig cfg;
  P param = P::kDrive;
  float a = 0.0f;
  float b = 0.0f;
  std::vector<uint8_t> notes{69};
  uint8_t velocity = 100;
  Obs obs = Obs::kSpectrum;
  Timeline tl = Timeline::kSustain;
  bool modal_glide = false;
};

NativeSynthConfig base_config(Engine engine, int unison, VaWaveform wave) {
  NativeSynthConfig cfg;
  auto& p = cfg.patch;
  p.retrigger = SynthRetrigger::kNote;
  p.waveform = wave;
  p.unison = unison;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.decay_ms = 1.0f;
  p.amp_env.sustain = 1.0f;
  p.filter_env.attack_ms = 1.0f;
  p.filter_env.decay_ms = 1.0f;
  p.filter_env.sustain = 1.0f;
  if (engine == Engine::kModal) {
    // One undamped-in-practice mode: a sine the measurements can read.
    p.mode = SynthEngineMode::kModal;
    p.modal.num_modes = 1;
    p.modal.modes[0] = {1.0f, 1.0f, 1.0f};
    p.modal.decay_s = 1000.0f;
    p.modal.decay_stretch = 0.0f;
    p.modal.release_damp_s = 1000.0f;
  }
  return cfg;
}

struct Runs {
  Stereo held, ctrl, ref;
  size_t held_from = 0;
  size_t ref_from = 0;
  int voices_before = 0;
  int voices_after = 0;
};

/// Plays @p sc on one synth; @p initial set before the first note, @p change
/// (when non-null) applied at the scenario's change point.
size_t play(const Scenario& sc, NativeSynth& s, Stereo& out, float initial, const float* change,
            int* voices_before = nullptr, int* voices_after = nullptr) {
  s.prepare(kRate, kBlock);
  REQUIRE(s.apply_parameter(pid(sc.param), initial));
  if (sc.tl == Timeline::kGlide) {
    s.set_articulation(
        0, sc.modal_glide ? ArticulationMode::kMonoRetrigger : ArticulationMode::kMonoLegato);
  }
  auto apply_change = [&] {
    if (voices_before != nullptr) *voices_before = s.active_voice_count();
    if (change != nullptr) REQUIRE(s.apply_parameter(pid(sc.param), *change));
    render(s, out, kBlock);
    if (voices_after != nullptr) *voices_after = s.active_voice_count();
  };
  if (sc.tl == Timeline::kGlide) {
    note_on(s, 57, sc.velocity);
    render(s, out, kSettle);
    note_on(s, 69, sc.velocity);
    render(s, out, 2560);
    const size_t at = out.l.size();
    apply_change();
    render(s, out, kWindow - kBlock);
    return at;
  }
  for (uint8_t n : sc.notes) note_on(s, n, sc.velocity);
  size_t at = 0;
  switch (sc.tl) {
    case Timeline::kSustain:
      render(s, out, kSettle + kWindow);
      at = out.l.size();
      apply_change();
      render(s, out, kSettle + kWindow - kBlock);
      return at + kSettle;
    case Timeline::kInDecay:
      render(s, out, 2560);
      at = out.l.size();
      apply_change();
      render(s, out, 48128 + kWindow - kBlock);
      return at + 48128;
    case Timeline::kAttack:
    case Timeline::kDecay:
      render(s, out, 4864);
      at = out.l.size();
      apply_change();
      render(s, out, 49152 - 4864 - kBlock);
      return at;
    case Timeline::kRelease:
      render(s, out, kSettle);
      for (uint8_t n : sc.notes) note_off(s, n);
      render(s, out, 4864);
      at = out.l.size();
      apply_change();
      render(s, out, kWindow - kBlock);
      return at;
    case Timeline::kGlide:
      break;
  }
  return at;
}

Runs run_scenario(const Scenario& sc) {
  Runs r;
  NativeSynth held(sc.cfg);
  NativeSynth ctrl(sc.cfg);
  NativeSynth ref(sc.cfg);
  r.held_from = play(sc, held, r.held, sc.a, &sc.b, &r.voices_before, &r.voices_after);
  play(sc, ctrl, r.ctrl, sc.a, nullptr);
  const size_t ref_at = play(sc, ref, r.ref, sc.b, nullptr);
  switch (sc.tl) {
    case Timeline::kSustain:
    case Timeline::kInDecay:
      r.ref_from = ref_at;  // same absolute window
      break;
    case Timeline::kAttack:
      r.ref_from = 0;
      break;
    case Timeline::kDecay:
      r.ref_from = kBlock;  // past the 1 ms attack
      break;
    case Timeline::kRelease:
      r.ref_from = kSettle;
      break;
    case Timeline::kGlide:
      r.ref_from = kSettle;
      break;
  }
  return r;
}

/// Steady amplitude of a sine at full envelope level (end of a run).
double tail_amplitude(const std::vector<float>& x) {
  const Frames f = tkeo_frames(x, x.size() - 8192, 8192 - 2);
  return mean(f.a);
}

bool is_filter_env_param(P p) {
  return p == P::kFilterAttackMs || p == P::kFilterDecayMs || p == P::kFilterReleaseMs;
}

/// The same timeline with the envelope under test taken out of the signal, so
/// what the carrier does on its own (modal damping, the amp release under a
/// filter window) divides out: the cutoff sweep removed for the filter
/// envelope; for the amp envelope an instant attack, a full sustain, or the
/// longest release.
Stereo carrier_run(const Scenario& sc) {
  Scenario flat = sc;
  float initial = sc.a;
  switch (sc.param) {
    case P::kAmpAttackMs:
      initial = 0.0f;
      break;
    case P::kAmpDecayMs:
      flat.cfg.patch.amp_env.sustain = 1.0f;
      break;
    case P::kAmpReleaseMs:
      initial = 20000.0f;
      break;
    default:
      flat.cfg.patch.env_to_cutoff_cents = 0.0f;
      break;
  }
  NativeSynth s(flat.cfg);
  Stereo out;
  play(flat, s, out, initial, nullptr);
  return out;
}

/// Envelope time constant (samples) read from a window starting at a stage
/// event, relative to @p carrier: the amplitude ratio for the amp envelope;
/// for the filter envelope the dB gap, which is linear in the envelope (the
/// sine sits 4+ octaves above the cutoff). @p full_scale is the reading at
/// envelope level 1.
double envelope_tau(const Scenario& sc, const std::vector<float>& x, size_t from, double full_scale,
                    const std::vector<float>& carrier) {
  const Frames f = tkeo_frames(x, from, kWindow);
  const Frames g = tkeo_frames(carrier, from, kWindow);
  const bool filter = is_filter_env_param(sc.param);
  const bool attack = sc.param == P::kAmpAttackMs || sc.param == P::kFilterAttackMs;
  std::vector<double> t, y;
  const double first = f.a.size() > 1 ? f.a[1] / g.a[1] : 0.0;
  for (size_t j = 1; j < f.a.size() && j < g.a.size(); ++j) {
    const double z = filter ? db(f.a[j]) - db(g.a[j]) : f.a[j] / g.a[j];
    if (attack) {
      const double e = z / full_scale;
      if (e >= 0.95) break;
      t.push_back(f.t[j]);
      y.push_back(kAttackTarget - e);
    } else {
      if (z < (filter ? 0.5 : 0.02 * first)) break;
      t.push_back(f.t[j]);
      y.push_back(z);
    }
  }
  return fit_tau(t, y);
}

/// Time constant (samples) of the remaining glide offset.
double glide_tau(const std::vector<float>& x, size_t from, double target_hz, size_t skip) {
  const Cycles c = cycles(x, from, kWindow);
  std::vector<double> t, y;
  for (size_t i = 0; i < c.hz.size(); ++i) {
    if (c.t[i] < static_cast<double>(skip)) continue;
    const double dev = std::fabs(1200.0 * std::log2(c.hz[i] / target_hz));
    if (dev > 900.0) continue;
    if (dev < 3.0) break;
    t.push_back(c.t[i]);
    y.push_back(dev);
  }
  return fit_tau(t, y);
}

struct Verdict {
  double held_ref = 0.0;
  double ctrl_ref = 0.0;
  double held_ctrl = 0.0;
  double tol = 0.0;
  // Raw readings for scalar observables (held, control, reference).
  double h = 0.0, c = 0.0, f = 0.0;
};

/// A relative difference against the reference value.
double rel(double v, double ref) { return ref != 0.0 ? std::fabs(v - ref) / std::fabs(ref) : 1e9; }

Verdict evaluate(const Scenario& sc, const Runs& r) {
  Verdict v;
  const size_t hf = r.held_from;
  const size_t rf = r.ref_from;
  switch (sc.obs) {
    case Obs::kSpectrum: {
      const auto h = avg_spectrum(r.held.l, hf, kWindow);
      const auto c = avg_spectrum(r.ctrl.l, hf, kWindow);
      const auto f = avg_spectrum(r.ref.l, rf, kWindow);
      v = {spectral_diff_db(f, h), spectral_diff_db(f, c), spectral_diff_db(c, h), kSpectrumTolDb};
      break;
    }
    case Obs::kPanRatio: {
      auto lr = [](const Stereo& s, size_t from) {
        return db(rms(s.l, from, kWindow)) - db(rms(s.r, from, kWindow));
      };
      const double h = lr(r.held, hf), c = lr(r.ctrl, hf), f = lr(r.ref, rf);
      v = {std::fabs(h - f), std::fabs(c - f), std::fabs(h - c), kLevelTolDb, h, c, f};
      break;
    }
    case Obs::kLevel: {
      const double h = db(rms(r.held.l, hf, kWindow)), c = db(rms(r.ctrl.l, hf, kWindow)),
                   f = db(rms(r.ref.l, rf, kWindow));
      v = {std::fabs(h - f), std::fabs(c - f), std::fabs(h - c), kLevelTolDb, h, c, f};
      break;
    }
    case Obs::kLfoPeriod: {
      const double h = modulation_period(r.held.l, hf, kWindow),
                   c = modulation_period(r.ctrl.l, hf, kWindow),
                   f = modulation_period(r.ref.l, rf, kWindow);
      v = {rel(h, f), rel(c, f), rel(h, c), kRelTol, h, c, f};
      break;
    }
    case Obs::kLfoStop: {
      // A stopped LFO has no period; compare the excursion against the depth.
      const double h = pitch_deviation(r.held.l, hf, kWindow),
                   c = pitch_deviation(r.ctrl.l, hf, kWindow),
                   f = pitch_deviation(r.ref.l, rf, kWindow);
      v = {std::fabs(h - f) / kLfoDepthCents, std::fabs(c - f) / kLfoDepthCents,
           std::fabs(h - c) / kLfoDepthCents, kRelTol};
      break;
    }
    case Obs::kPitchDeviation: {
      const double h = pitch_deviation(r.held.l, hf, kWindow),
                   c = pitch_deviation(r.ctrl.l, hf, kWindow),
                   f = pitch_deviation(r.ref.l, rf, kWindow);
      v = {rel(h, f), rel(c, f), rel(h, c), kRelTol, h, c, f};
      REQUIRE(f > 5.0);  // the seeded drift depth is not near zero for this note
      break;
    }
    case Obs::kAmpTau:
    case Obs::kFilterTau: {
      const Stereo carrier = carrier_run(sc);
      // Amp: the ratio is 1 at full level. Filter: the dB gap at level 1.
      const double full = sc.obs == Obs::kAmpTau
                              ? 1.0
                              : db(tail_amplitude(r.ref.l)) - db(tail_amplitude(carrier.l));
      const double h = envelope_tau(sc, r.held.l, hf, full, carrier.l);
      const double c = envelope_tau(sc, r.ctrl.l, hf, full, carrier.l);
      const double f = envelope_tau(sc, r.ref.l, rf, full, carrier.l);
      REQUIRE(f > 0.0);
      v = {rel(h, f), rel(c, f), rel(h, c), kTauTol, h, c, f};
      break;
    }
    case Obs::kGlideTau: {
      const Cycles tail = cycles(r.ref.l, r.ref.l.size() - 4096, 4096);
      const double target = mean(tail.hz);
      const size_t skip = 768;  // past a choked predecessor's 5 ms fade
      const double h = glide_tau(r.held.l, hf, target, 0);
      const double c = glide_tau(r.ctrl.l, hf, target, 0);
      const double f = glide_tau(r.ref.l, rf, target, skip);
      REQUIRE(f > 0.0);
      v = {rel(h, f), rel(c, f), rel(h, c), kTauTol, h, c, f};
      break;
    }
  }
  return v;
}

/// The equivalence oracle: preconditions REQUIRE'd, the claim CHECK'd.
void check_equivalence(const Scenario& sc) {
  const Runs r = run_scenario(sc);
  const Verdict v = evaluate(sc, r);
  INFO(sc.name << ": held-vs-ref " << v.held_ref << ", ctrl-vs-ref " << v.ctrl_ref
               << ", held-vs-ctrl " << v.held_ctrl << ", tolerance " << v.tol << " [h " << v.h
               << " c " << v.c << " f " << v.f << "]");
  // A/B are wide enough for the oracle to tell them apart (K20 validity).
  REQUIRE(v.ctrl_ref >= kContrast * v.tol);
  CHECK(v.held_ref <= v.tol);
  CHECK(v.held_ctrl >= kContrast * v.tol);
  CHECK(r.voices_after == r.voices_before);
}

// --- scenario construction ------------------------------------------------

enum class Row {
  kDetune,
  kDrive,
  kKeyTrack,
  kVelToCutoff,
  kBodyMix,
  kHpCutoff,
  kConverter,
  kStereoSpread,
  kLfoRate,
  kLfo2Rate,
  kDrift,
  kEnvTime,
  kSustain,
  kGlide,
};
enum class Transition { kOffOn, kOnOff, kOnOn };
enum class Stage { kAttack, kDecay, kSustain, kRelease };

struct PairwiseRow {
  Row param;
  Transition transition;
  Stage stage;
  int unison;
  Engine engine;
};

using E = Engine;
using R = Row;
using S = Stage;
using T = Transition;

constexpr std::array<PairwiseRow, 39> kPairwiseRows{{
    {R::kSustain, T::kOnOn, S::kDecay, 1, E::kSubtractive},
    {R::kEnvTime, T::kOnOn, S::kDecay, 1, E::kModal},
    {R::kDrive, T::kOffOn, S::kSustain, 3, E::kSubtractive},
    {R::kConverter, T::kOnOff, S::kSustain, 1, E::kModal},
    {R::kEnvTime, T::kOnOn, S::kRelease, 3, E::kSubtractive},
    {R::kHpCutoff, T::kOnOff, S::kSustain, 3, E::kSubtractive},
    {R::kBodyMix, T::kOnOff, S::kSustain, 1, E::kSubtractive},
    {R::kBodyMix, T::kOffOn, S::kSustain, 1, E::kModal},
    {R::kLfoRate, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kConverter, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kDrive, T::kOnOff, S::kSustain, 1, E::kModal},
    {R::kKeyTrack, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kEnvTime, T::kOnOn, S::kAttack, 1, E::kSubtractive},
    {R::kEnvTime, T::kOnOn, S::kRelease, 1, E::kModal},
    {R::kHpCutoff, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kVelToCutoff, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kStereoSpread, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kBodyMix, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kLfoRate, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kConverter, T::kOffOn, S::kSustain, 1, E::kModal},
    {R::kGlide, T::kOnOn, S::kSustain, 1, E::kSubtractive},
    {R::kDetune, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kDrift, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kBodyMix, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kHpCutoff, T::kOffOn, S::kSustain, 1, E::kSubtractive},
    {R::kEnvTime, T::kOnOn, S::kAttack, 1, E::kModal},
    {R::kSustain, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kLfo2Rate, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kDrive, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kVelToCutoff, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kLfo2Rate, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kDrift, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kGlide, T::kOnOn, S::kSustain, 3, E::kSubtractive},
    {R::kSustain, T::kOnOn, S::kDecay, 3, E::kSubtractive},
    {R::kEnvTime, T::kOnOn, S::kAttack, 3, E::kSubtractive},
    {R::kStereoSpread, T::kOnOn, S::kSustain, 1, E::kSubtractive},
    {R::kStereoSpread, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kKeyTrack, T::kOnOn, S::kSustain, 1, E::kModal},
    {R::kGlide, T::kOnOn, S::kSustain, 1, E::kModal},
}};

const char* engine_name(Engine e) {
  switch (e) {
    case Engine::kSubtractive:
      return "subtractive";
    case Engine::kModal:
      return "modal";
  }
  return "?";
}

std::pair<float, float> transition_values(Transition t, float on_lo, float on_hi) {
  switch (t) {
    case Transition::kOffOn:
      return {0.0f, on_hi};
    case Transition::kOnOff:
      return {on_hi, 0.0f};
    case Transition::kOnOn:
      return {on_lo, on_hi};
  }
  return {0.0f, 0.0f};
}

// The stereo-spread note: its seeded scatter sits far from centre (asserted
// through the oracle's control contrast).
constexpr uint8_t kSpreadNote = 62;
// The drift note: its seeded drift depth is near full scale.
constexpr uint8_t kDriftNote = 84;

/// Expands one pairwise row into its concrete scenarios.
std::vector<Scenario> expand(const PairwiseRow& row, size_t index) {
  std::vector<Scenario> out;
  const bool modal = row.engine == Engine::kModal;
  auto make = [&](const char* what, VaWaveform wave) {
    Scenario sc;
    sc.cfg = base_config(row.engine, row.unison, wave);
    sc.name = "row " + std::to_string(index) + " " + what + " " + engine_name(row.engine) +
              " unison " + std::to_string(row.unison);
    return sc;
  };
  const Transition tr = row.transition;
  switch (row.param) {
    case Row::kDetune: {
      Scenario sc = make("detuneCents 25->100", VaWaveform::kSaw);
      sc.param = P::kDetuneCents;
      sc.a = 25.0f;
      sc.b = 100.0f;
      sc.notes = {81};
      out.push_back(sc);
      break;
    }
    case Row::kDrive: {
      Scenario sc = make("drive", VaWaveform::kSaw);
      sc.param = P::kDrive;
      std::tie(sc.a, sc.b) = transition_values(tr, 0.2f, tr == T::kOnOn ? 0.8f : 0.6f);
      sc.notes = {57};
      sc.name += " " + std::to_string(sc.a) + "->" + std::to_string(sc.b);
      out.push_back(sc);
      break;
    }
    case Row::kKeyTrack: {
      Scenario sc = make("keyTrack 0.25->1", VaWaveform::kSaw);
      // The modal sine at note 96 sits under a 2 kHz cutoff either way.
      sc.cfg.patch.cutoff_hz = modal ? 500.0f : 2000.0f;
      sc.param = P::kKeyTrack;
      sc.a = 0.25f;
      sc.b = 1.0f;
      sc.notes = {96};
      out.push_back(sc);
      break;
    }
    case Row::kVelToCutoff: {
      Scenario sc = make("velToCutoffCents 600->3600", VaWaveform::kSaw);
      sc.cfg.patch.cutoff_hz = 2000.0f;
      sc.param = P::kVelToCutoffCents;
      sc.a = 600.0f;
      sc.b = 3600.0f;
      sc.notes = {static_cast<uint8_t>(modal ? 91 : 48)};  // a sine near the cutoff
      sc.velocity = 40;
      out.push_back(sc);
      break;
    }
    case Row::kBodyMix: {
      // The wood-tube body tracks the note, so its resonance lands on the sine.
      Scenario sc = make("bodyMix", VaWaveform::kSine);
      sc.cfg.patch.body = BodyType::kWoodTube;
      sc.param = P::kBodyMix;
      std::tie(sc.a, sc.b) = transition_values(tr, 0.3f, tr == T::kOnOn ? 1.0f : 0.8f);
      sc.notes = {52};
      sc.name += " " + std::to_string(sc.a) + "->" + std::to_string(sc.b);
      out.push_back(sc);
      break;
    }
    case Row::kHpCutoff: {
      Scenario sc = make("hpCutoffHz", VaWaveform::kSaw);
      sc.param = P::kHpCutoffHz;
      std::tie(sc.a, sc.b) = transition_values(tr, 100.0f, 800.0f);
      sc.notes = {36};
      sc.name += " " + std::to_string(sc.a) + "->" + std::to_string(sc.b);
      out.push_back(sc);
      break;
    }
    case Row::kConverter: {
      Scenario hold = make("sampleHoldHz", VaWaveform::kSine);
      hold.param = P::kSampleHoldHz;
      std::tie(hold.a, hold.b) = transition_values(tr, 1500.0f, tr == T::kOnOn ? 6000.0f : 3000.0f);
      hold.name += " " + std::to_string(hold.a) + "->" + std::to_string(hold.b);
      out.push_back(hold);
      Scenario bits = make("bitDepth", VaWaveform::kSine);
      bits.param = P::kBitDepth;
      std::tie(bits.a, bits.b) = transition_values(tr, 2.0f, tr == T::kOnOn ? 6.0f : 3.0f);
      bits.name += " " + std::to_string(bits.a) + "->" + std::to_string(bits.b);
      out.push_back(bits);
      break;
    }
    case Row::kStereoSpread: {
      Scenario sc = make("stereoSpread 0.2->1", VaWaveform::kSine);
      sc.param = P::kStereoSpread;
      sc.a = 0.2f;
      sc.b = 1.0f;
      sc.notes = {kSpreadNote};
      sc.obs = Obs::kPanRatio;
      out.push_back(sc);
      break;
    }
    case Row::kLfoRate: {
      Scenario sc = make("lfoRateHz k1->k2", VaWaveform::kSine);
      sc.cfg.patch.lfo_to_pitch_cents = kLfoDepthCents;
      sc.param = P::kLfoRateHz;
      sc.a = kLfoA;
      sc.b = kLfoB;
      sc.obs = Obs::kLfoPeriod;
      out.push_back(sc);
      break;
    }
    case Row::kLfo2Rate: {
      Scenario sc = make("lfo2RateHz k1->k2", VaWaveform::kSine);
      sc.cfg.patch.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kPitchCents,
                                           kLfoDepthCents};
      sc.param = P::kLfo2RateHz;
      sc.a = kLfoA;
      sc.b = kLfoB;
      sc.obs = Obs::kLfoPeriod;
      out.push_back(sc);
      break;
    }
    case Row::kDrift: {
      Scenario sc = make("driftCents 10->30", VaWaveform::kSine);
      sc.cfg.patch.drift_rate_hz = 40.0f;
      sc.param = P::kDriftCents;
      sc.a = 10.0f;
      sc.b = 30.0f;
      sc.notes = {kDriftNote};
      sc.obs = Obs::kPitchDeviation;
      out.push_back(sc);
      break;
    }
    case Row::kEnvTime: {
      // Amp: sine at full cutoff. Filter: sine 4 octaves above the cutoff,
      // swept 2 octaves, so the filtered level in dB is linear in the envelope.
      for (bool filter : {false, true}) {
        Scenario sc = make(filter ? "filter" : "amp", VaWaveform::kSine);
        auto& p = sc.cfg.patch;
        sc.notes = {81};
        sc.obs = filter ? Obs::kFilterTau : Obs::kAmpTau;
        if (filter) {
          p.cutoff_hz = sonare::midi::synth::synth_note_to_hz(81.0f) / 16.0f;
          p.env_to_cutoff_cents = 2400.0f;
        }
        auto& env = filter ? p.filter_env : p.amp_env;
        if (row.stage == Stage::kAttack) {
          sc.tl = Timeline::kAttack;
          sc.param = filter ? P::kFilterAttackMs : P::kAmpAttackMs;
          sc.a = 400.0f;
          sc.b = 200.0f;
          sc.name += " attackMs 400->200";
        } else if (row.stage == Stage::kDecay) {
          sc.tl = Timeline::kDecay;
          sc.param = filter ? P::kFilterDecayMs : P::kAmpDecayMs;
          env.sustain = 0.0f;
          sc.a = 600.0f;
          sc.b = 300.0f;
          sc.name += " decayMs 600->300";
        } else {
          sc.tl = Timeline::kRelease;
          sc.param = filter ? P::kFilterReleaseMs : P::kAmpReleaseMs;
          if (filter) p.amp_env.release_ms = 20000.0f;
          sc.a = 600.0f;
          sc.b = 300.0f;
          sc.name += " releaseMs 600->300";
        }
        out.push_back(sc);
      }
      break;
    }
    case Row::kSustain: {
      for (bool filter : {false, true}) {
        Scenario sc =
            make(filter ? "filterSustain 0.5->0.25" : "ampSustain 0.5->0.25", VaWaveform::kSine);
        auto& p = sc.cfg.patch;
        sc.notes = {81};
        sc.obs = Obs::kLevel;
        sc.param = filter ? P::kFilterSustain : P::kAmpSustain;
        sc.a = 0.5f;
        sc.b = 0.25f;
        if (filter) {
          p.cutoff_hz = sonare::midi::synth::synth_note_to_hz(81.0f) / 16.0f;
          p.env_to_cutoff_cents = 2400.0f;
        }
        if (row.stage == Stage::kDecay) {
          sc.tl = Timeline::kInDecay;
          (filter ? p.filter_env : p.amp_env).decay_ms = 300.0f;
          sc.name += " in decay";
        } else {
          sc.name += " in sustain";
        }
        out.push_back(sc);
      }
      break;
    }
    case Row::kGlide: {
      Scenario sc = make("glideMs 400->100", VaWaveform::kSine);
      sc.param = P::kGlideMs;
      sc.a = 400.0f;
      sc.b = 100.0f;
      sc.obs = Obs::kGlideTau;
      sc.tl = Timeline::kGlide;
      // The modal engine declines legato; its portamento runs on a retrigger.
      sc.modal_glide = modal;
      out.push_back(sc);
      break;
    }
  }
  return out;
}

std::vector<Scenario> sc1_scenarios() {
  std::vector<Scenario> all;
  for (size_t i = 0; i < kPairwiseRows.size(); ++i) {
    for (Scenario& sc : expand(kPairwiseRows[i], i)) all.push_back(std::move(sc));
  }
  // Outside the model: an LFO stopped. The bowed-string body crossing is not
  // here: its full-spectrum contrast stays under 2 dB for every body and note,
  // below the oracle's 3 dB validity floor.
  {
    Scenario sc;
    sc.cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    sc.cfg.patch.lfo_to_pitch_cents = kLfoDepthCents;
    sc.name = "extra lfoRateHz k1->0";
    sc.param = P::kLfoRateHz;
    sc.a = kLfoA;
    sc.b = 0.0f;
    sc.obs = Obs::kLfoStop;
    all.push_back(sc);
  }
  return all;
}

/// SC1 scenarios whose observable is in @p group.
std::vector<Scenario> sc1_group(std::initializer_list<Obs> group) {
  std::vector<Scenario> out;
  for (Scenario& sc : sc1_scenarios()) {
    if (std::find(group.begin(), group.end(), sc.obs) != group.end()) out.push_back(sc);
  }
  return out;
}

}  // namespace

// --- SC1 -------------------------------------------------------------------

TEST_CASE("live params: the 21 note-on ids cover the pairwise expansion",
          "[midi][synth][synth-live]") {
  std::vector<bool> seen(sonare::midi::synth::native_synth_param_count(), false);
  for (const Scenario& sc : sc1_scenarios()) seen[pid(sc.param)] = true;
  const std::array<P, 7> already_live{P::kGain,
                                      P::kBusDrive,
                                      P::kCutoffHz,
                                      P::kResonanceQ,
                                      P::kEnvToCutoffCents,
                                      P::kLfoToPitchCents,
                                      P::kPitchOffsetCents};
  int covered = 0;
  for (size_t i = 0; i < seen.size(); ++i) {
    const bool live = std::find(already_live.begin(), already_live.end(), static_cast<P>(i)) !=
                      already_live.end();
    INFO("param id " << i);
    CHECK(seen[i] != live);
    if (seen[i]) ++covered;
  }
  CHECK(covered == 21);
}

TEST_CASE("live params: a spectral parameter moved on a held note sounds as if struck at B",
          "[midi][synth][synth-live]") {
  for (const Scenario& sc : sc1_group({Obs::kSpectrum})) {
    DYNAMIC_SECTION(sc.name) { check_equivalence(sc); }
  }
}

TEST_CASE("live params: pan, level and pitch parameters reach a held note",
          "[midi][synth][synth-live]") {
  for (const Scenario& sc : sc1_group(
           {Obs::kPanRatio, Obs::kLevel, Obs::kLfoPeriod, Obs::kLfoStop, Obs::kPitchDeviation})) {
    DYNAMIC_SECTION(sc.name) { check_equivalence(sc); }
  }
}

TEST_CASE("live params: envelope and glide times change inside the running stage",
          "[midi][synth][synth-live]") {
  for (const Scenario& sc : sc1_group({Obs::kAmpTau, Obs::kFilterTau, Obs::kGlideTau})) {
    DYNAMIC_SECTION(sc.name) { check_equivalence(sc); }
  }
}

// --- SC3 -------------------------------------------------------------------

TEST_CASE("live params: a change reaches every voice of a held chord",
          "[midi][synth][synth-live]") {
  auto chord = [](const char* name, VaWaveform wave, P param, float a, float b,
                  std::vector<uint8_t> notes, int unison = 1) {
    Scenario sc;
    sc.cfg = base_config(Engine::kSubtractive, unison, wave);
    sc.name = name;
    sc.param = param;
    sc.a = a;
    sc.b = b;
    sc.notes = std::move(notes);
    return sc;
  };
  const std::vector<uint8_t> mid{57, 61, 64};
  std::vector<Scenario> all;
  all.push_back(chord("drive 0.2->0.8", VaWaveform::kSaw, P::kDrive, 0.2f, 0.8f, mid));
  all.push_back(
      chord("hpCutoffHz 100->800", VaWaveform::kSaw, P::kHpCutoffHz, 100.0f, 800.0f, {36, 40, 43}));
  all.push_back(
      chord("sampleHoldHz 1500->6000", VaWaveform::kSine, P::kSampleHoldHz, 1500.0f, 6000.0f, mid));
  all.push_back(chord("bitDepth 2->6", VaWaveform::kSine, P::kBitDepth, 2.0f, 6.0f, mid));
  all.push_back(chord("detuneCents 25->100", VaWaveform::kSaw, P::kDetuneCents, 25.0f, 100.0f,
                      {76, 80, 83}, 3));
  {
    Scenario sc =
        chord("keyTrack 0.25->1", VaWaveform::kSaw, P::kKeyTrack, 0.25f, 1.0f, {84, 88, 91});
    sc.cfg.patch.cutoff_hz = 2000.0f;
    all.push_back(sc);
  }
  {
    Scenario sc = chord("velToCutoffCents 600->3600", VaWaveform::kSaw, P::kVelToCutoffCents,
                        600.0f, 3600.0f, {48, 52, 55});
    sc.cfg.patch.cutoff_hz = 2000.0f;
    sc.velocity = 40;
    all.push_back(sc);
  }
  {
    Scenario sc = chord("bodyMix 0.3->1", VaWaveform::kSine, P::kBodyMix, 0.3f, 1.0f, mid);
    sc.cfg.patch.body = BodyType::kWoodTube;
    all.push_back(sc);
  }
  {
    Scenario sc = chord("stereoSpread 0.2->1", VaWaveform::kSine, P::kStereoSpread, 0.2f, 1.0f,
                        {kSpreadNote, 66, 69});
    sc.obs = Obs::kPanRatio;
    all.push_back(sc);
  }
  {
    Scenario sc =
        chord("ampSustain 0.5->0.25", VaWaveform::kSine, P::kAmpSustain, 0.5f, 0.25f, mid);
    sc.obs = Obs::kLevel;
    all.push_back(sc);
  }
  for (const Scenario& sc : all) {
    DYNAMIC_SECTION(sc.name) { check_equivalence(sc); }
  }
}

// --- SC2(a) ------------------------------------------------------------------

namespace {

constexpr float kTestBusDrive = 0.3f;

/// The value @p id currently holds on @p s (busDrive is the one the test set).
float current_value(const NativeSynth& s, P id) {
  const auto& p = s.patch();
  switch (id) {
    case P::kGain:
      return s.gain();
    case P::kBusDrive:
      return kTestBusDrive;
    case P::kCutoffHz:
      return p.cutoff_hz;
    case P::kResonanceQ:
      return p.resonance_q;
    case P::kDrive:
      return p.drive;
    case P::kKeyTrack:
      return p.key_track;
    case P::kEnvToCutoffCents:
      return p.env_to_cutoff_cents;
    case P::kVelToCutoffCents:
      return p.vel_to_cutoff_cents;
    case P::kAmpAttackMs:
      return p.amp_env.attack_ms;
    case P::kAmpDecayMs:
      return p.amp_env.decay_ms;
    case P::kAmpSustain:
      return p.amp_env.sustain;
    case P::kAmpReleaseMs:
      return p.amp_env.release_ms;
    case P::kFilterAttackMs:
      return p.filter_env.attack_ms;
    case P::kFilterDecayMs:
      return p.filter_env.decay_ms;
    case P::kFilterSustain:
      return p.filter_env.sustain;
    case P::kFilterReleaseMs:
      return p.filter_env.release_ms;
    case P::kLfoRateHz:
      return p.lfo_rate_hz;
    case P::kLfoToPitchCents:
      return p.lfo_to_pitch_cents;
    case P::kLfo2RateHz:
      return p.lfo2_rate_hz;
    case P::kGlideMs:
      return p.glide_ms;
    case P::kBodyMix:
      return p.body_mix;
    case P::kStereoSpread:
      return p.stereo_spread;
    case P::kDetuneCents:
      return p.detune_cents;
    case P::kDriftCents:
      return p.drift_cents;
    case P::kPitchOffsetCents:
      return p.pitch_offset_cents;
    case P::kHpCutoffHz:
      return p.hp_cutoff_hz;
    case P::kSampleHoldHz:
      return p.sample_hold_hz;
    case P::kBitDepth:
      return p.bit_depth;
  }
  return 0.0f;
}

/// A patch with every automatable field set inside its table range.
NativeSynthConfig in_range_config() {
  NativeSynthConfig cfg;
  cfg.gain = 0.5f;
  cfg.bus_drive = kTestBusDrive;
  auto& p = cfg.patch;
  p.waveform = VaWaveform::kSaw;
  p.unison = 3;
  p.detune_cents = 12.0f;
  p.drift_cents = 4.0f;
  p.pitch_offset_cents = 7.0f;
  p.amp_env = {0.0f, 5.0f, 0.0f, 200.0f, 0.6f, 300.0f};
  p.filter_env = {0.0f, 10.0f, 0.0f, 300.0f, 0.4f, 200.0f};
  p.env_to_cutoff_cents = 1200.0f;
  p.cutoff_hz = 3000.0f;
  p.resonance_q = 1.2f;
  p.hp_cutoff_hz = 60.0f;
  p.drive = 0.3f;
  p.sample_hold_hz = 30000.0f;
  p.bit_depth = 14.0f;
  p.key_track = 0.5f;
  p.vel_to_cutoff_cents = 1200.0f;
  p.lfo_rate_hz = 5.0f;
  p.lfo_to_pitch_cents = 10.0f;
  p.lfo2_rate_hz = 1.5f;
  p.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kCutoffCents, 300.0f};
  p.glide_ms = 30.0f;
  p.body = BodyType::kGuitar;
  p.body_mix = 0.4f;
  p.stereo_spread = 0.6f;
  return cfg;
}

void apply_all_current(NativeSynth& s) {
  for (size_t i = 0; i < sonare::midi::synth::native_synth_param_count(); ++i) {
    const P id = static_cast<P>(i);
    REQUIRE(s.apply_parameter(pid(id), current_value(s, id)));
  }
}

}  // namespace

TEST_CASE("live params: re-applying every current value leaves a held chord bit-identical",
          "[midi][synth][synth-live]") {
  const NativeSynthConfig cfg = in_range_config();
  NativeSynth touched(cfg);
  NativeSynth untouched(cfg);
  Stereo a, b;
  for (NativeSynth* s : {&touched, &untouched}) {
    s->prepare(kRate, kBlock);
    for (uint8_t n : {60, 64, 67}) note_on(*s, n, 100);
  }
  // Before the first block, mid-attack, every block for a stretch, and in release.
  apply_all_current(touched);
  render(touched, a, 4096);
  render(untouched, b, 4096);
  for (int block = 0; block < 16; ++block) {
    apply_all_current(touched);
    render(touched, a, kBlock);
    render(untouched, b, kBlock);
  }
  for (NativeSynth* s : {&touched, &untouched}) {
    for (uint8_t n : {60, 64, 67}) note_off(*s, n);
  }
  apply_all_current(touched);
  render(touched, a, 8192);
  render(untouched, b, 8192);

  REQUIRE(rms(b.l, 0, b.l.size()) > 1.0e-3);
  size_t mismatches = 0;
  for (size_t i = 0; i < a.l.size(); ++i) {
    if (a.l[i] != b.l[i] || a.r[i] != b.r[i]) ++mismatches;
  }
  CHECK(mismatches == 0);
}

// --- SC6 -------------------------------------------------------------------

TEST_CASE("live params: a GM program voice ignores patch automation", "[midi][synth][synth-live]") {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  NativeSynth touched(cfg);
  NativeSynth untouched(cfg);
  Stereo a, b;
  for (NativeSynth* s : {&touched, &untouched}) {
    s->prepare(kRate, kBlock);
    s->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 80)));
    note_on(*s, 69, 100);
    render(*s, s == &touched ? a : b, 4096);
  }
  int applied = 0;
  for (size_t i = 0; i < sonare::midi::synth::native_synth_param_count(); ++i) {
    const P id = static_cast<P>(i);
    if (id == P::kGain || id == P::kBusDrive) continue;  // mix bus, reaches GM voices too
    sonare::automation::ParameterDescription desc;
    REQUIRE(touched.describe_parameter(pid(id), &desc));
    const float cur = current_value(touched, id);
    const float span = desc.max_value - desc.min_value;
    float v = cur + 0.25f * span;
    if (v > desc.max_value) v = cur - 0.25f * span;
    REQUIRE(touched.apply_parameter(pid(id), v));
    INFO("param id " << i);
    REQUIRE(current_value(touched, id) != cur);
    ++applied;
  }
  REQUIRE(applied == 26);
  render(touched, a, 8192);
  render(untouched, b, 8192);
  REQUIRE(rms(b.l, 4096, 8192) > 1.0e-3);
  size_t mismatches = 0;
  for (size_t i = 0; i < a.l.size(); ++i) {
    if (a.l[i] != b.l[i] || a.r[i] != b.r[i]) ++mismatches;
  }
  CHECK(mismatches == 0);
}

// --- SC7 -------------------------------------------------------------------

namespace {

/// First sample index from which |x| < 1e-6 holds for 256 samples, or x.size().
size_t silence_onset(const std::vector<float>& x, size_t from) {
  size_t run = 0;
  for (size_t i = from; i < x.size(); ++i) {
    run = std::fabs(x[i]) < 1.0e-6f ? run + 1 : 0;
    if (run == 256) return i + 1 - 256;
  }
  return x.size();
}

}  // namespace

TEST_CASE("live params: a choked voice does not take a lengthened release",
          "[midi][synth][synth-live]") {
  // The replacing note waits out a 300 ms delay stage, so the window after the
  // cut holds only the cut voice's tail.
  constexpr float kDelayMs = 300.0f;
  constexpr size_t kPreCut = 24576;    // past the delay and into the sustain
  constexpr size_t kAfterCut = 12288;  // inside the replacing note's delay

  auto cut_tail = [&](NativeSynthConfig cfg, ArticulationMode mode, uint8_t first, uint8_t second,
                      bool lengthen) {
    cfg.dc_block = false;
    NativeSynth s(cfg);
    s.prepare(kRate, kBlock);
    s.set_articulation(0, mode);
    Stereo out;
    note_on(s, first, 100);
    render(s, out, kPreCut);
    note_on(s, second, 100);
    if (lengthen) REQUIRE(s.apply_parameter(pid(P::kAmpReleaseMs), 20000.0f));
    render(s, out, kAfterCut);
    REQUIRE(rms(out.l, kPreCut - 4096, 4096) > 1.0e-3);  // the first note was sounding
    return silence_onset(out.l, kPreCut);
  };

  SECTION("mono retrigger choke_fast") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    cfg.patch.amp_env.delay_ms = kDelayMs;
    const size_t plain = cut_tail(cfg, ArticulationMode::kMonoRetrigger, 60, 64, false);
    const size_t lengthened = cut_tail(cfg, ArticulationMode::kMonoRetrigger, 60, 64, true);
    REQUIRE(plain < kPreCut + kAfterCut);  // the cut voice does fall silent
    CHECK(lengthened == plain);
  }

  SECTION("exclusive-class percussion choke") {
    NativeSynthConfig cfg;
    auto& p = cfg.patch;
    p.mode = SynthEngineMode::kPercussion;
    p.one_shot = true;
    p.retrigger = SynthRetrigger::kNote;
    p.percussion.gm_kit = false;
    p.percussion.exclusive_class = 1;
    p.percussion.num_modes = 1;
    p.percussion.mode_decay_s = 30.0f;  // rings far past the window unless cut
    p.amp_env = {kDelayMs, 1.0f, 0.0f, 1.0f, 1.0f, 50.0f};
    const size_t plain = cut_tail(cfg, ArticulationMode::kPoly, 42, 46, false);
    const size_t lengthened = cut_tail(cfg, ArticulationMode::kPoly, 42, 46, true);
    REQUIRE(plain < kPreCut + kAfterCut);
    CHECK(lengthened == plain);
  }
}

// --- SC4 -------------------------------------------------------------------

namespace {

/// Largest first difference over [from, to).
double max_step(const std::vector<float>& x, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) {
    m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  }
  return m;
}

struct Continuity {
  std::string name;
  NativeSynthConfig cfg;
  P param;
  float a;
  std::vector<float> changes;  // applied in turn, `spacing` samples apart
  size_t change_at;            // first change, in samples (block-aligned)
  int spacing = kBlock;
  std::vector<uint8_t> notes{69};
  bool release_first = false;  // note-off 150 ms before the change
  bool legato = false;
  Obs effect = Obs::kSpectrum;
};

/// Steady difference between the pre- and post-change spans, with its floor.
std::pair<double, double> steady_effect(const Continuity& c, const Stereo& out,
                                        size_t last_change) {
  const std::vector<float>& x = out.l;
  const size_t pre_from = c.change_at >= kWindow ? c.change_at - kWindow : 0;
  const size_t post_from = last_change + kSettle;
  switch (c.effect) {
    case Obs::kSpectrum: {
      const auto pre = avg_spectrum(x, pre_from, kWindow);
      const auto post = avg_spectrum(x, post_from, kWindow);
      return {spectral_diff_db(pre, post), kContrast * kSpectrumTolDb};
    }
    case Obs::kLfoPeriod: {
      const double pre = modulation_period(x, pre_from, kWindow);
      const double post = modulation_period(x, post_from, kWindow);
      return {rel(post, pre), kContrast * kRelTol};
    }
    case Obs::kPitchDeviation: {
      const double pre = pitch_deviation(x, pre_from, kWindow);
      const double post = pitch_deviation(x, post_from, kWindow);
      return {rel(post, pre), kContrast * kRelTol};
    }
    case Obs::kAmpTau: {
      // Envelope stage: fit the log-amplitude slope on each side of the change.
      const size_t span = 4096;
      const Frames pre = tkeo_frames(x, c.change_at - span, span);
      const Frames post = tkeo_frames(x, c.change_at, span);
      const bool attack = c.param == P::kAmpAttackMs;
      const double full = tail_amplitude(x);
      auto ys = [&](const Frames& f) {
        std::vector<double> y;
        for (double a : f.a) y.push_back(attack ? kAttackTarget - a / full : a);
        return y;
      };
      const double tp = fit_tau(pre.t, ys(pre));
      const double tq = fit_tau(post.t, ys(post));
      INFO("tau pre " << tp << " post " << tq);
      return {rel(tq, tp), kContrast * kTauTol};
    }
    case Obs::kGlideTau: {
      const Cycles tail = cycles(x, x.size() - 4096, 4096);
      const double target = mean(tail.hz);
      const size_t glide_start = c.change_at - 4864;
      auto tau_between = [&](size_t from, size_t len) {
        const Cycles cy = cycles(x, from, len);
        std::vector<double> t, y;
        for (size_t i = 0; i < cy.hz.size(); ++i) {
          const double dev = std::fabs(1200.0 * std::log2(cy.hz[i] / target));
          if (dev < 3.0 || dev > 1150.0) continue;
          t.push_back(cy.t[i]);
          y.push_back(dev);
        }
        return fit_tau(t, y);
      };
      const double tp = tau_between(glide_start, 4864);
      const double tq = tau_between(c.change_at, 4864);
      INFO("glide tau pre " << tp << " post " << tq);
      return {rel(tq, tp), kContrast * kTauTol};
    }
    default:
      break;
  }
  return {0.0, 1.0};
}

void check_continuity(const Continuity& c) {
  NativeSynth s(c.cfg);
  s.prepare(kRate, kBlock);
  REQUIRE(s.apply_parameter(pid(c.param), c.a));
  if (c.legato) s.set_articulation(0, ArticulationMode::kMonoLegato);
  Stereo out;
  if (c.legato) {
    note_on(s, 57);
    render(s, out, c.change_at - 4864);
    note_on(s, 69);
    render(s, out, 4864);
  } else {
    for (uint8_t n : c.notes) note_on(s, n);
    if (c.release_first) {
      render(s, out, c.change_at - 7168);
      for (uint8_t n : c.notes) note_off(s, n);
      render(s, out, 7168);
    } else {
      render(s, out, c.change_at);
    }
  }
  REQUIRE(out.l.size() == c.change_at);
  std::vector<size_t> points;
  for (float v : c.changes) {
    points.push_back(out.l.size());
    REQUIRE(s.apply_parameter(pid(c.param), v));
    render(s, out, static_cast<size_t>(c.spacing), c.spacing);
  }
  render(s, out, kSettle + kWindow);

  for (size_t at : points) {
    // Baseline is the 256 samples just before each change, a running fade included.
    const double before = max_step(out.l, at - 256, at);
    REQUIRE(before > 0.0);
    const double around = max_step(out.l, at - 32, at + 32);
    INFO(c.name << ": max step around change at " << at << " = " << around
                << ", before = " << before);
    CHECK(around <= 1.5 * before);
  }
  const auto [effect, floor] = steady_effect(c, out, points.back());
  INFO(c.name << ": steady effect " << effect << " (needs >= " << floor << ")");
  CHECK(effect >= floor);
}

}  // namespace

TEST_CASE("live params: a held-note change does not click", "[midi][synth][synth-live]") {
  std::vector<Continuity> all;
  auto sine = [] { return base_config(Engine::kSubtractive, 1, VaWaveform::kSine); };
  constexpr size_t kAt = kSettle + kWindow;  // steady sustain
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.amp_env.attack_ms = 400.0f;
    all.push_back({"ampAttackMs 400->200", cfg, P::kAmpAttackMs, 400.0f, {200.0f}, 4864});
    all.back().effect = Obs::kAmpTau;
  }
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.amp_env.sustain = 0.0f;
    all.push_back({"ampDecayMs 600->300", cfg, P::kAmpDecayMs, 600.0f, {300.0f}, 7168});
    all.back().effect = Obs::kAmpTau;
  }
  {
    NativeSynthConfig cfg = sine();
    all.push_back({"ampReleaseMs 600->300", cfg, P::kAmpReleaseMs, 600.0f, {300.0f}, kAt});
    all.back().release_first = true;
    all.back().effect = Obs::kAmpTau;
  }
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.lfo_to_pitch_cents = kLfoDepthCents;
    all.push_back({"lfoRateHz k1->k2", cfg, P::kLfoRateHz, kLfoA, {kLfoB}, kAt});
    all.back().effect = Obs::kLfoPeriod;
  }
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kPitchCents,
                                      kLfoDepthCents};
    all.push_back({"lfo2RateHz k1->k2", cfg, P::kLfo2RateHz, kLfoA, {kLfoB}, kAt});
    all.back().effect = Obs::kLfoPeriod;
  }
  {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 3, VaWaveform::kSine);
    all.push_back({"detuneCents 30->60", cfg, P::kDetuneCents, 30.0f, {60.0f}, kAt});
    all.back().notes = {93};
  }
  {
    NativeSynthConfig cfg = sine();
    all.push_back({"glideMs 400->100", cfg, P::kGlideMs, 400.0f, {100.0f}, kSettle + 4864});
    all.back().legato = true;
    all.back().effect = Obs::kGlideTau;
  }
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.drift_rate_hz = 40.0f;
    all.push_back({"driftCents 10->30", cfg, P::kDriftCents, 10.0f, {30.0f}, kAt});
    all.back().notes = {kDriftNote};
    all.back().effect = Obs::kPitchDeviation;
  }
  all.push_back({"drive 0->0.6", sine(), P::kDrive, 0.0f, {0.6f}, kAt});
  all.push_back({"drive 0.6->0", sine(), P::kDrive, 0.6f, {0.0f}, kAt});
  all.push_back({"drive 0.05->0.15", sine(), P::kDrive, 0.05f, {0.15f}, kAt});
  all.push_back({"drive 0->0.6->0->0.6 inside one fade",
                 sine(),
                 P::kDrive,
                 0.0f,
                 {0.6f, 0.0f, 0.6f},
                 kAt,
                 64});
  {
    NativeSynthConfig cfg = sine();
    cfg.patch.body = BodyType::kWoodTube;
    all.push_back({"bodyMix 0->1", cfg, P::kBodyMix, 0.0f, {1.0f}, kAt});
    all.push_back({"bodyMix 1->0", cfg, P::kBodyMix, 1.0f, {0.0f}, kAt});
    all.push_back({"bodyMix 0->1->0->1 inside one fade",
                   cfg,
                   P::kBodyMix,
                   0.0f,
                   {1.0f, 0.0f, 1.0f},
                   kAt,
                   64});
  }
  for (const Continuity& c : all) {
    DYNAMIC_SECTION(c.name) { check_continuity(c); }
  }
}

TEST_CASE("live params: zero-sustain envelopes stay editable on the sounding path",
          "[midi][synth][synth-live]") {
  SECTION("filter envelope remains alive for a later sustain edit") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    cfg.patch.waveform = VaWaveform::kSaw;
    cfg.patch.cutoff_hz = 500.0f;
    cfg.patch.env_to_cutoff_cents = 4800.0f;
    cfg.patch.filter_env.attack_ms = 1.0f;
    cfg.patch.filter_env.decay_ms = 40.0f;
    cfg.patch.filter_env.sustain = 0.0f;

    NativeSynth held(cfg);
    NativeSynth control(cfg);
    NativeSynthConfig fresh_cfg = cfg;
    fresh_cfg.patch.filter_env.sustain = 1.0f;
    NativeSynth fresh(fresh_cfg);
    for (NativeSynth* s : {&held, &control, &fresh}) s->prepare(kRate, kBlock);
    note_on(held, 69);
    note_on(control, 69);
    note_on(fresh, 69);

    Stereo pre_held, pre_control, pre_fresh;
    render(held, pre_held, kSettle);
    render(control, pre_control, kSettle);
    render(fresh, pre_fresh, kSettle);

    REQUIRE(held.apply_parameter(pid(P::kFilterSustain), 1.0f));
    Stereo post_held, post_control, post_fresh;
    render(held, post_held, kBlock + kWindow);
    render(control, post_control, kBlock + kWindow);
    render(fresh, post_fresh, kBlock + kWindow);

    const auto h = avg_spectrum(post_held.l, kBlock, kWindow);
    const auto c = avg_spectrum(post_control.l, kBlock, kWindow);
    const auto f = avg_spectrum(post_fresh.l, kBlock, kWindow);
    INFO("filter sustain live: held/ref " << spectral_diff_db(f, h) << ", control/ref "
                                          << spectral_diff_db(f, c));
    REQUIRE(held.active_voice_count() == 1);
    CHECK(spectral_diff_db(f, h) < kSpectrumTolDb);
    CHECK(spectral_diff_db(f, c) > kContrast * kSpectrumTolDb);
  }

  SECTION("amp envelope clears its percussive latch when sustain rises") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    cfg.patch.amp_env.attack_ms = 1.0f;
    cfg.patch.amp_env.decay_ms = 20.0f;
    cfg.patch.amp_env.sustain = 0.0f;

    NativeSynth held(cfg);
    NativeSynth control(cfg);
    NativeSynthConfig fresh_cfg = cfg;
    fresh_cfg.patch.amp_env.sustain = 0.5f;
    NativeSynth fresh(fresh_cfg);
    for (NativeSynth* s : {&held, &control, &fresh}) s->prepare(kRate, kBlock);
    note_on(held, 69);
    note_on(control, 69);
    note_on(fresh, 69);
    Stereo pre_held, pre_control, pre_fresh;
    render(held, pre_held, 256);
    render(control, pre_control, 256);
    render(fresh, pre_fresh, 256);

    REQUIRE(held.apply_parameter(pid(P::kAmpSustain), 0.5f));
    Stereo post_held, post_control, post_fresh;
    render(held, post_held, kBlock + kWindow);
    render(control, post_control, kBlock + kWindow);
    render(fresh, post_fresh, kBlock + kWindow);

    const auto h = avg_spectrum(post_held.l, kBlock, kWindow);
    const auto c = avg_spectrum(post_control.l, kBlock, kWindow);
    const auto f = avg_spectrum(post_fresh.l, kBlock, kWindow);
    INFO("amp sustain live: held/ref " << spectral_diff_db(f, h) << ", control/ref "
                                       << spectral_diff_db(f, c));
    REQUIRE(held.active_voice_count() == 1);
    CHECK(spectral_diff_db(f, h) < kSpectrumTolDb);
    CHECK(spectral_diff_db(f, c) > kContrast * kSpectrumTolDb);
  }
}

TEST_CASE("legato retune updates key tracking and live body tuning", "[midi][synth][synth-live]") {
  SECTION("key tracking follows the carried note") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSaw);
    cfg.patch.cutoff_hz = 1000.0f;
    cfg.patch.key_track = 1.0f;
    cfg.patch.env_to_cutoff_cents = 0.0f;

    NativeSynth held(cfg);
    NativeSynth fresh(cfg);
    NativeSynth control(cfg);
    for (NativeSynth* s : {&held, &fresh, &control}) s->prepare(kRate, kBlock);
    held.set_articulation(0, ArticulationMode::kMonoLegato);
    note_on(held, 48);
    Stereo held_pre;
    render(held, held_pre, kSettle);
    note_on(held, 72);
    Stereo held_out;
    render(held, held_out, kSettle + kWindow);

    note_on(fresh, 72);
    Stereo fresh_out;
    render(fresh, fresh_out, kSettle + kWindow);
    note_on(control, 48);
    Stereo control_out;
    render(control, control_out, kSettle + kWindow);

    const auto h = avg_spectrum(held_out.l, kSettle, kWindow);
    const auto f = avg_spectrum(fresh_out.l, kSettle, kWindow);
    const auto c = avg_spectrum(control_out.l, kSettle, kWindow);
    INFO("legato key tracking: held/ref " << spectral_diff_db(f, h) << ", control/ref "
                                          << spectral_diff_db(f, c));
    CHECK(spectral_diff_db(f, h) < kSpectrumTolDb);
    CHECK(spectral_diff_db(f, c) > kContrast * kSpectrumTolDb);
  }

  SECTION("a body enabled after legato uses the retuned note") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    cfg.patch.body = BodyType::kWoodTube;
    cfg.patch.body_mix = 0.0f;
    NativeSynthConfig fresh_cfg = cfg;
    fresh_cfg.patch.body_mix = 1.0f;

    NativeSynth held(cfg);
    NativeSynth fresh(fresh_cfg);
    NativeSynth control(cfg);
    for (NativeSynth* s : {&held, &fresh, &control}) s->prepare(kRate, kBlock);
    held.set_articulation(0, ArticulationMode::kMonoLegato);
    note_on(held, 48);
    Stereo pre;
    render(held, pre, kSettle);
    note_on(held, 72);
    render(held, pre, kWindow);
    REQUIRE(held.apply_parameter(pid(P::kBodyMix), 1.0f));
    Stereo held_out;
    render(held, held_out, kBlock + kWindow);

    note_on(fresh, 72);
    Stereo fresh_out;
    render(fresh, fresh_out, kSettle + kWindow);

    note_on(control, 72);
    Stereo control_out;
    render(control, control_out, kSettle + kWindow);

    const auto h = avg_spectrum(held_out.l, kBlock, kWindow);
    const auto f = avg_spectrum(fresh_out.l, kSettle, kWindow);
    const auto c = avg_spectrum(control_out.l, kSettle, kWindow);
    INFO("legato body tuning: held/ref " << spectral_diff_db(f, h) << ", control/ref "
                                         << spectral_diff_db(f, c));
    CHECK(spectral_diff_db(f, h) < kSpectrumTolDb);
    CHECK(spectral_diff_db(f, c) > kContrast * kSpectrumTolDb);
  }

  SECTION("an active note-tracked body retunes with the carried note") {
    NativeSynthConfig cfg = base_config(Engine::kSubtractive, 1, VaWaveform::kSine);
    cfg.patch.body = BodyType::kWoodTube;
    cfg.patch.body_mix = 1.0f;

    NativeSynth held(cfg);
    NativeSynth fresh(cfg);
    NativeSynth control(cfg);
    for (NativeSynth* s : {&held, &fresh, &control}) s->prepare(kRate, kBlock);
    held.set_articulation(0, ArticulationMode::kMonoLegato);
    note_on(held, 48);
    Stereo held_pre;
    render(held, held_pre, kSettle);
    note_on(held, 72);
    Stereo held_out;
    render(held, held_out, kSettle + kWindow);

    note_on(fresh, 72);
    Stereo fresh_out;
    render(fresh, fresh_out, kSettle + kWindow);

    note_on(control, 48);
    Stereo control_out;
    render(control, control_out, kSettle + kWindow);

    const auto h = avg_spectrum(held_out.l, kSettle, kWindow);
    const auto f = avg_spectrum(fresh_out.l, kSettle, kWindow);
    const auto c = avg_spectrum(control_out.l, kSettle, kWindow);
    INFO("active legato body tuning: held/ref " << spectral_diff_db(f, h) << ", control/ref "
                                                << spectral_diff_db(f, c));
    CHECK(spectral_diff_db(f, h) < kSpectrumTolDb);
    CHECK(spectral_diff_db(f, c) > kContrast * kSpectrumTolDb);
  }
}

// --- SC5 -------------------------------------------------------------------

#if defined(SONARE_WITH_MIXING)
TEST_CASE("live params: automating every id on a held chord does not allocate",
          "[midi][synth][synth-live]") {
  using sonare::test::AllocationGuard;
  // Positive control: the counting hooks are linked and armed.
  {
    void* probe = nullptr;
    size_t probe_allocations = 0;
    {
      AllocationGuard guard;
      probe = ::operator new(1);
      probe_allocations = guard.count();
    }
    ::operator delete(probe);
    REQUIRE(probe_allocations >= 1);
  }

  NativeSynth synth(in_range_config());
  synth.prepare(kRate, kBlock);
  for (uint8_t n : {60, 64, 67}) note_on(synth, n, 100);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  synth.process(ch, 2, kBlock);

  const size_t count = sonare::midi::synth::native_synth_param_count();
  std::vector<float> lo(count), hi(count);
  for (size_t i = 0; i < count; ++i) {
    sonare::automation::ParameterDescription desc;
    REQUIRE(synth.describe_parameter(static_cast<unsigned>(i), &desc));
    // The minimum is off / zero for the stages that switch on and off.
    lo[i] = desc.min_value;
    hi[i] = desc.min_value + 0.25f * (desc.max_value - desc.min_value);
  }
  size_t allocations = 0;
  {
    AllocationGuard guard;
    for (int iter = 0; iter < 32; ++iter) {
      for (size_t i = 0; i < count; ++i) {
        synth.apply_parameter(static_cast<unsigned>(i), (iter & 1) != 0 ? lo[i] : hi[i]);
      }
      synth.process(ch, 2, kBlock);
    }
    allocations = guard.count();
  }
  CHECK(allocations == 0);
}
#endif

TEST_CASE("live amp release extends the reported tail and outlives the old bound",
          "[midi][synth][tail-live]") {
  NativeSynthConfig cfg;
  cfg.patch.amp_env = {0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  note_on(synth, 60);
  Stereo held;
  render(synth, held, 4096);
  REQUIRE(synth.active_voice_count() == 1);

  const int64_t old_tail = synth.tail_samples();
  REQUIRE(old_tail > 0);
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), 20000.0f));
  const int64_t expected =
      sonare::midi::synth::DahdsrEnvelope::release_tail_samples(kRate, 20000.0f);
  CHECK(static_cast<int64_t>(synth.tail_samples()) >= expected);

  // Let the pending live refresh land before entering Release.
  Stereo refresh;
  render(synth, refresh, kBlock);
  note_off(synth, 60);
  Stereo after_old_bound;
  render(synth, after_old_bound, static_cast<size_t>(old_tail));
  CHECK(synth.active_voice_count() > 0);
}

TEST_CASE("live one-shot tail follows attack, decay and sustain transitions",
          "[midi][synth][tail-live]") {
  NativeSynthConfig cfg;
  cfg.patch.one_shot = true;
  cfg.patch.amp_env = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 1.0f};
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  const int initial = synth.tail_samples();

  REQUIRE(synth.apply_parameter(pid(P::kAmpAttackMs), 200.0f));
  const int after_attack = synth.tail_samples();
  CHECK(after_attack > initial);
  REQUIRE(synth.apply_parameter(pid(P::kAmpDecayMs), 2000.0f));
  const int after_decay = synth.tail_samples();
  CHECK(after_decay > after_attack);

  note_on(synth, 60);
  Stereo onset;
  render(synth, onset, kBlock);
  REQUIRE(synth.active_voice_count() == 1);
  REQUIRE(synth.apply_parameter(pid(P::kAmpSustain), 1.0f));
  Stereo raised;
  render(synth, raised, kBlock);
  REQUIRE(synth.active_voice_count() == 1);
  CHECK(synth.tail_samples() == std::numeric_limits<int>::max());

  // Lowering sustain does not relatch an already-running envelope. The
  // sentinel therefore remains until the old voice is cleared by reset().
  REQUIRE(synth.apply_parameter(pid(P::kAmpSustain), 0.0f));
  Stereo lowered;
  render(synth, lowered, kBlock);
  CHECK(synth.active_voice_count() == 1);
  CHECK(synth.tail_samples() == std::numeric_limits<int>::max());
  synth.reset();
  CHECK(synth.tail_samples() < std::numeric_limits<int>::max());
}

TEST_CASE("live amp tail clamps nonfinite values and stays monotonic after a choke",
          "[midi][synth][tail-live]") {
  NativeSynthConfig cfg;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 1.0f;
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  synth.set_articulation(0, ArticulationMode::kMonoRetrigger);
  note_on(synth, 60);
  Stereo first;
  render(synth, first, kBlock);

  // Choke with the original short release; the subsequent live write must
  // refresh the held voice without extending the predecessor's cut.
  note_on(synth, 64);
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), 20000.0f));
  Stereo long_refresh;
  render(synth, long_refresh, kBlock);
  const int long_tail = synth.tail_samples();
  const int64_t expected =
      sonare::midi::synth::DahdsrEnvelope::release_tail_samples(kRate, 20000.0f);
  REQUIRE(static_cast<int64_t>(long_tail) >= expected);

  Stereo choked;
  render(synth, choked, 8192);
  REQUIRE(synth.active_voice_count() == 1);
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), 1.0f));
  CHECK(synth.tail_samples() >= long_tail);

  const int after_long = synth.tail_samples();
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), std::numeric_limits<float>::quiet_NaN()));
  CHECK(synth.tail_samples() == after_long);
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), std::numeric_limits<float>::infinity()));
  CHECK(synth.tail_samples() == after_long);
}

TEST_CASE("GM fallback tail ignores custom amp automation", "[midi][synth][tail-live]") {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.patch.amp_env.release_ms = 1.0f;
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  const int fallback_tail = synth.tail_samples();
  REQUIRE(fallback_tail > 0);
  REQUIRE(synth.apply_parameter(pid(P::kAmpReleaseMs), 20000.0f));
  CHECK(synth.tail_samples() == fallback_tail);
}
