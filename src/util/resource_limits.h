#pragma once

/// @file resource_limits.h
/// @brief Shared limits for caller-controlled offline resource expansion.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "util/numeric_validation.h"

namespace sonare::resource {

/// Maximum number of float samples accepted by one offline audio operation.
inline constexpr std::size_t kMaxOfflineAudioSamples = 500'000'000;
/// Peak memory budget for an engine-owned offline bounce/freeze result. Bounce
/// paths can hold planar, interleaved, and binding-owned copies concurrently;
/// the helper below accounts for that multiplier before any allocation.
inline constexpr std::size_t kMaxEngineOfflinePeakBytes = 1024u * 1024u * 1024u;
/// Upper bound for caller-controlled iterative phase reconstruction.
inline constexpr int kMaxGriffinLimIterations = 256;

/// Maximum MIDI expansion performed by one project export operation.
inline constexpr std::size_t kMaxProjectMidiExportEvents = 1'000'000;
inline constexpr std::size_t kMaxMidiExportLoopIterations = 1'000'000;

/// Meter-spectrum allocations include result arrays, FFT work/config storage,
/// a windowed frame, complex bins, and a double-precision power accumulator.
struct SpectrumResourceLimits {
  std::size_t max_fft_size;
  std::size_t max_bins;
  std::size_t max_peak_bytes;
};

inline constexpr SpectrumResourceLimits kDefaultSpectrumResourceLimits{
    4u * 1024u * 1024u,       // 4M-point FFT; well above normal metering use
    2u * 1024u * 1024u + 1u,  // real-FFT bins for the limit above
    256u * 1024u * 1024u,     // conservative live working-set ceiling
};

struct AcousticBandResourceLimits {
  std::size_t max_octave_bands;
  std::size_t max_third_octave_bands;
};

inline constexpr AcousticBandResourceLimits kDefaultAcousticBandResourceLimits{
    64u,   // physically useful octave bands are far fewer at supported rates
    128u,  // likewise for third-octave analysis
};

struct ProjectImportResourceLimits {
  std::size_t max_json_bytes;
  std::size_t max_json_nodes;
  std::size_t max_entities;
  std::size_t max_string_bytes;
  std::size_t max_decoded_payload_bytes;
};

inline constexpr ProjectImportResourceLimits kDefaultProjectImportResourceLimits{
    64u * 1024u * 1024u,  // encoded document
    2'000'000u,           // objects, arrays, and scalar values
    1'000'000u,           // cumulative array elements decoded as entities
    32u * 1024u * 1024u,  // cumulative decoded JSON keys and string values
    32u * 1024u * 1024u,  // cumulative sidecar and SysEx payload bytes
};

struct MidiImportResourceLimits {
  std::size_t max_file_bytes;
  std::size_t max_tracks;
  std::size_t max_events;
  std::size_t max_metadata_bytes;
  std::size_t max_sysex_bytes;
};

inline constexpr MidiImportResourceLimits kDefaultMidiImportResourceLimits{
    64u * 1024u * 1024u,  // encoded SMF / MIDI Clip File
    4096u,                // data + conductor tracks
    1'000'000u,           // channel, tempo, signature, marker, and SysEx events
    16u * 1024u * 1024u,  // copied names / text / lyrics / cue points
    32u * 1024u * 1024u,  // copied SysEx payload bytes
};

/// Default file-size ceiling used by the general audio-file loaders.
inline constexpr std::size_t kMaxAudioFileBytes = 500u * 1024u * 1024u;

/// SoundFont parsing budgets. The peak estimate counts the caller-owned input
/// bytes plus the decoded float sample pool, which is the dominant expansion.
struct Sf2ResourceLimits {
  std::size_t max_file_bytes;
  std::size_t max_sample_points;
  std::size_t max_peak_bytes;
  std::size_t max_table_records;
};

inline constexpr Sf2ResourceLimits kDefaultSf2ResourceLimits{
    256u * 1024u * 1024u,  // input SF2 bytes
    64u * 1024u * 1024u,   // decoded sample points (256 MiB as float)
    512u * 1024u * 1024u,  // input + decoded float sample pool
    65'536u,               // one pdta table (indices are uint16_t)
};

struct EngineOfflineLimits {
  std::size_t max_total_samples;
  std::size_t max_peak_bytes;
};

inline constexpr EngineOfflineLimits kDefaultEngineOfflineLimits{kMaxOfflineAudioSamples,
                                                                 kMaxEngineOfflinePeakBytes};

inline bool bounded_accumulate(std::size_t additional, std::size_t limit,
                               std::size_t* total) noexcept {
  if (total == nullptr) return false;
  std::size_t next = 0;
  if (!numeric::checked_add(*total, additional, &next) || next > limit) return false;
  *total = next;
  return true;
}

inline bool spectrum_shape_fits(
    int n_fft, const SpectrumResourceLimits& limits = kDefaultSpectrumResourceLimits) noexcept {
  if (n_fft <= 0) return false;
  const std::size_t fft_size = static_cast<std::size_t>(n_fft);
  const std::size_t bins = fft_size / 2u + 1u;
  if (fft_size > limits.max_fft_size || bins > limits.max_bins) return false;

  std::size_t peak_bytes = 0;
  const auto add_array = [&](std::size_t count, std::size_t element_bytes,
                             std::size_t copies) noexcept {
    std::size_t bytes = 0;
    if (!numeric::checked_size_product(count, element_bytes, limits.max_peak_bytes, &bytes) ||
        !numeric::checked_size_product(bytes, copies, limits.max_peak_bytes, &bytes)) {
      return false;
    }
    return bounded_accumulate(bytes, limits.max_peak_bytes, &peak_bytes);
  };
  // frame + cached window; three conservative KissFFT complex work/config
  // buffers; result frequency/magnitude/power/db; output bins; power accumulator.
  return add_array(fft_size, sizeof(float), 2u) && add_array(fft_size, sizeof(float) * 2u, 3u) &&
         add_array(bins, sizeof(float), 4u) && add_array(bins, sizeof(float) * 2u, 1u) &&
         add_array(bins, sizeof(double), 1u);
}

inline bool acoustic_band_counts_fit(
    int octave_bands, int third_octave_bands,
    const AcousticBandResourceLimits& limits = kDefaultAcousticBandResourceLimits) noexcept {
  return octave_bands >= 0 && third_octave_bands >= 0 &&
         static_cast<std::size_t>(octave_bands) <= limits.max_octave_bands &&
         static_cast<std::size_t>(third_octave_bands) <= limits.max_third_octave_bands;
}

inline bool project_document_fits(
    std::size_t json_bytes, std::size_t nodes, std::size_t entities, std::size_t string_bytes,
    std::size_t decoded_payload_bytes,
    const ProjectImportResourceLimits& limits = kDefaultProjectImportResourceLimits) noexcept {
  return json_bytes <= limits.max_json_bytes && nodes <= limits.max_json_nodes &&
         entities <= limits.max_entities && string_bytes <= limits.max_string_bytes &&
         decoded_payload_bytes <= limits.max_decoded_payload_bytes;
}

inline bool midi_import_shape_fits(
    std::size_t file_bytes, std::size_t tracks, std::size_t events, std::size_t metadata_bytes,
    std::size_t sysex_bytes,
    const MidiImportResourceLimits& limits = kDefaultMidiImportResourceLimits) noexcept {
  return file_bytes <= limits.max_file_bytes && tracks <= limits.max_tracks &&
         events <= limits.max_events && metadata_bytes <= limits.max_metadata_bytes &&
         sysex_bytes <= limits.max_sysex_bytes;
}

/// Validates an engine-owned offline result before narrowing the signed frame
/// count to size_t or multiplying it by channels/copy count. `live_float_copies`
/// is the maximum number of full-size float buffers simultaneously retained by
/// the path (e.g. planar + interleaved + binding result for bounce).
inline bool engine_offline_shape_fits(
    int64_t frames, int channels, std::size_t live_float_copies,
    const EngineOfflineLimits& limits = kDefaultEngineOfflineLimits) noexcept {
  if (frames <= 0 || channels <= 0 || live_float_copies == 0) return false;
  const auto unsigned_frames = static_cast<uint64_t>(frames);
  if (unsigned_frames > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  std::size_t total_samples = 0;
  if (!numeric::checked_size_product(static_cast<std::size_t>(unsigned_frames),
                                     static_cast<std::size_t>(channels), limits.max_total_samples,
                                     &total_samples)) {
    return false;
  }
  std::size_t one_copy_bytes = 0;
  if (!numeric::checked_size_product(total_samples, sizeof(float), limits.max_peak_bytes,
                                     &one_copy_bytes)) {
    return false;
  }
  std::size_t peak_bytes = 0;
  return numeric::checked_size_product(one_copy_bytes, live_float_copies, limits.max_peak_bytes,
                                       &peak_bytes);
}

/// Bounce adds resampler double buffers when rates differ, so validate both the
/// source allocation and the rounded output size using a conservative
/// float-equivalent copy multiplier. This mirrors core/resample.cpp's rounded
/// output-length contract while rejecting before its floating-to-size_t cast.
inline bool engine_bounce_shape_fits(
    int64_t frames, int channels, int source_sample_rate, int target_sample_rate,
    const EngineOfflineLimits& limits = kDefaultEngineOfflineLimits) noexcept {
  if (source_sample_rate <= 0 || target_sample_rate <= 0) return false;
  const bool resampling = source_sample_rate != target_sample_rate;
  const std::size_t live_float_copies = resampling ? 7u : 3u;
  if (!engine_offline_shape_fits(frames, channels, live_float_copies, limits)) return false;

  const long double projected =
      std::round(static_cast<long double>(frames) * static_cast<long double>(target_sample_rate) /
                 static_cast<long double>(source_sample_rate));
  if (!std::isfinite(projected) || projected <= 0.0L ||
      projected > static_cast<long double>(limits.max_total_samples)) {
    return false;
  }
  return engine_offline_shape_fits(static_cast<int64_t>(projected), channels, live_float_copies,
                                   limits);
}

inline bool sf2_file_fits(std::size_t file_bytes,
                          const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  return file_bytes <= limits.max_file_bytes;
}

/// Checks the dominant SF2 expansion without allocating. This helper is also
/// used for exact boundary tests with small custom budgets.
inline bool sf2_sample_expansion_fits(
    std::size_t file_bytes, std::size_t sample_points,
    const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  if (!sf2_file_fits(file_bytes, limits) || sample_points > limits.max_sample_points) {
    return false;
  }
  std::size_t decoded_bytes = 0;
  if (!numeric::checked_size_product(sample_points, sizeof(float), limits.max_peak_bytes,
                                     &decoded_bytes)) {
    return false;
  }
  std::size_t peak_bytes = 0;
  return numeric::checked_add(file_bytes, decoded_bytes, &peak_bytes) &&
         peak_bytes <= limits.max_peak_bytes;
}

inline bool sf2_table_records_fit(
    std::size_t current_records, std::size_t additional_records,
    const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  std::size_t total = 0;
  return numeric::checked_add(current_records, additional_records, &total) &&
         total <= limits.max_table_records;
}

}  // namespace sonare::resource
