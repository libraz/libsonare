#include "midi/synth/fm_voice.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// Operator wiring per FmAlgorithm: mod_mask[i] = bitmask of operators that
/// phase-modulate operator i; carrier_mask = operators summed to the output.
struct FmAlgorithmSpec {
  uint8_t mod_mask[kMaxFmOperators];
  uint8_t carrier_mask;
};

constexpr FmAlgorithmSpec kFmAlgorithms[] = {
    {{0x02, 0x00, 0x00, 0x00}, 0x01},  // kStack2:  op1 -> op0
    {{0x02, 0x04, 0x00, 0x00}, 0x01},  // kStack3:  op2 -> op1 -> op0
    {{0x02, 0x04, 0x08, 0x00}, 0x01},  // kStack4:  op3 -> op2 -> op1 -> op0
    {{0x02, 0x00, 0x08, 0x00}, 0x05},  // kPair2x2: (1->0) + (3->2)
    {{0x06, 0x00, 0x00, 0x00}, 0x01},  // kBright3: (1 + 2) -> op0
    {{0x00, 0x00, 0x00, 0x00}, 0x03},  // kAdd2:    op0 + op1 carriers
};

float note_to_hz(float note) noexcept { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

}  // namespace

void FmVoiceCore::start(const FmPatchParams& params, double sample_rate, uint8_t note,
                        uint8_t velocity) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float base_hz = note_to_hz(static_cast<float>(note & 0x7Fu));
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float octaves_above_middle = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;

  const size_t algo = static_cast<size_t>(
      std::clamp(static_cast<int>(params.algorithm), 0,
                 static_cast<int>(sizeof(kFmAlgorithms) / sizeof(kFmAlgorithms[0])) - 1));
  const FmAlgorithmSpec& spec = kFmAlgorithms[algo];
  carrier_mask_ = spec.carrier_mask;
  int carriers = 0;
  for (int i = 0; i < kMaxFmOperators; ++i) {
    mod_mask_[i] = spec.mod_mask[i];
    if (carrier_mask_ & (1u << i)) ++carriers;
  }
  carrier_norm_ = carriers > 0 ? 1.0f / static_cast<float>(carriers) : 1.0f;

  for (int i = 0; i < kMaxFmOperators; ++i) {
    const FmOperatorParams& p = params.ops[static_cast<size_t>(i)];
    Operator& op = ops_[static_cast<size_t>(i)];
    op.phase = 0.0;
    op.prev1 = 0.0f;
    op.prev2 = 0.0f;
    const float ratio = std::clamp(p.ratio, 0.0f, 64.0f);
    const float detune = std::exp2(std::clamp(p.detune_cents, -1200.0f, 1200.0f) / 1200.0f);
    op.base_inc = static_cast<float>(static_cast<double>(base_hz) * ratio * detune / sr);
    // Velocity -> level (modulation index for modulators = brightness).
    const float vel_amount = std::clamp(p.vel_to_level, 0.0f, 1.0f);
    op.level = std::max(0.0f, p.level) * ((1.0f - vel_amount) + vel_amount * vel01);
    op.feedback = std::clamp(p.feedback, 0.0f, 4.0f);
    // Key-rate scaling: decay/release shorten going up the keyboard.
    DahdsrConfig env = p.env;
    const float krs = std::clamp(p.key_rate_scale, 0.0f, 1.0f);
    if (krs > 0.0f && octaves_above_middle != 0.0f) {
      const float scale = std::exp2(-krs * octaves_above_middle);
      env.decay_ms = std::max(1.0f, env.decay_ms * scale);
      env.release_ms = std::max(1.0f, env.release_ms * scale);
    }
    op.env.configure(sr, env);
    op.env.kill();
    op.env.note_on();
  }
}

float FmVoiceCore::render(float pitch_ratio) noexcept {
  // Per-operator outputs this sample (phase-modulation depth in radians).
  float outputs[kMaxFmOperators] = {0.0f, 0.0f, 0.0f, 0.0f};
  float mix = 0.0f;
  // Highest index first: modulators always precede their consumers.
  for (int i = kMaxFmOperators - 1; i >= 0; --i) {
    Operator& op = ops_[static_cast<size_t>(i)];
    if (op.level <= 0.0f) continue;
    float mod_in = 0.0f;
    for (int m = i + 1; m < kMaxFmOperators; ++m) {
      if (mod_mask_[i] & (1u << m)) mod_in += outputs[m];
    }
    if (op.feedback > 0.0f) mod_in += op.feedback * 0.5f * (op.prev1 + op.prev2);
    const float env = op.env.next();
    const float y = std::sin(kTwoPi * static_cast<float>(op.phase) + mod_in) * env * op.level;
    op.phase += static_cast<double>(op.base_inc * pitch_ratio);
    if (op.phase >= 1.0) op.phase -= std::floor(op.phase);
    op.prev2 = op.prev1;
    op.prev1 = y;
    outputs[i] = y;
    if (carrier_mask_ & (1u << i)) mix += y;
  }
  return mix * carrier_norm_;
}

void FmVoiceCore::release() noexcept {
  for (Operator& op : ops_) op.env.note_off();
}

void FmVoiceCore::kill() noexcept {
  for (Operator& op : ops_) op.env.kill();
}

}  // namespace sonare::midi::synth
