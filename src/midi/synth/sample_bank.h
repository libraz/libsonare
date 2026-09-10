#pragma once

/// @file sample_bank.h
/// @brief Host-supplied PCM for the NativeSynth sample engine: one float pool,
///        a tuning/loop header per sample, and key/velocity zones grouped into
///        sets (one set is one playable keymap).
///
/// The SF2 player has its own container and its own generator model; this is
/// the path for PCM a caller prepared elsewhere, with nothing in the way but a
/// keymap. A bank is built on the control thread and then read as immutable
/// data — the audio thread only resolves zones and reads samples.
///
/// A zone's region is validated as it is added (loop points clamped inside the
/// sample, a loop mode dropped when the loop is empty), which is the invariant
/// SampleReader relies on to skip per-sample range checks.

#include <cstdint>
#include <vector>

#include "midi/synth/sample_reader.h"

namespace sonare::midi::synth {

/// Tuning and looping for one sample, in units relative to the sample itself.
struct SampleDesc {
  /// Key at which the sample sounds at its recorded pitch.
  uint8_t root_key = 60;
  float fine_tune_cents = 0.0f;
  /// Rate the sample was recorded at; 0 means "the output rate", so a bank
  /// built without rate information plays back unresampled.
  double source_rate = 0.0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  /// SF2 sampleModes: 0 = no loop, 1 = continuous, 3 = loop while key down.
  int loop_mode = 0;
};

/// One key/velocity rectangle mapped onto a sample.
struct SampleZoneDesc {
  uint8_t key_lo = 0;
  uint8_t key_hi = 127;
  uint8_t vel_lo = 1;
  uint8_t vel_hi = 127;
  uint32_t sample_index = 0;
  float tune_cents = 0.0f;
  float gain = 1.0f;
  /// SF2 pan units (-500..500), the same scale the voice mixer uses.
  float pan_units = 0.0f;
};

/// A zone with its pool region and tuning resolved, ready for the audio thread.
struct SampleZone {
  SampleRegion region;
  uint8_t key_lo = 0;
  uint8_t key_hi = 127;
  uint8_t vel_lo = 1;
  uint8_t vel_hi = 127;
  uint8_t root_key = 60;
  float tune_cents = 0.0f;
  float gain = 1.0f;
  float pan_units = 0.0f;
  /// source_rate / output_rate is folded in at note-on, not here, because a
  /// bank outlives the rate any one instrument prepared at.
  double source_rate = 0.0;
};

/// Samples plus keymaps, owned by the host and handed to NativeSynth.
class SampleBank {
 public:
  /// CONTROL thread. Copies @p n_frames mono samples into the pool and returns
  /// the sample's index. Returns false on empty or null data. The pool moves as
  /// it grows, so every sounding voice's reader dangles: finish adding before
  /// anything plays.
  bool add_sample(const float* data, size_t n_frames, const SampleDesc& desc, uint32_t* out_index);

  /// CONTROL thread. Appends a zone to @p set, creating intervening sets. False
  /// when the zone names a sample that is not in the bank or an empty range.
  bool add_zone(uint32_t set, const SampleZoneDesc& zone);

  size_t sample_count() const noexcept { return samples_.size(); }
  size_t set_count() const noexcept { return sets_.size(); }
  /// Frames held across every sample, for a caller enforcing a budget.
  size_t pool_size() const noexcept { return pool_.size(); }
  const float* pool() const noexcept { return pool_.data(); }

  /// AUDIO thread: the first zone of @p set covering (@p key, @p velocity), or
  /// nullptr when the set does not exist or nothing covers the note.
  const SampleZone* find(int32_t set, uint8_t key, uint8_t velocity) const noexcept;

 private:
  /// Pool region and header of one added sample.
  struct Sample {
    SampleRegion region;
    uint8_t root_key = 60;
    float fine_tune_cents = 0.0f;
    double source_rate = 0.0;
  };

  std::vector<float> pool_;
  std::vector<Sample> samples_;
  std::vector<std::vector<SampleZone>> sets_;
};

}  // namespace sonare::midi::synth
