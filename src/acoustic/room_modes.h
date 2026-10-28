#pragma once

/// @file room_modes.h
/// @brief Low-frequency modal region of a shoebox RIR's late field.
///
/// Below the Schroeder frequency (2000 sqrt(T60 / V) Hz) a room's reverberation is a
/// handful of resolved standing waves, not a diffuse field, so the statistical noise tail is
/// physically wrong there: it has no ringing modes, no pitch to the bass decay, and no
/// dependence on where the source and listener stand. This replaces the tail's band below the
/// crossover with the rigid-wall eigenmodes of the box, each ringing at its eigenfrequency and
/// decaying at the room's RT60 there. A mode's amplitude is the product of its shapes at the
/// source and the receiver over its own normalization and frequency, so a listener at a node
/// hears it vanish, and a receiver pair hears the bass with the coherence the geometry gives.
///
/// The level is set from the diffuse tail it replaces: averaged over positions, the modal band
/// carries the same energy as the noise band it removes, so the early/late balance and the
/// late tail's level matching are unchanged on average.

#include <cstddef>
#include <vector>

#include "acoustic/late_reverb.h"
#include "acoustic/room_model.h"

namespace sonare::acoustic {

/// @brief One rigid-wall eigenmode of a shoebox.
struct RoomMode {
  int nx = 0;
  int ny = 0;
  int nz = 0;
  float freq_hz = 0.0f;
  float decay_per_s = 0.0f;  ///< amplitude decay rate ln(1000) / RT60(freq_hz)
};

/// @brief The modes a late tail's low band is rebuilt from, and where the band ends.
struct ModalPlan {
  float crossover_hz = 0.0f;  ///< 0 when the room has no modal region at this rate
  std::vector<RoomMode> modes;
};

/// @brief Schroeder frequency (Hz) of a room of volume @p volume_m3 with reverberation time
///        @p rt60_s; 0 for a non-positive input.
float schroeder_frequency_hz(float rt60_s, float volume_m3) noexcept;

/// @brief Modes of @p room below the band they must fill, with their decay from @p rt.
///
/// The crossover is the Schroeder frequency at the room's RT60 there. Modes are enumerated up
/// to the frequency at which the crossover's lowpass has fallen past audibility, and the
/// crossover is lowered when that would exceed the mode budget or the rendering work over a
/// @p length-sample tail. The plan is empty (crossover 0) when no mode lies below the
/// crossover — a large hall, whose bass is already diffuse.
ModalPlan plan_room_modes(const ShoeboxRoom& room, const ReverbTime& rt, int sample_rate,
                          std::size_t length);

/// @brief Removes the band below the plan's crossover from @p late, in place, and returns the
///        removed band's energy (sum of squares). No-op returning 0 for an empty plan.
double remove_modal_band(std::vector<float>& late, const ModalPlan& plan, int sample_rate);

/// @brief Mode amplitude scale that gives the modal band, averaged over source and receiver
///        positions, @p band_energy over @p length samples.
float modal_level(const ModalPlan& plan, double band_energy, std::size_t length, int sample_rate);

/// @brief Adds the plan's modes for @p placement, scaled by @p level and weighted by the
///        crossover lowpass so the result is complementary to @ref remove_modal_band.
void add_room_modes(std::vector<float>& late, const ModalPlan& plan, const ShoeboxRoom& room,
                    const SourceListener& placement, float level, int sample_rate);

}  // namespace sonare::acoustic
