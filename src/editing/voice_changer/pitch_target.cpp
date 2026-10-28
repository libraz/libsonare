#include "editing/voice_changer/pitch_target.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::editing::voice_changer {

using constants::kA4Hz;
using constants::kCentsPerOctave;
using constants::kMidiA4;
using constants::kSemitonesPerOctave;
using constants::kTwoPiD;

namespace {

constexpr double kHysteresisSemitones = 15.0 / 100.0;  // past the midpoint
constexpr double kHysteresisHoldS = 0.030;             // sustain before switching
constexpr double kCentreTimeConstantS = 1.0;           // robot octave centre
constexpr double kTonicHysteresisSemitones = 3.0;      // 300 cent
constexpr double kDetuneRateHz = 0.7;
constexpr double kDetuneSigma[3] = {1.0, -1.0, 1.0};
constexpr double kDetunePhase[3] = {0.0, kTwoPiD / 3.0, 2.0 * kTwoPiD / 3.0};

constexpr std::uint16_t kMajorMask = 0b101010110101;
constexpr std::uint16_t kMinorMask = 0b010110101101;
constexpr std::uint16_t kChromaticMask = 0x0FFF;

double hz_to_midi(double hz) { return kMidiA4 + kSemitonesPerOctave * std::log2(hz / kA4Hz); }
double midi_to_hz(double midi) { return kA4Hz * std::exp2((midi - kMidiA4) / kSemitonesPerOctave); }
double semis_to_ratio(double semis) { return std::exp2(semis / kSemitonesPerOctave); }

int floor_mod(int a, int m) { return ((a % m) + m) % m; }

}  // namespace

void PitchTargetController::prepare(double sr) {
  sr_ = sr;
  reset();
}

void PitchTargetController::reset() {
  time_s_ = 0.0;
  have_f0_ = false;
  last_f0_hz_ = 0.0;
  auto_valid_ = false;
  smooth_valid_ = false;
  pend_valid_ = false;
  centre_valid_ = false;
  tonic_valid_ = false;
  held_count_ = 0;
  hold_note_ = -1;
}

void PitchTargetController::set_config(const PitchCorrectionConfig& pc, const HarmonyConfig& hm,
                                       const MidiConfig& midi, float semitones,
                                       float formant_factor, FormantMode mode) {
  const int old_root = pc_.scale == ScaleKind::Chromatic ? 0 : pc_.key_root;
  const int new_root = pc.scale == ScaleKind::Chromatic ? 0 : pc.key_root;
  if (old_root != new_root || pc_.scale != pc.scale) {
    auto_valid_ = false;
    pend_valid_ = false;
    tonic_valid_ = false;
  }
  pc_ = pc;
  hm_ = hm;
  midi_ = midi;
  semitones_ = semitones;
  formant_factor_ = formant_factor;
  fmode_ = mode;
}

void PitchTargetController::apply_note(const NoteEvent& ev) noexcept {
  int idx = -1;
  for (int i = 0; i < held_count_; ++i) {
    if (held_[i] == ev.note) idx = i;
  }
  if (ev.kind == NoteEvent::Kind::On) {
    if (idx >= 0) {
      for (int i = idx; i + 1 < held_count_; ++i) held_[i] = held_[i + 1];
      --held_count_;
    } else if (held_count_ == kMaxHeldNotes) {
      for (int i = 0; i + 1 < held_count_; ++i) held_[i] = held_[i + 1];
      --held_count_;
    }
    held_[held_count_++] = ev.note;
    hold_note_ = -1;
    return;
  }
  if (idx < 0) return;
  for (int i = idx; i + 1 < held_count_; ++i) held_[i] = held_[i + 1];
  --held_count_;
  if (held_count_ == 0) hold_note_ = ev.note;
}

void PitchTargetController::all_notes_off() noexcept {
  held_count_ = 0;
  hold_note_ = -1;
}

