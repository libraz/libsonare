#pragma once

/// @file track_profile.h
/// @brief Per-track analysis for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing in this header is
///          realtime-safe: it allocates, runs an STFT, and is measured in
///          milliseconds per track. Never call it from `process()`.
///
/// @details The assistant analyses tracks and returns parameters. It never
///          processes or emits audio; applying a suggestion is the caller's
///          explicit second step through `sonare_mixer_from_scene_json`.

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "mastering/assistant/audio_profile.h"

namespace sonare::mixing::assistant {

/// @brief Number of analysis bands, matching @ref
///        mastering::assistant::SpectralProfile.
/// @details The mixing assistant deliberately reuses the mastering profile's
///          band split rather than defining a second one. Two band grids would
///          make a masking figure and a spectral figure incomparable.
inline constexpr int kBandCount = 7;

/// @brief One analysis band's frequency span in Hz.
struct BandEdge {
  float low_hz = 0.0f;
  float high_hz = 0.0f;
};

/// @brief The 7 analysis bands, in ascending frequency order.
/// @details Boundaries are the ones @ref mastering::assistant::analyze_audio_profile
///          measures its per-band RMS over. The top band runs to Nyquist; it is
///          written as infinity here and clamped against the actual sample rate
///          at the call site.
inline constexpr std::array<BandEdge, kBandCount> kBands = {{
    {20.0f, 60.0f},                                      // sub
    {60.0f, 250.0f},                                     // low
    {250.0f, 500.0f},                                    // lowMid
    {500.0f, 2000.0f},                                   // mid
    {2000.0f, 6000.0f},                                  // highMid
    {6000.0f, 12000.0f},                                 // high
    {12000.0f, std::numeric_limits<float>::infinity()},  // air
}};

/// @brief camelCase band identifiers used in JSON output and explanations.
inline constexpr std::array<const char*, kBandCount> kBandNames = {
    "sub", "low", "lowMid", "mid", "highMid", "high", "air"};

/// @brief Coarse source taxonomy the rule-based classifier resolves to.
/// @details Deliberately coarse. The classifier is a single-layer decision
///          table over measured features (spectral centroid, rolloff, flatness,
///          onset density, sustain ratio, voicing); it carries no trained model
///          and no statistical mapping layer, and refuses to guess rather than
///          returning a low-confidence label.
enum class SourceClass {
  Unknown = 0,
  Kick,
  Snare,
  HiHat,
  Tom,
  Cymbal,
  Bass,
  Guitar,
  Keys,
  Strings,
  Lead,
  Vocal,
  Backing,
  Percussion,
  Fx,
};

/// @brief Number of entries in @ref SourceClass.
inline constexpr int kSourceClassCount = 15;

/// @brief camelCase identifier for a source class.
/// @return A static string; never null.
const char* source_class_to_string(SourceClass source) noexcept;

/// @brief Resolves a camelCase identifier back to a @ref SourceClass.
/// @return The matching class, or @ref SourceClass::Unknown for an unknown name.
SourceClass source_class_from_string(const std::string& name) noexcept;

/// @brief Every @ref SourceClass identifier, in enum order.
std::vector<std::string> source_class_names();

/// @brief One track handed to the assistant.
/// @details Planar, matching the `input_left` / `input_right` convention the
///          mixing C ABI already uses for multi-track input. A mono track
///          leaves @ref right null.
struct TrackInput {
  /// @brief Strip id the suggestion is written against. Must be unique.
  std::string id;
  /// @brief Optional human-facing track name.
  /// @details Used only as a *hint* that adjusts classifier confidence; it can
  ///          never select a class on its own.
  std::string name;
  const float* left = nullptr;
  const float* right = nullptr;
  /// @brief Frames per channel (not samples across channels).
  std::size_t frame_count = 0;
  int sample_rate = 0;
};

/// @brief Per-band energy over time for one track.
/// @details This is the cache the cross-track phase reads. It exists so the
///          STFT is computed once per track rather than once per track *pair*.
///
///          Layout is `[band][frame]`, i.e. `energy[band * n_frames + frame]`,
///          which matches @ref Spectrogram's band-major convention. Values are
///          **linear power** (sum of `|X|^2` over the band's bins), not dB and
///          not amplitude.
///
///          Frame `f` covers the analysis window centred at
///          `frames_to_time(f, sample_rate, hop_length)` seconds. Use that
///          helper rather than `f * hop / sr`: STFT analysis is centred, so the
///          naive form is off by half a window. Every track in one call shares
///          @ref n_fft and @ref hop_length, so frame indices are directly
///          comparable across tracks.
struct BandEnergyEnvelope {
  int n_frames = 0;
  int n_fft = 0;
  int hop_length = 0;
  int sample_rate = 0;
  /// @brief `[band * n_frames + frame]`, linear power.
  std::vector<float> energy;

