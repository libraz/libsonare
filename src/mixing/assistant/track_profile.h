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
///
///          Every entry here is producible. Most come from the decision table;
///          Keys, Strings, Backing and Fx have no row it could separate them
///          with and are supplied by the track name instead. A class nothing
///          can produce would tell a host about a label it will never see, so
///          the classifier holds that as a compile-time invariant.
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
  /// @details For a class the classifier's decision table can measure, this is
  ///          only a *hint* that adjusts confidence; it cannot select one of
  ///          those on its own. For the four classes the table has no rule for
  ///          — @ref SourceClass::Keys, @ref SourceClass::Strings, @ref
  ///          SourceClass::Backing and @ref SourceClass::Fx — it is the only
  ///          thing that can supply the class at all.
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

/// @brief Time-averaged linear power spectrum for one track.
/// @details Kept because the analysis bands are too coarse to answer a question
///          asked at one frequency. Every high-pass corner a source class uses
///          falls *inside* the sub or low band, so band occupancy cannot say how
///          much of a track sits below one; this can.
///
///          **Not a second band grid and not a second envelope over time.** The
///          standing constraint that there is one band grid — so that a masking
///          figure and a spectral figure stay comparable — is untouched: this
///          carries no bands and no time axis, and nothing in the masking or
///          dominance path reads it. Those stay on @ref BandEnergyEnvelope, and
///          could not use this even if they wanted to: a figure averaged over
///          the whole track cannot tell a genuine collision apart from two parts
///          that share a band but never sound at the same moment.
///
///          Values are **linear power** (the mean of `|X|^2` over frames), not
///          dB and not amplitude. Bin `k` is centred at
///          `k * sample_rate / n_fft`, and the bin's span is taken as half a bin
///          either side of that centre.
struct MeanPowerSpectrum {
  /// @brief `n_fft / 2 + 1` for a measured track; zero when nothing was measured.
  int n_bins = 0;
  int n_fft = 0;
  int sample_rate = 0;
  /// @brief One entry per bin, linear power averaged over frames.
  std::vector<float> power;

  /// @brief Share of the track's total energy below @p frequency_hz, in `[0, 1]`.
  /// @details A bin the frequency falls inside contributes the fraction of
  ///          itself that lies below it, so the answer moves smoothly with the
  ///          frequency rather than in bin-sized steps. Bins are around 23 Hz
  ///          wide at the default geometry, which is coarse next to the corners
  ///          this is asked about.
  /// @param frequency_hz Frequency to measure below. Above Nyquist the whole
  ///        spectrum is below it, so the answer is 1 for a track with energy.
  /// @return 0 when there is no spectrum, no energy, or @p frequency_hz is not a
  ///         positive finite number. Zero is also the honest answer for a track
  ///         with nothing down there, and both readings mean "do not filter".
  float energy_share_below(float frequency_hz) const noexcept;
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
  /// @brief Time-averaged spectrum, for the questions a band is too wide to answer.
  MeanPowerSpectrum spectrum;
  /// @brief 1 for mono, 2 for stereo.
  int channel_count = 1;
  float duration_sec = 0.0f;
  /// @brief False when the track is silent, too short to measure, carries a
  ///        non-finite sample, or has no meaningful spectral content.
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

/// @note No raw spectrogram is retained — nothing keeps both full bin resolution
///       and the time axis. Three minutes at 48 kHz with a 512 hop is roughly
///       70 MB of magnitude per track, so twenty tracks would be 1.4 GB. What is
///       kept is two folds of it, each cheap because each gives up one axis: the
///       @ref BandEnergyEnvelope keeps time at 7 bands, roughly 0.5 MB for the
///       same material, and the @ref MeanPowerSpectrum keeps every bin with no
///       time at all, roughly 4 KB whatever the track's length. The one consumer
///       that needs full-band detail over time is the time-alignment analysis,
///       and it reads the caller's own @ref TrackInput buffers in the time domain
///       rather than a frequency-domain copy.

/// @brief Profiles every track with one shared STFT geometry.
/// @details Degenerate input never throws: a null buffer, a zero frame count, a
///          non-positive sample rate or a non-finite sample yields a
///          default-constructed profile with @ref TrackProfile::usable false and
///          an @ref TrackProfile::exclusion_reason naming which, so callers need
///          no error handling. A NaN is reported as its own reason rather than
///          being allowed to reach the loudness measurement, where it would come
///          back as "track is silent" — a statement about the material instead
///          of about the buffer.
/// @details A returned profile is complete, @ref TrackProfile::source and
///          @ref TrackProfile::source_confidence included. Classification runs
///          here rather than as a step the caller has to know to take: every
///          decision stage skips a track it reads as
///          @ref SourceClass::Unknown, so an unclassified profile does not
///          fail — it quietly loses most of the suggestion.
/// @param tracks Tracks to profile.
/// @param config Shared analysis geometry.
/// @return One profile per input track, in input order.
std::vector<TrackProfile> analyze_track_profiles(const std::vector<TrackInput>& tracks,
                                                 const TrackProfileConfig& config = {});

/// @brief Profiles a single track, classification included. Prefer the batch
///        entry point.
TrackProfile analyze_track_profile(const TrackInput& track, const TrackProfileConfig& config = {});

}  // namespace sonare::mixing::assistant