bool PitchTargetController::in_scale(int note) const noexcept {
  std::uint16_t mask = kChromaticMask;
  int root = 0;
  if (pc_.scale == ScaleKind::Major) {
    mask = kMajorMask;
    root = pc_.key_root;
  } else if (pc_.scale == ScaleKind::NaturalMinor) {
    mask = kMinorMask;
    root = pc_.key_root;
  }
  return ((mask >> floor_mod(note - root, 12)) & 1) != 0;
}

int PitchTargetController::next_in_scale(int note, int dir) const noexcept {
  int n = note + dir;
  while (!in_scale(n)) n += dir;
  return n;
}

int PitchTargetController::snap(double midi) const noexcept {
  int lo = static_cast<int>(std::floor(midi));
  if (!in_scale(lo)) lo = next_in_scale(lo, -1);
  int hi = static_cast<int>(std::floor(midi)) + 1;
  if (!in_scale(hi)) hi = next_in_scale(hi, 1);
  return (midi - lo) <= (hi - midi) ? lo : hi;
}

int PitchTargetController::scale_step(int note, int steps) const noexcept {
  const int dir = steps >= 0 ? 1 : -1;
  int n = note;
  for (int i = 0; i < std::abs(steps); ++i) n = next_in_scale(n, dir);
  return n;
}

void PitchTargetController::track_auto(double midi, double t) noexcept {
  if (!auto_valid_) {
    auto_valid_ = true;
    auto_note_ = snap(midi);
    pend_valid_ = false;
    return;
  }
  const int dir = midi > auto_note_ ? 1 : -1;
  const int nbr = next_in_scale(auto_note_, dir);
  const double mid = 0.5 * (auto_note_ + nbr);
  if (dir * (midi - mid) < kHysteresisSemitones) {
    pend_valid_ = false;
    return;
  }
  const int cand = snap(midi);
  if (cand == auto_note_) {
    pend_valid_ = false;
    return;
  }
  if (!pend_valid_ || pend_note_ != cand) {
    pend_valid_ = true;
    pend_note_ = cand;
    pend_since_ = t;
  } else if (t - pend_since_ >= kHysteresisHoldS) {
    auto_note_ = cand;
    pend_valid_ = false;
  }
}

void PitchTargetController::track_robot(double midi, double t) noexcept {
  if (!centre_valid_) {
    centre_valid_ = true;
    centre_midi_ = midi;
  } else {
    const double dt = std::max(0.0, t - centre_t_);
    centre_midi_ += (midi - centre_midi_) * (1.0 - std::exp(-dt / kCentreTimeConstantS));
  }
  centre_t_ = t;
  const int root = pc_.scale == ScaleKind::Chromatic ? 0 : pc_.key_root;
  const int nearest =
      root + 12 * static_cast<int>(std::lround((centre_midi_ - root) / kSemitonesPerOctave));
  if (!tonic_valid_) {
    tonic_valid_ = true;
    tonic_ = nearest;
    return;
  }
  if (nearest != tonic_ &&
      std::abs(centre_midi_ - tonic_) > 0.5 * kSemitonesPerOctave + kTonicHysteresisSemitones) {
    tonic_ = nearest;
  }
}

void PitchTargetController::track_estimate(const F0Estimate& e) noexcept {
  if (!e.voiced || !(e.f0_hz > 0.0f)) {
    pend_valid_ = false;
    return;
  }
  const double t = static_cast<double>(e.centre_sample) / sr_;
  const double midi = hz_to_midi(e.f0_hz);
  have_f0_ = true;
  last_f0_hz_ = e.f0_hz;
  if (pc_.mode == PitchCorrectionMode::Auto) track_auto(midi, t);
  if (pc_.mode == PitchCorrectionMode::Robot) track_robot(midi, t);
}

