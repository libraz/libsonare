// SONARE_WASM_EXCEPTION_UNWIND: release edit vectors when validation throws.
#include "editing/vocal_edit/types.h"

#include <cmath>
#include <limits>

namespace sonare::editing::vocal_edit {
namespace {

ErrorCode error_code_for(VocalReason reason) {
  switch (reason) {
    case VocalReason::kCancelled:
      return ErrorCode::Cancelled;
    case VocalReason::kRevisionConflict:
    case VocalReason::kSourceMismatch:
    case VocalReason::kCounterExhausted:
    case VocalReason::kInvalidState:
      return ErrorCode::InvalidState;
    case VocalReason::kUnsupported:
      return ErrorCode::NotImplemented;
    case VocalReason::kNone:
    case VocalReason::kInvalidInput:
      return ErrorCode::InvalidParameter;
  }
  return ErrorCode::InvalidParameter;
}

}  // namespace

VocalNoteEdit VocalNoteEdit::identity_for(const SampleRange source_range) {
  if (source_range.start < 0 || source_range.end <= source_range.start) {
    throw VocalEditException(VocalReason::kInvalidInput,
                             "identity edit requires a positive non-negative source range",
                             "source_range");
  }
  VocalNoteEdit edit;
  edit.destination_start_sample = source_range.start;
  edit.destination_length_samples = source_range.length();
  if (edit.destination_length_samples < 0) {
    throw VocalEditException(VocalReason::kInvalidInput, "source range length is not representable",
                             "source_range");
  }
  return edit;
}

bool VocalNoteEdit::is_identity(const SampleRange source_range) const noexcept {
  return pitch.target.mode == PitchTargetMode::kNone && pitch.amount == 0.0 &&
         pitch.speed_ms == 0.0 && pitch.max_correction_semitones == 12.0 &&
         pitch.transpose_semitones == 0.0 && pitch.drift_scale == 1.0 &&
         pitch.vibrato_scale == 1.0 && destination_start_sample == source_range.start &&
         destination_length_samples == source_range.length() && gain_db == 0.0 && !muted &&
         amplitude_envelope.empty() && formant.mode == FormantMode::kPreserve &&
         formant.shift_semitones == 0.0;
}

VocalEditException::VocalEditException(VocalReason reason, std::string message, std::string field,
                                       std::string expected, std::string actual)
    : SonareException(error_code_for(reason), std::move(message)),
      reason_(reason),
      field_(std::move(field)),
      expected_(std::move(expected)),
      actual_(std::move(actual)) {}

}  // namespace sonare::editing::vocal_edit