  /// @brief Band-major accessor. Out-of-range frames read as silence.
  /// @details Tracks in one mix are not all the same length. Rather than
  ///          truncating every track to the shortest (which would delete a part
  ///          that only enters in the last chorus), each track keeps its real
  ///          frame count and reads as silent past its end.
  float at(int band, int frame) const noexcept {
    if (band < 0 || band >= kBandCount || frame < 0 || frame >= n_frames) return 0.0f;
    return energy[static_cast<std::size_t>(band) * static_cast<std::size_t>(n_frames) +
                  static_cast<std::size_t>(frame)];
  }
};

/// @brief Everything the assistant measures about a single track.
struct TrackProfile {
  /// @brief Mirrors @ref TrackInput::id.
  std::string strip_id;
  /// @brief Mirrors @ref TrackInput::name.
  std::string name;
  /// @brief Loudness / spectral / dynamics measured by the mastering profiler.
  /// @details Composed rather than inherited: the mixing assistant adds fields
  ///          but must not change the meaning of any mastering field.
  mastering::assistant::AudioProfile base{};
  SourceClass source = SourceClass::Unknown;
  /// @brief Classifier confidence in `[0, 1]`.
  float source_confidence = 0.0f;
  /// @brief Share of total energy in each band, summing to 1 for a non-silent
  ///        track and to 0 for a silent one.
  std::array<float, kBandCount> band_occupancy{};
  /// @brief Per-band energy over time; the cross-track phase reads only this.
  BandEnergyEnvelope bands;
  /// @brief 1 for mono, 2 for stereo.
  int channel_count = 1;
  float duration_sec = 0.0f;
  /// @brief False when the track is silent, too short to measure, or has no
  ///        meaningful spectral content.
  /// @details An unusable track gets no suggestions at all — not a suggestion
  ///          of zero. A zero-valued delta would read downstream as "deliberately
  ///          decided to be zero".
  bool usable = false;
  /// @brief Why the track was excluded; empty when @ref usable is true.
  std::string exclusion_reason;
};

/// @brief Tunables for @ref analyze_track_profiles.
struct TrackProfileConfig {
  /// @brief Shared across every track in one call. Mismatched geometry would
  ///        make frame indices incomparable in the cross-track phase.
  int n_fft = 2048;
  int hop_length = 512;
  /// @brief Tracks shorter than this cannot produce a meaningful gated
  ///        integrated loudness, so they are marked unusable.
  float min_duration_sec = 0.4f;
};

/// @note No raw spectrogram is retained. Three minutes at 48 kHz with a 512 hop
///       is roughly 70 MB of magnitude per track, so twenty tracks would be
///       1.4 GB; the folded @ref BandEnergyEnvelope is roughly 0.5 MB for the
///       same material. The one consumer that needs full-band detail is the
///       time-alignment analysis, and it reads the caller's own @ref TrackInput
///       buffers in the time domain rather than a frequency-domain copy.

/// @brief Profiles every track with one shared STFT geometry.
/// @details Degenerate input never throws: a null buffer, a zero frame count or
///          a non-positive sample rate yields a default-constructed profile with
///          @ref TrackProfile::usable false, so callers need no error handling.
/// @param tracks Tracks to profile.
/// @param config Shared analysis geometry.
/// @return One profile per input track, in input order.
std::vector<TrackProfile> analyze_track_profiles(const std::vector<TrackInput>& tracks,
                                                 const TrackProfileConfig& config = {});

/// @brief Profiles a single track. Prefer the batch entry point.
TrackProfile analyze_track_profile(const TrackInput& track, const TrackProfileConfig& config = {});

}  // namespace sonare::mixing::assistant