VoiceTargets PitchTargetController::update(const F0Estimate* e, int count,
                                           int block_samples) noexcept {
  for (int i = 0; i < count; ++i) track_estimate(e[i]);

  const double dt = static_cast<double>(block_samples) / sr_;
  const double t0 = time_s_;
  time_s_ += dt;

  const double s = semis_to_ratio(semitones_);
  const bool note_mode = held_count_ > 0 || (midi_.release == MidiRelease::Hold && hold_note_ >= 0);

  // Per voice: pitch (MIDI units) of the absolute target and the relative interval in semitones.
  double target_midi[kMaxVoices] = {};
  double interval[kMaxVoices] = {};
  bool have_target = false;
  double w = 0.0;
  int nv = 1;

  if (note_mode) {
    if (held_count_ > 0) {
      nv = std::min(held_count_, 1 + std::clamp(hm_.voices, 0, kMaxHarmonyVoices));
      for (int v = 0; v < nv; ++v) target_midi[v] = held_[held_count_ - 1 - v];
    } else {
      target_midi[0] = hold_note_;
    }
    have_target = true;
    w = 1.0;
  } else {
    nv = 1 + std::clamp(hm_.voices, 0, kMaxHarmonyVoices);
    bool have_base = false;
    double base = 0.0;
    int base_note = 0;
    switch (pc_.mode) {
      case PitchCorrectionMode::Off:
        if (have_f0_) {
          base_note = snap(hz_to_midi(last_f0_hz_));
          base = base_note;
          have_base = true;
        }
        w = 0.0;
        break;
      case PitchCorrectionMode::Auto:
        if (auto_valid_) {
          const double target = auto_note_;
          const double tau = static_cast<double>(pc_.speed_ms) * 1e-3;
          if (tau <= 0.0 || !smooth_valid_) {
            smooth_valid_ = true;
            auto_smooth_midi_ = target;
          } else {
            auto_smooth_midi_ += (target - auto_smooth_midi_) * (1.0 - std::exp(-dt / tau));
          }
          base_note = auto_note_;
          base = auto_smooth_midi_;
          have_base = true;
        }
        w = std::clamp(static_cast<double>(pc_.amount), 0.0, 1.0);
        break;
      case PitchCorrectionMode::Robot:
        if (tonic_valid_) {
          base_note = tonic_ + 12 * std::clamp(pc_.robot_octave, -1, 1);
          base = base_note;
          have_base = true;
        }
        w = have_base ? 1.0 : 0.0;
        break;
    }
    if (have_base) {
      have_target = true;
      target_midi[0] = base;
      for (int v = 1; v < nv; ++v) {
        interval[v] = scale_step(base_note, hm_.steps[v - 1]) - base_note;
        target_midi[v] = base + interval[v];
      }
    }
  }

  VoiceTargets out;
  out.count = nv;
  for (int v = 0; v < nv; ++v) {
    double d = 1.0;
    if (v > 0) {
      const int j = v - 1;
      const double c = kDetuneSigma[j] * hm_.detune_cents *
                       (0.5 + 0.5 * std::sin(kTwoPiD * kDetuneRateHz * t0 + kDetunePhase[j]));
      d = std::exp2(c / kCentsPerOctave);
    }
    const double sd = s * d;
    const double ratio = sd * semis_to_ratio(interval[v]);
    const double target_hz = have_target ? midi_to_hz(target_midi[v]) * sd : 0.0;
    const double wv = have_target ? w : 0.0;

    VoiceTarget& vt = out.voices[v];
    vt.ratio = static_cast<float>(ratio);
    vt.target_hz = static_cast<float>(target_hz);
    vt.absolute_weight = static_cast<float>(wv);
    if (fmode_ == FormantMode::Absolute) {
      vt.formant_alpha = formant_factor_;
    } else {
      double out_over_in = ratio;
      if (have_f0_ && wv > 0.0 && target_hz > 0.0) {
        out_over_in =
            std::pow(last_f0_hz_ * ratio, 1.0 - wv) * std::pow(target_hz, wv) / last_f0_hz_;
      }
      vt.formant_alpha = static_cast<float>(out_over_in * formant_factor_);
    }
    vt.gain = v == 0 ? (nv > 1 ? hm_.lead_level : 1.0f) : hm_.level;
    vt.delay_ms = v == 0 ? 0.0f : hm_.delay_ms;
    vt.active = true;
  }
  return out;
}

}  // namespace sonare::editing::voice_changer
