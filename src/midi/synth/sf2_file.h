#pragma once

/// @file sf2_file.h
/// @brief SoundFont 2 (SF2) file parser and in-memory model: RIFF
///        INFO / sdta / pdta chunks decoded into presets, instruments, zones
///        (generators + modulators), sample headers and a float sample pool.
///
/// Scope (build-plan P1): parsing and the sample-PCM pool only — the SF2
/// voice model that *plays* this data is layered on top (sf2_voice /
/// sf2_player). Reference: SoundFont 2.04 Technical Specification.
///
/// Threading: parse() runs on the CONTROL thread and allocates (the only
/// place). The resulting Sf2File is immutable afterwards; the audio thread
/// reads the sample pool and zone tables read-only.
///
/// Robustness: every chunk read is bounds-checked; malformed or truncated
/// files fail with an error message, never crash. 16-bit PCM is required,
/// the optional sm24 chunk upgrades to 24-bit precision when well-formed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonare::midi::synth {

/// SF2 generator operators (SoundFont 2.04 §8.1.2; subset used by the player,
/// raw values preserved for the rest).
enum Sf2GenOper : uint16_t {
  kGenStartAddrsOffset = 0,
  kGenEndAddrsOffset = 1,
  kGenStartloopAddrsOffset = 2,
  kGenEndloopAddrsOffset = 3,
  kGenStartAddrsCoarseOffset = 4,
  kGenModLfoToPitch = 5,
  kGenVibLfoToPitch = 6,
  kGenModEnvToPitch = 7,
  kGenInitialFilterFc = 8,
  kGenInitialFilterQ = 9,
  kGenModLfoToFilterFc = 10,
  kGenModEnvToFilterFc = 11,
  kGenEndAddrsCoarseOffset = 12,
  kGenModLfoToVolume = 13,
  kGenChorusEffectsSend = 15,
  kGenReverbEffectsSend = 16,
  kGenPan = 17,
  kGenDelayModLfo = 21,
  kGenFreqModLfo = 22,
  kGenDelayVibLfo = 23,
  kGenFreqVibLfo = 24,
  kGenDelayModEnv = 25,
  kGenAttackModEnv = 26,
  kGenHoldModEnv = 27,
  kGenDecayModEnv = 28,
  kGenSustainModEnv = 29,
  kGenReleaseModEnv = 30,
  kGenKeynumToModEnvHold = 31,
  kGenKeynumToModEnvDecay = 32,
  kGenDelayVolEnv = 33,
  kGenAttackVolEnv = 34,
  kGenHoldVolEnv = 35,
  kGenDecayVolEnv = 36,
  kGenSustainVolEnv = 37,
  kGenReleaseVolEnv = 38,
  kGenKeynumToVolEnvHold = 39,
  kGenKeynumToVolEnvDecay = 40,
  kGenInstrument = 41,
  kGenKeyRange = 43,
  kGenVelRange = 44,
  kGenStartloopAddrsCoarseOffset = 45,
  kGenKeynum = 46,
  kGenVelocity = 47,
  kGenInitialAttenuation = 48,
  kGenEndloopAddrsCoarseOffset = 50,
  kGenCoarseTune = 51,
  kGenFineTune = 52,
  kGenSampleId = 53,
  kGenSampleModes = 54,
  kGenScaleTuning = 56,
  kGenExclusiveClass = 57,
  kGenOverridingRootKey = 58,
};

/// One generator record (operator + 16-bit amount; range operators pack
/// lo/hi bytes into the amount).
struct Sf2Gen {
  uint16_t oper = 0;
  int16_t amount = 0;

  uint8_t range_lo() const noexcept { return static_cast<uint8_t>(amount & 0xFF); }
  uint8_t range_hi() const noexcept { return static_cast<uint8_t>((amount >> 8) & 0xFF); }
};

/// One modulator record (SoundFont 2.04 §8.2). Stored raw; the default
/// modulator set is synthesized by the voice layer.
struct Sf2Mod {
  uint16_t src_oper = 0;
  uint16_t dest_oper = 0;
  int16_t amount = 0;
  uint16_t amount_src_oper = 0;
  uint16_t transform_oper = 0;
};

/// A preset or instrument zone: its generator/modulator lists plus decoded
/// convenience fields. A GLOBAL zone (no terminal instrument/sampleID
/// generator) supplies defaults for the zones that follow it.
struct Sf2Zone {
  std::vector<Sf2Gen> gens;
  std::vector<Sf2Mod> mods;
  uint8_t key_lo = 0;
  uint8_t key_hi = 127;
  uint8_t vel_lo = 0;
  uint8_t vel_hi = 127;
  /// Preset zones: target instrument index, or -1 (global zone).
  int32_t instrument = -1;
  /// Instrument zones: target sample index, or -1 (global zone).
  int32_t sample = -1;

  bool is_global() const noexcept { return instrument < 0 && sample < 0; }
  bool matches(uint8_t key, uint8_t velocity) const noexcept {
    return key >= key_lo && key <= key_hi && velocity >= vel_lo && velocity <= vel_hi;
  }
  /// Last occurrence of @p oper in this zone, or nullptr.
  const Sf2Gen* find_gen(uint16_t oper) const noexcept;
};

/// Sample header (shdr record). Indices address the file-wide sample pool.
struct Sf2Sample {
  std::string name;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  uint32_t sample_rate = 0;
  uint8_t original_pitch = 60;
  int8_t correction = 0;  // pitch correction in cents
  uint16_t sample_link = 0;
  uint16_t sample_type = 1;  // 1 mono, 2 right, 4 left, 8 linked, 0x8000 ROM flag

  bool is_rom() const noexcept { return (sample_type & 0x8000u) != 0; }
};

struct Sf2Instrument {
  std::string name;
  std::vector<Sf2Zone> zones;  // zone 0 may be global
};

struct Sf2Preset {
  std::string name;
  uint16_t program = 0;
  uint16_t bank = 0;
  std::vector<Sf2Zone> zones;  // zone 0 may be global
};

/// Parsed SoundFont: the pdta object model plus the sdta PCM converted to
/// float [-1, 1] (the "sample pool"; shdr start/end index into it).
class Sf2File {
 public:
  /// CONTROL thread: parse @p size bytes of SF2 data. Returns false and sets
  /// @p error (if non-null) on malformed input; *this is left empty then.
  bool parse(const uint8_t* data, size_t size, std::string* error);

  const std::vector<Sf2Preset>& presets() const noexcept { return presets_; }
  const std::vector<Sf2Instrument>& instruments() const noexcept { return instruments_; }
  const std::vector<Sf2Sample>& samples() const noexcept { return samples_; }

  /// Float PCM pool ([-1,1]); Sf2Sample::start/end index into this.
  const std::vector<float>& sample_pool() const noexcept { return sample_pool_; }

  /// Index of the preset at (bank, program), or -1.
  int find_preset(uint16_t bank, uint16_t program) const noexcept;

  bool empty() const noexcept { return presets_.empty(); }
  void clear();

 private:
  std::vector<Sf2Preset> presets_;
  std::vector<Sf2Instrument> instruments_;
  std::vector<Sf2Sample> samples_;
  std::vector<float> sample_pool_;
};

}  // namespace sonare::midi::synth
