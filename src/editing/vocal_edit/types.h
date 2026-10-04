#pragma once

/// @file types.h
/// @brief Value types shared by the vocal edit state, planner and renderer.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "core/audio.h"
#include "util/exception.h"

namespace sonare::editing::vocal_edit {

using VocalNoteId = uint32_t;
using VocalRevision = uint64_t;
using VocalDraftId = uint64_t;
using VocalGeneration = uint64_t;
using VocalSessionEpoch = uint64_t;

inline constexpr VocalNoteId kInvalidVocalNoteId = 0;
inline constexpr VocalRevision kInitialVocalRevision = 0;

using Sha256Digest = std::array<uint8_t, 32>;
using VocalSessionId = std::array<uint8_t, 16>;

enum class PitchTargetMode : uint32_t {
  kNone = 0,
  kCenter = 1,
  kCurve = 2,
};

enum class FormantMode : uint32_t {
  kPreserve = 0,
  kShift = 1,
};

enum class TransitionCurve : uint32_t {
  kSmoothstep = 1,
};

enum class MergeMode : uint32_t {
  kPreserveEdits = 0,
  kResetEdits = 1,
};

enum class RenderProfile : uint32_t {
  kVocalPsolaV1 = 1,
};

/// The domain-level reason set is intentionally smaller than the native
/// exception categories. Facades translate these values without having to
/// maintain a second set of feature-specific ordinals.
enum class VocalReason : uint32_t {
  kNone = 0,
  kInvalidInput = 1,
  kRevisionConflict = 2,
  kSourceMismatch = 3,
  kUnsupported = 4,
  kCancelled = 5,
  kCounterExhausted = 6,
  kInvalidState = 7,
};

/// @brief A half-open sample interval in the immutable source timeline.
struct SampleRange {
  int64_t start = 0;
  int64_t end = 0;

  int64_t length() const noexcept;
  bool empty() const noexcept { return start >= end; }
};

struct AnalysisGrid {
  double frame_origin_sample = 0.0;
  double samples_per_frame = 512.0;
  uint32_t frame_length_samples = 2048;
};

/// Normalized analysis settings stored with the F0 grid and its digest.
struct VocalAnalysisSettings {
  double fmin_hz = 65.0;
  double fmax_hz = 2093.0;
  double yin_threshold = 0.1;
  double voiced_threshold = 0.5;
  bool centered = true;
  double segmentation_threshold_cents = 50.0;
  double minimum_note_ms = 30.0;
  double reference_hz = 440.0;
};

struct SourceDescriptor {
  uint32_t sample_rate = 0;
  int64_t sample_count = 0;
  Sha256Digest digest{};
};

struct VocalPitchPoint {
  double source_sample = 0.0;
  double target_midi = 0.0;
};

struct VocalPitchTarget {
  PitchTargetMode mode = PitchTargetMode::kNone;
  double center_midi = 0.0;
  std::vector<VocalPitchPoint> points;
};

struct VocalPitchEdit {
  VocalPitchTarget target{};
  double amount = 0.0;
  double speed_ms = 0.0;
  double max_correction_semitones = 12.0;
  double transpose_semitones = 0.0;
  double drift_scale = 1.0;
  double vibrato_scale = 1.0;
};

struct VocalFormantEdit {
  FormantMode mode = FormantMode::kPreserve;
  double shift_semitones = 0.0;
};

struct VocalNoteEdit {
  VocalPitchEdit pitch{};
  int64_t destination_start_sample = 0;
  int64_t destination_length_samples = 0;
  double gain_db = 0.0;
  bool muted = false;
  std::vector<float> amplitude_envelope;
  VocalFormantEdit formant{};

