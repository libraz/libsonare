#pragma once

/// @file fdn_plate.h
/// @brief Dense inharmonic plate resonator (Jot & Chaigne feedback delay
///        network) for the NativeSynth percussion voice — the mechanism a
///        struck cymbal, gong or bell plate needs and a small modal bank
///        cannot supply.
///
/// A modal bank spends one resonator per partial, so a few modes can only put
/// energy in a few places and more gain makes them louder rather than the field
/// denser — it reads as a tuned bar however it is voiced. Measured against a
/// sampled kit the gap is a corner, not a level: a closed hi-hat's reference
/// holds its fine structure correlated at +0.47 with only 6 % of its band energy
/// in its twenty strongest bins, where a six-mode bank at full gain reaches +0.34
/// with 93 % — the opposite corner. Independent fixed partials do not reach that
/// spread until there are several hundred of them.
///
/// An FDN buys the density for the cost of its delay lines instead: N lines
/// closed through a lossless Householder matrix resonate at one pole per delay
/// sample, so eight lines of a few hundred samples put thousands of partials in
/// the band for eight multiply-accumulates. The lengths are prime, so no two
/// share a period and the partials land inharmonically. Decay follows Jot — a
/// per-line gain sets the low-frequency T60 independently of that line's length,
/// and a one-pole loss in each loop gives the top of the band its own, which is
/// the difference between a ride that shimmers and a plate that thuds.
///
/// RT contract: start() and process() are allocation-free (the lines are
/// fixed-size members). Determinism: fixed lengths, no RNG.

#include <algorithm>
#include <array>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

namespace fdn_plate_detail {

/// The smallest prime at least @p n, for the delay-line lengths. Prime lengths
/// are what keep the lines from sharing a period: two lines whose lengths have
/// a common factor place partials on top of each other, which spends taps
/// without buying density.
inline int next_prime_at_least(int n) noexcept {
  if (n <= 2) return 2;
  for (int c = n | 1;; c += 2) {
    bool prime = true;
    for (int d = 3; d * d <= c; d += 2) {
      if (c % d == 0) {
        prime = false;
        break;
      }
    }
    if (prime) return c;
  }
}

}  // namespace fdn_plate_detail

class FdnPlate {
 public:
  /// Delay lines in the network. Eight is the smallest count whose Householder
  /// mixing reaches full echo density within one pass of the shortest line;
  /// fewer leaves an audible flutter at the loop period.
  static constexpr int kLines = 8;

  /// Longest delay line, in samples. It sets two things at once: the lowest
  /// partial the network can place and, summed over the lines, the ceiling on
  /// how many partials it holds — and the two pull against each other, since
  /// spending the budget on one long line buys reach and spending it on eight
  /// buys density. A line array this size is the voice's largest member, so
  /// this is the shortest length that still reaches a plate's mode density
  /// while placing a partial as low as any cymbal radiates (70 Hz at 48 kHz,
  /// 140 Hz at 96 kHz) rather than a comfortable round number.
  static constexpr int kMaxDelay = 1024;

  /// Configures the network. @p low_hz is the lowest partial (it scales every
  /// line), @p t60_s the reverberation time at the bottom of the band,
  /// @p hf_ratio the reverberation time at Nyquist as a fraction of it —
  /// 1 leaves the top of the band undamped, and values above 1 are clamped
  /// there because a loop that gains with frequency does not settle — and
  /// @p air_hz the top of the band the plate responds in at all (0 = up to
  /// Nyquist).
  ///
  /// @p air_hz and @p hf_ratio are not the same control and neither substitutes
  /// for the other. The ratio says how fast the top of the band dies once it is
  /// ringing; the bound says whether it rings. A network has poles up to Nyquist
  /// and a strike is broadband, so with no bound the plate answers a hi-hat's
  /// wash 9 dB over its reference in the top two third-octave bands — and
  /// damping that away instead takes the whole top with it, which the same
  /// piece needs 100 ms after the strike. One is a ceiling and the other is a
  /// decay, and a piece that speaks under a ceiling needs the ceiling moved.
  ///
  /// A @p t60_s at or below zero leaves the plate inactive.
  void start(double sample_rate, float low_hz, float t60_s, float hf_ratio, float air_hz) noexcept {
    const float sr = static_cast<float>(sample_rate > 0.0 ? sample_rate : 48000.0);
    active_ = t60_s > 0.0f;
    if (!active_) {
      pos_.fill(0);
      return;
    }
    const float t60_dc = std::max(0.01f, t60_s);
    const float hf = std::clamp(hf_ratio, 0.01f, 1.0f);
    const float t60_hf = t60_dc * hf;

    // Band bounds on the excitation rather than on the output: the two are the
    // same signal through a linear network, and bounding what goes in means the
    // poles outside the band are never rung in the first place. Each is two
    // cascaded one-poles, because one leaves a 6 dB/octave skirt that is most
    // of what there was to remove.
    //
    // The floor is not optional and has no field of its own, because it is not
    // a voicing choice. A delay line of length L is a comb whose first peak is
    // at DC, so an unbounded network answers a broadband strike with a burst of
    // sub-audio that decays over the whole t60 — against a sampled kit that put
    // every cymbal 50 to 60 dB over its reference below 125 Hz, where a plate
    // radiates nothing at all. `low_hz` already names the lowest partial the
    // plate has, so it is also the frequency below which the plate does not
    // respond.
    lo_a_ = 1.0f - std::exp(-sonare::constants::kTwoPi * std::min(low_hz, 0.45f * sr) / sr);
    lo1_ = 0.0f;
    lo2_ = 0.0f;
    bound_ = air_hz > 0.0f;
    if (bound_) {
      const float f = std::min(air_hz, 0.45f * sr);
      air_a_ = 1.0f - std::exp(-sonare::constants::kTwoPi * f / sr);
      air1_ = 0.0f;
      air2_ = 0.0f;
    }

    // The base line is the period of the lowest partial; the rest fan out above
    // it on incommensurate multiples, so the network's poles interleave instead
    // of clustering.
    //
    // The whole fan is scaled to fit rather than each line being clipped to the
    // array: clipping collapses every line that overruns onto the same length,
    // and lines of equal length are one comb repeated, which is a ringing tube
    // and not a plate. Scaling loses the requested pitch and keeps the network.
    static constexpr std::array<float, kLines> kSpread = {1.0f,  1.055f, 1.11f, 1.17f,
                                                          1.23f, 1.30f,  1.37f, 1.45f};
    // The prime search steps up from each length, so the fan stops short of the
    // array end by more than any prime gap below kMaxDelay.
    const float headroom = static_cast<float>(kMaxDelay - 32) / kSpread[kLines - 1];
    const float base = std::min(sr / std::max(20.0f, low_hz), headroom);
    for (int i = 0; i < kLines; ++i) {
      const auto k = static_cast<size_t>(i);
      const int want = std::clamp(static_cast<int>(base * kSpread[k]), 8, kMaxDelay - 32);
      const int len = std::min(fdn_plate_detail::next_prime_at_least(want), kMaxDelay);
      len_[k] = len;

      // Jot: the round-trip gain that reaches t60 after len samples, so every
      // line decays at the same rate however long it is.
      const float exponent = -3.0f * static_cast<float>(len) / sr;
      gain_[k] = std::pow(10.0f, exponent / t60_dc);
      // The loop loss needed at Nyquist for the top of the band to reach its
      // own (shorter) t60, realized as a one-pole whose Nyquist magnitude is
      // (1 - a) / (1 + a).
      const float nyquist_gain =
          std::min(1.0f, std::pow(10.0f, exponent / t60_hf) / std::max(1.0e-12f, gain_[k]));
      damp_[k] = (1.0f - nyquist_gain) / (1.0f + nyquist_gain);
      lp_[k] = 0.0f;

      // Each line is its own ring, so only the samples it uses need clearing —
      // a strike does not inherit the previous one's tail, and the note-on cost
      // follows the tuning rather than the array size.
      std::fill_n(lines_[k].begin(), len, 0.0f);
      pos_[k] = 0;
    }
  }

