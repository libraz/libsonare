#pragma once

/// @file destination_instrument.h
/// @brief Value-owned destination instrument data for arrangement projects.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sonare::arrangement {

/// One patch modulation route, stored without depending on a synth runtime type.
struct DestinationSynthModRouting {
  int source = 0;
  int destination = 0;
  float depth = 0.0f;

  bool operator==(const DestinationSynthModRouting& other) const noexcept {
    return source == other.source && destination == other.destination && depth == other.depth;
  }
};

/// Sparse synth patch overrides keyed by the public parameter names.
using DestinationPatchField = std::variant<double, std::string>;

struct DestinationSynthInstrument {
  std::string preset;
  std::string engine_mode;
  std::map<std::string, DestinationPatchField> fields;
  std::map<std::string, double> engine_params;
  std::vector<DestinationSynthModRouting> mod_routings;
  bool clears_base_mod_routings = false;
  bool use_gm_programs = false;

  bool operator==(const DestinationSynthInstrument& other) const noexcept {
    return preset == other.preset && engine_mode == other.engine_mode && fields == other.fields &&
           engine_params == other.engine_params && mod_routings == other.mod_routings &&
           clears_base_mod_routings == other.clears_base_mod_routings &&
           use_gm_programs == other.use_gm_programs;
  }
};

/// Built-in oscillator patch values, kept independent from the synth runtime.
struct DestinationBuiltinInstrument {
  int waveform = 0;
  float gain = 0.2f;
  float attack_ms = 5.0f;
  float decay_ms = 60.0f;
  float sustain = 0.7f;
  float release_ms = 120.0f;
  int polyphony = 16;

  bool operator==(const DestinationBuiltinInstrument& other) const noexcept {
    return waveform == other.waveform && gain == other.gain && attack_ms == other.attack_ms &&
           decay_ms == other.decay_ms && sustain == other.sustain &&
           release_ms == other.release_ms && polyphony == other.polyphony;
  }
};

/// SoundFont player config values, kept independent from the player runtime.
struct DestinationSf2Instrument {
  float gain = 0.5f;
  int polyphony = 48;
  int prefer_model_for_modeled_families = 0;
  int clear_bank_rig = 0;
  int gs_efx_realization = 0;
  std::optional<std::string> sound_font_uri;

  bool operator==(const DestinationSf2Instrument& other) const noexcept {
    return gain == other.gain && polyphony == other.polyphony &&
           prefer_model_for_modeled_families == other.prefer_model_for_modeled_families &&
           clear_bank_rig == other.clear_bank_rig &&
           gs_efx_realization == other.gs_efx_realization && sound_font_uri == other.sound_font_uri;
  }
};

using DestinationInstrument = std::variant<DestinationSynthInstrument, DestinationBuiltinInstrument,
                                           DestinationSf2Instrument>;

struct DestinationMidiBank {
  int32_t msb = 0;
  int32_t lsb = 0;

  bool operator==(const DestinationMidiBank& other) const noexcept {
    return msb == other.msb && lsb == other.lsb;
  }
};

/// One destination's instrument and optional MIDI bank/program override.
struct DestinationInstrumentBinding {
  DestinationInstrument instrument;
  std::optional<DestinationMidiBank> bank;
  std::optional<int32_t> program;

  bool operator==(const DestinationInstrumentBinding& other) const noexcept {
    return instrument == other.instrument && bank == other.bank && program == other.program;
  }
};

/// Refuses a destination instrument whose model values cannot be used by the
/// corresponding instrument runtime.
void validate_destination_instrument(const DestinationInstrumentBinding& binding);

}  // namespace sonare::arrangement