  static VocalNoteEdit identity_for(SampleRange source_range);
  bool is_identity(SampleRange source_range) const noexcept;
};

struct VocalNote {
  VocalNoteId id = kInvalidVocalNoteId;
  SampleRange source_range{};
  uint32_t analysis_frame_start = 0;
  uint32_t analysis_frame_end = 0;
  bool has_pitch = false;
  double centre_midi = 0.0;
  double median_hz = 0.0;
  double f0_stability = 0.0;
  VocalNoteEdit edit{};
};

struct PitchTransition {
  VocalNoteId left_note_id = kInvalidVocalNoteId;
  VocalNoteId right_note_id = kInvalidVocalNoteId;
  int64_t left_window_samples = 0;
  int64_t right_window_samples = 0;
  double strength = 0.0;
  TransitionCurve curve = TransitionCurve::kSmoothstep;
};

struct VocalStateToken {
  VocalSessionEpoch epoch = 0;
  VocalRevision committed_revision = 0;
  VocalDraftId draft_id = 0;
  VocalGeneration draft_generation = 0;
};

struct RenderSettings {
  RenderProfile profile = RenderProfile::kVocalPsolaV1;
  uint32_t algorithm_version = 1;
  double edge_fade_ms = 5.0;
  double vibrato_cutoff_hz = 3.0;
};

struct VocalSessionLimits {
  uint64_t max_history_bytes = 64u * 1024u * 1024u;
  uint64_t max_cache_bytes = 128u * 1024u * 1024u;
  uint32_t max_undo_depth = 128;
  uint32_t max_render_jobs = 4;
};

struct VocalAnalysisData {
  AnalysisGrid grid{};
  VocalAnalysisSettings settings{};
  std::vector<float> f0_hz;
  std::vector<float> amplitude;
  std::vector<uint8_t> voiced;
  std::string algorithm_id;
  uint32_t algorithm_version = 0;
  Sha256Digest digest{};
};

struct VocalEditState {
  std::vector<VocalNote> notes;
  std::vector<PitchTransition> transitions;
};

struct SetNoteEditOp {
  VocalNoteId note_id = kInvalidVocalNoteId;
  VocalNoteEdit edit{};
};

struct SetNoteSourceSpanOp {
  VocalNoteId note_id = kInvalidVocalNoteId;
  SampleRange source_range{};
  int64_t destination_start_sample = 0;
  int64_t destination_length_samples = 0;
};

struct SplitNoteOp {
  VocalNoteId note_id = kInvalidVocalNoteId;
  int64_t source_sample = 0;
};

struct MergeNotesOp {
  std::vector<VocalNoteId> note_ids;
  MergeMode mode = MergeMode::kPreserveEdits;
};

struct SetTransitionOp {
  PitchTransition transition{};
};

struct RemoveTransitionOp {
  VocalNoteId left_note_id = kInvalidVocalNoteId;
  VocalNoteId right_note_id = kInvalidVocalNoteId;
};

struct ResetNotesOp {
  std::vector<VocalNoteId> note_ids;
};

using Operation = std::variant<SetNoteEditOp, SetNoteSourceSpanOp, SplitNoteOp, MergeNotesOp,
                               SetTransitionOp, RemoveTransitionOp, ResetNotesOp>;

struct IdChange {
  uint32_t operation_index = 0;
  VocalNoteId retired_id = kInvalidVocalNoteId;
  std::vector<VocalNoteId> new_ids;
};

struct DraftApplyResult {
  VocalStateToken token{};
  std::vector<SampleRange> dirty_ranges;
  std::vector<IdChange> id_changes;
};

struct StateChangeResult {
  VocalStateToken token{};
  std::vector<SampleRange> dirty_ranges;
};

struct VocalCapabilities {
  uint32_t api_version = 1;
  RenderProfile profile = RenderProfile::kVocalPsolaV1;
  bool monophonic_only = true;
  bool analysis_cancellable = false;
  double min_formant_shift_semitones = -10.3;
  double max_formant_shift_semitones = 8.7;
};

/// @brief Exception carrying a stable reason and machine-readable context.
class VocalEditException : public SonareException {
 public:
  VocalEditException(VocalReason reason, std::string message, std::string field = {},
                     std::string expected = {}, std::string actual = {});

  VocalReason reason() const noexcept { return reason_; }
  const std::string& field() const noexcept { return field_; }
  const std::string& expected() const noexcept { return expected_; }
  const std::string& actual() const noexcept { return actual_; }

 private:
  VocalReason reason_;
  std::string field_;
  std::string expected_;
  std::string actual_;
};

/// Shared final-candidate validator used by the session and state codec.
void validate_vocal_edit_state(const VocalEditState& state, const VocalAnalysisData& analysis,
                               int64_t source_length_samples, int64_t output_length_samples,
                               const RenderSettings& render_settings = {});

}  // namespace sonare::editing::vocal_edit