  /// Feeds one sample of excitation in and returns one sample of plate.
  float process(float x) noexcept {
    if (!active_) return 0.0f;
    lo1_ += (x - lo1_) * lo_a_;
    x -= lo1_;
    lo2_ += (x - lo2_) * lo_a_;
    x -= lo2_;
    if (bound_) {
      air1_ += (x - air1_) * air_a_;
      air2_ += (air1_ - air2_) * air_a_;
      x = air2_;
    }

    // In a ring of len samples the value sitting at the write position is the
    // one written len samples ago, so reading before writing is the delay.
    std::array<float, kLines> s{};
    float sum = 0.0f;
    for (int i = 0; i < kLines; ++i) {
      const auto k = static_cast<size_t>(i);
      s[k] = lines_[k][static_cast<size_t>(pos_[k])];
      sum += s[k];
    }

    // Householder mixing, H = I - (2/N) * 1 * 1^T. Orthogonal, so the matrix
    // itself is lossless and every decay in the network comes from the
    // per-line gain and loss below it rather than from the mixing.
    const float shared = sum * (2.0f / static_cast<float>(kLines));
    float out = 0.0f;
    for (int i = 0; i < kLines; ++i) {
      const auto k = static_cast<size_t>(i);
      float v = (s[k] - shared) * gain_[k];
      lp_[k] = v * (1.0f - damp_[k]) + lp_[k] * damp_[k];
      v = lp_[k];
      lines_[k][static_cast<size_t>(pos_[k])] = v + x * kInputSign[k];
      if (++pos_[k] >= len_[k]) pos_[k] = 0;
      out += s[k] * kOutputSign[k];
    }
    return out * (1.0f / std::sqrt(static_cast<float>(kLines)));
  }

  void reset() noexcept {
    active_ = false;
    bound_ = false;
    air1_ = 0.0f;
    air2_ = 0.0f;
    lo1_ = 0.0f;
    lo2_ = 0.0f;
    pos_.fill(0);
    lp_.fill(0.0f);
  }

  bool active() const noexcept { return active_; }

 private:
  // Orthogonal sign patterns: the excitation enters on one and the pickup
  // reads on the other, so what leaves the network is not a copy of what
  // entered it.
  static constexpr std::array<float, kLines> kInputSign = {1.0f, -1.0f, 1.0f, -1.0f,
                                                           1.0f, -1.0f, 1.0f, -1.0f};
  static constexpr std::array<float, kLines> kOutputSign = {1.0f, 1.0f, -1.0f, -1.0f,
                                                            1.0f, 1.0f, -1.0f, -1.0f};

  std::array<std::array<float, kMaxDelay>, kLines> lines_{};
  std::array<int, kLines> len_{};
  std::array<int, kLines> pos_{};
  std::array<float, kLines> gain_{};
  std::array<float, kLines> damp_{};
  std::array<float, kLines> lp_{};
  float air_a_ = 0.0f;
  float air1_ = 0.0f;
  float air2_ = 0.0f;
  float lo_a_ = 0.0f;
  float lo1_ = 0.0f;
  float lo2_ = 0.0f;
  bool bound_ = false;
  bool active_ = false;
};

}  // namespace sonare::midi::synth
