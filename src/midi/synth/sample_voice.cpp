#include "midi/synth/sample_voice.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kCentsPerSemitone;

bool SampleVoiceCore::start(const SamplePatchParams& p, double sample_rate, uint8_t note,
                            uint8_t velocity) noexcept {
  // Cleared before the lookup so a failed start cannot leave a reused slot
  // placed and levelled by the note that last played through it.
  finished_ = true;
  gain_ = 0.0f;
  pan_units_ = 0.0f;
  if (bank_ == nullptr || sample_rate <= 0.0) return false;

  const SampleZone* zone = bank_->find(p.set_index, note, velocity);
  if (zone == nullptr) return false;

  SampleRegion region = zone->region;
  if (p.loop_override >= 0) {
    region.loop_mode =
        (p.loop_override == 1 || p.loop_override == 3) && region.loop_end > region.loop_start
            ? p.loop_override
            : 0;
  }

  const double span = static_cast<double>(region.end) - static_cast<double>(region.start);
  const double offset = std::clamp(static_cast<double>(p.start_offset01), 0.0, 0.999) * span;
  reader_.start(bank_->pool(), region, offset);

  const float key_cents =
      p.key_track
          ? kCentsPerSemitone * (static_cast<float>(note) - static_cast<float>(zone->root_key))
          : 0.0f;
  const float cents = key_cents + zone->tune_cents;
  const double rate_ratio = zone->source_rate > 0.0 ? zone->source_rate / sample_rate : 1.0;
  increment_ = rate_ratio * std::exp2(static_cast<double>(cents) / kCentsPerOctave);

  gain_ = zone->gain * p.level;
  pan_units_ = zone->pan_units;
  finished_ = false;
  return true;
}

float SampleVoiceCore::render(float pitch_ratio, bool key_down) noexcept {
  if (finished_ || !reader_.valid()) return 0.0f;

  const bool looping = reader_.looping(key_down);
  if (!reader_.wrap(looping)) {
    finished_ = true;
    return 0.0f;
  }
  const float sample = reader_.read(looping);
  reader_.advance(increment_ * static_cast<double>(pitch_ratio));
  return sample * gain_;
}

}  // namespace sonare::midi::synth
