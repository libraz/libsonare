#pragma once

/// @file external_stems.h
/// @brief Control-thread import facade for externally separated PCM stems.
///
/// This facade deliberately accepts decoded PCM only. It does not load models,
/// resample, estimate latency, or alter phase alignment. On success the input
/// samples are copied into the ordinary @ref AudioContentStore and represented
/// by regular audio tracks and clips.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"

namespace sonare::arrangement {

/// Channel layouts accepted by the initial external-stem import contract.
enum class ExternalSeparatedStemLayout : uint32_t {
  kMono = 1,
  kStereo = 2,
};

/// One decoded output of a host-owned external separator.
struct ExternalSeparatedStem {
  /// Non-empty, valid UTF-8 and unique within one import set.
  std::string name;
  /// Optional host-defined semantic label (for example "vocals" or "drums").
  /// It is stored as source metadata but has no DSP effect.
  std::optional<std::string> role;
  ExternalSeparatedStemLayout layout = ExternalSeparatedStemLayout::kStereo;
  /// Deinterleaved PCM. The outer size must match @ref layout and every plane
  /// must have the same non-zero frame count.
  std::vector<std::vector<float>> planar_samples;
  /// Absolute project-rate frame at which the stem begins.
  int64_t start_frame = 0;
};

/// Input supplied by a host after running its own separation model.
struct ExternalSeparatedStemSet {
  int sample_rate = 0;
  std::vector<ExternalSeparatedStem> stems;
};

enum class ExternalSeparatedStemImportError : uint32_t {
  kOk = 0,
  kInvalidArgument,
  kSampleRateMismatch,
  kFramePositionNotRepresentable,
  kProjectMutationFailed,
};

/// Result of an all-or-nothing external-stem import.
struct ExternalSeparatedStemImportResult {
  ExternalSeparatedStemImportError error = ExternalSeparatedStemImportError::kOk;
  std::vector<TrackId> track_ids;
  std::vector<ClipId> clip_ids;

  bool ok() const noexcept { return error == ExternalSeparatedStemImportError::kOk; }
};

/// Imports externally separated PCM as one ordinary audio track and clip per
/// stem. This is a CONTROL-THREAD-ONLY structural operation. The function
/// validates the entire set before modifying anything, copies input PCM on
/// success, and performs no resampling, phase adjustment, or gain compensation.
///
/// The caller owns @p project and @p audio. They are unchanged on every error.
ExternalSeparatedStemImportResult import_external_separated_stems(
    Project* project, AudioContentStore* audio, const ExternalSeparatedStemSet& input);

}  // namespace sonare::arrangement
