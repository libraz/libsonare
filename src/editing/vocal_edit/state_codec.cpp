// SONARE_WASM_EXCEPTION_UNWIND: release decoded state vectors when strict decoding throws.
#include "editing/vocal_edit/state_codec.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "editing/vocal_edit/analysis.h"
#include "util/exception.h"

namespace sonare::editing::vocal_edit {
namespace {

// These limits are deliberately finite and are applied before reserve() or a
// vector resize. They keep a corrupt file from turning a count field into an
// allocation request while leaving room for long vocal sessions.
constexpr uint64_t kMaxEncodedBytes = 256u * 1024u * 1024u;
constexpr uint64_t kMaxAlgorithmBytes = 1024u * 1024u;
constexpr uint64_t kMaxFrames = 4u * 1024u * 1024u;
constexpr uint64_t kMaxNotes = 1u * 1024u * 1024u;
constexpr uint64_t kMaxPitchPoints = 4u * 1024u * 1024u;
constexpr uint64_t kMaxEnvelopeSamples = 4u * 1024u * 1024u;
constexpr uint64_t kMaxTransitions = 1u * 1024u * 1024u;

constexpr std::string_view kSupportedAlgorithm = "libsonare.pyin";
constexpr std::string_view kHostAlgorithm = "host";

[[noreturn]] void invalid(const char* field, const char* message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

bool checked_add(size_t left, size_t right, size_t* result) {
  if (right > std::numeric_limits<size_t>::max() - left) return false;
  *result = left + right;
  return true;
}

bool valid_utf8(const std::string_view value) noexcept {
  size_t index = 0;
  while (index < value.size()) {
    const uint8_t lead = static_cast<uint8_t>(value[index]);
    if (lead <= 0x7f) {
      ++index;
      continue;
    }
    size_t width = 0;
    uint8_t second_min = 0x80;
    uint8_t second_max = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
      width = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      width = 3;
      if (lead == 0xe0) second_min = 0xa0;
      if (lead == 0xed) second_max = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      width = 4;
      if (lead == 0xf0) second_min = 0x90;
      if (lead == 0xf4) second_max = 0x8f;
    } else {
      return false;
    }
    if (index + width > value.size()) return false;
    const uint8_t second = static_cast<uint8_t>(value[index + 1]);
    if (second < second_min || second > second_max) return false;
    for (size_t continuation = 2; continuation < width; ++continuation) {
      const uint8_t byte = static_cast<uint8_t>(value[index + continuation]);
      if (byte < 0x80 || byte > 0xbf) return false;
    }
    index += width;
  }
  return true;
}

bool supported_algorithm(const std::string& algorithm_id) {
  return algorithm_id == kSupportedAlgorithm || algorithm_id == kHostAlgorithm;
}

bool valid_pitch_target_mode(const uint32_t value) {
  return value <= static_cast<uint32_t>(PitchTargetMode::kCurve);
}

bool valid_formant_mode(const uint32_t value) {
  return value <= static_cast<uint32_t>(FormantMode::kShift);
}

bool valid_transition_curve(const uint32_t value) {
  return value == static_cast<uint32_t>(TransitionCurve::kSmoothstep);
}

bool valid_render_profile(const uint32_t value) {
  return value == static_cast<uint32_t>(RenderProfile::kVocalPsolaV1);
}

void require_finite(const double value, const char* field) {
  if (!std::isfinite(value)) invalid(field, "must be finite");
}

void require_finite_nonnegative(const double value, const char* field) {
  if (!std::isfinite(value) || value < 0.0) invalid(field, "must be finite and non-negative");
}

void require_finite_nonnegative(const float value, const char* field) {
  if (!std::isfinite(value) || value < 0.0f) invalid(field, "must be finite and non-negative");
}

void require_source_and_output(const VocalPersistedState& state) {
  if (state.source.sample_rate == 0 ||
      state.source.sample_rate > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    invalid("source.sample_rate", "must be a positive native sample rate");
  }
  if (state.source.sample_count < 0) invalid("source.sample_count", "must be non-negative");
  if (state.output_length_samples < 0) {
    invalid("output_length_samples", "must be non-negative");
  }
  if (state.output_length_samples < state.source.sample_count) {
    invalid("output_length_samples", "must cover the source sample range");
  }
  if (state.next_note_id == 0 ||
      state.next_note_id > static_cast<uint64_t>(std::numeric_limits<VocalNoteId>::max()) + 1u) {
    invalid("next_note_id", "is outside the note ID high-water range");
  }
}

void require_analysis_wire_values(const VocalPersistedState& state) {
  const auto& analysis = state.analysis;
  if (!std::isfinite(analysis.grid.frame_origin_sample)) {
    invalid("analysis.grid.frame_origin_sample", "must be finite");
  }
  if (!std::isfinite(analysis.grid.samples_per_frame) || analysis.grid.samples_per_frame < 1.0) {
    invalid("analysis.grid.samples_per_frame", "must be finite and at least one");
  }
  if (analysis.grid.frame_length_samples == 0) {
    invalid("analysis.grid.frame_length_samples", "must be positive");
  }
  if (analysis.f0_hz.empty() || analysis.f0_hz.size() > kMaxFrames) {
    invalid("analysis.f0_hz", "has an unsupported frame count");
  }
  if (analysis.voiced.size() != analysis.f0_hz.size() ||
      analysis.amplitude.size() != analysis.f0_hz.size()) {
    invalid("analysis", "analysis arrays must have the same non-zero length");
  }
  if (analysis.algorithm_id.empty() || analysis.algorithm_id.size() > kMaxAlgorithmBytes ||
      !valid_utf8(analysis.algorithm_id)) {
    invalid("analysis.algorithm_id", "must be valid UTF-8 within the resource limit");
  }
  if (!supported_algorithm(analysis.algorithm_id)) {
    invalid("analysis.algorithm_id", "algorithm is not supported by this codec");
  }
  if (analysis.algorithm_version != 1) {
    invalid("analysis.algorithm_version", "algorithm version is not supported");
  }
  const auto& settings = analysis.settings;
  require_finite(settings.fmin_hz, "analysis.settings.fmin_hz");
  require_finite(settings.fmax_hz, "analysis.settings.fmax_hz");
  require_finite(settings.yin_threshold, "analysis.settings.yin_threshold");
  require_finite(settings.voiced_threshold, "analysis.settings.voiced_threshold");
  require_finite(settings.segmentation_threshold_cents,
                 "analysis.settings.segmentation_threshold_cents");
  require_finite(settings.minimum_note_ms, "analysis.settings.minimum_note_ms");
  require_finite(settings.reference_hz, "analysis.settings.reference_hz");
  if (settings.fmin_hz <= 0.0 || settings.fmax_hz <= settings.fmin_hz ||
      settings.yin_threshold < 0.0 || settings.yin_threshold > 1.0 ||
      settings.voiced_threshold < 0.0 || settings.voiced_threshold > 1.0 ||
      settings.segmentation_threshold_cents <= 0.0 || settings.minimum_note_ms <= 0.0 ||
      settings.reference_hz <= 0.0) {
    invalid("analysis.settings", "analysis settings are malformed");
  }
  for (size_t index = 0; index < analysis.f0_hz.size(); ++index) {
    if (analysis.voiced[index] > 1) invalid("analysis.voiced", "values must be zero or one");
    const float f0 = analysis.f0_hz[index];
    if (analysis.voiced[index] == 0) {
      if (std::signbit(f0) || f0 != 0.0f) {
        invalid("analysis.f0_hz", "unvoiced F0 must be canonical positive zero");
      }
    } else if (!std::isfinite(f0) || f0 <= 0.0f ||
               static_cast<double>(f0) > static_cast<double>(state.source.sample_rate) * 0.5) {
      invalid("analysis.f0_hz", "voiced F0 must be positive and finite");
    }
    require_finite_nonnegative(analysis.amplitude[index], "analysis.amplitude");
  }
}

void require_analysis_digest(const VocalAnalysisData& analysis) {
  if (digest_analysis(analysis) != analysis.digest) {
    invalid("analysis.digest", "does not match the analysis values");
  }
}

void validate_for_codec(const VocalPersistedState& state) {
  require_source_and_output(state);
  require_analysis_wire_values(state);
  require_analysis_digest(state.analysis);
  if (!valid_render_profile(static_cast<uint32_t>(state.render_settings.profile))) {
    invalid("render_settings.profile", "render profile is not supported");
  }
  if (state.render_settings.algorithm_version != 1) {
    invalid("render_settings.algorithm_version", "render algorithm version is not supported");
  }
  require_finite_nonnegative(state.render_settings.edge_fade_ms, "render_settings.edge_fade_ms");
  require_finite(state.render_settings.vibrato_cutoff_hz, "render_settings.vibrato_cutoff_hz");
  if (state.render_settings.vibrato_cutoff_hz <= 0.0) {
    invalid("render_settings.vibrato_cutoff_hz", "must be positive");
  }
  if (state.edit_state.notes.size() > kMaxNotes) invalid("notes", "count exceeds resource limit");
  if (state.edit_state.transitions.size() > kMaxTransitions) {
    invalid("transitions", "count exceeds resource limit");
  }
  for (const auto& note : state.edit_state.notes) {
    if (note.edit.pitch.target.points.size() > kMaxPitchPoints) {
      invalid("pitch.target.points", "count exceeds resource limit");
    }
    if (note.edit.amplitude_envelope.size() > kMaxEnvelopeSamples) {
      invalid("amplitude_envelope", "count exceeds resource limit");
    }
  }
  // This is the same final validator used when a session is created or a
  // draft is committed. The codec-specific checks above happen first so a
  // malformed wire value cannot be hidden by a permissive state validator.
  validate_vocal_edit_state(state.edit_state, state.analysis, state.source.sample_count,
                            state.output_length_samples, state.render_settings);
}

class Writer {
 public:
  void byte(const uint8_t value) {
    ensure(1);
    bytes_.push_back(value);
  }

  void u32(const uint32_t value) {
    ensure(4);
    bytes_.push_back(static_cast<uint8_t>(value));
    bytes_.push_back(static_cast<uint8_t>(value >> 8));
    bytes_.push_back(static_cast<uint8_t>(value >> 16));
    bytes_.push_back(static_cast<uint8_t>(value >> 24));
  }

  void u64(const uint64_t value) {
    ensure(8);
    for (uint32_t shift = 0; shift < 64; shift += 8) {
      bytes_.push_back(static_cast<uint8_t>(value >> shift));
    }
  }

  void i64(const int64_t value) { u64(static_cast<uint64_t>(value)); }

  void f32(const float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be IEEE-754 binary32");
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }

  void f64(const double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be IEEE-754 binary64");
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
  }

  void raw(const uint8_t* data, const size_t size) {
    ensure(size);
    if (size == 0) return;
    bytes_.insert(bytes_.end(), data, data + size);
  }

  void string(const std::string& value) {
    u32(static_cast<uint32_t>(value.size()));
    raw(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

  std::vector<uint8_t> finish() && { return std::move(bytes_); }

 private:
  void ensure(const size_t additional) {
    size_t result = 0;
    if (!checked_add(bytes_.size(), additional, &result) ||
        static_cast<uint64_t>(result) > kMaxEncodedBytes) {
      invalid("state", "encoded state exceeds the resource limit");
    }
    bytes_.reserve(result);
  }

  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const uint8_t* data, const size_t size) : data_(data), size_(size) {
    if (size_ != 0 && data_ == nullptr) invalid("state", "input bytes are null");
    if (static_cast<uint64_t>(size_) > kMaxEncodedBytes) {
      invalid("state", "encoded state exceeds the resource limit");
    }
  }

  uint8_t byte(const char* field) {
    const uint8_t* value = take(1, field);
    return value[0];
  }

  uint32_t u32(const char* field) {
    const uint8_t* value = take(4, field);
    return static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
  }

  uint64_t u64(const char* field) {
    const uint8_t* value = take(8, field);
    uint64_t result = 0;
    for (uint32_t shift = 0; shift < 64; shift += 8) {
      result |= static_cast<uint64_t>(value[shift / 8]) << shift;
    }
    return result;
  }

  int64_t i64(const char* field) { return static_cast<int64_t>(u64(field)); }

  float f32(const char* field) {
    const uint32_t bits = u32(field);
    float value = 0.0f;
    static_assert(sizeof(bits) == sizeof(value), "float must be IEEE-754 binary32");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  double f64(const char* field) {
    const uint64_t bits = u64(field);
    double value = 0.0;
    static_assert(sizeof(bits) == sizeof(value), "double must be IEEE-754 binary64");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  uint64_t count(const uint64_t limit, const size_t minimum_bytes, const char* field) {
    const uint64_t value = u64(field);
    if (value > limit || value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      invalid(field, "count exceeds resource or platform limits");
    }
    if (minimum_bytes != 0 && value > static_cast<uint64_t>(remaining() / minimum_bytes)) {
      invalid(field, "count exceeds bytes remaining in the input");
    }
    return value;
  }

  std::string string(const uint64_t limit, const char* field) {
    const uint32_t length = u32(field);
    if (length > limit || static_cast<size_t>(length) > remaining()) {
      invalid(field, "string exceeds resource or input limits");
    }
    const uint8_t* bytes = take(static_cast<size_t>(length), field);
    const std::string value = length == 0 ? std::string{}
                                          : std::string(reinterpret_cast<const char*>(bytes),
                                                        static_cast<size_t>(length));
    if (!valid_utf8(value)) invalid(field, "string is not valid UTF-8");
    return value;
  }

  bool boolean(const char* field) {
    const uint8_t value = byte(field);
    if (value > 1) invalid(field, "boolean must be encoded as zero or one");
    return value != 0;
  }

  size_t remaining() const noexcept { return size_ - position_; }

  void require_end() const {
    if (position_ != size_) invalid("state", "trailing bytes are not allowed");
  }

 private:
  const uint8_t* take(const size_t amount, const char* field) {
    if (amount > remaining()) invalid(field, "truncated state");
    const uint8_t* result = amount == 0 ? data_ : data_ + position_;
    position_ += amount;
    return result;
  }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t position_ = 0;
};

void write_pitch_target(Writer& writer, const VocalPitchTarget& target) {
  writer.u32(static_cast<uint32_t>(target.mode));
  writer.f64(target.center_midi);
  writer.u64(static_cast<uint64_t>(target.points.size()));
  for (const auto& point : target.points) {
    writer.f64(point.source_sample);
    writer.f64(point.target_midi);
  }
}

void write_note(Writer& writer, const VocalNote& note) {
  writer.u32(note.id);
  writer.i64(note.source_range.start);
  writer.i64(note.source_range.end);
  writer.u32(note.analysis_frame_start);
  writer.u32(note.analysis_frame_end);
  writer.byte(static_cast<uint8_t>(note.has_pitch ? 1 : 0));
  writer.f64(note.centre_midi);
  writer.f64(note.median_hz);
  writer.f64(note.f0_stability);
  write_pitch_target(writer, note.edit.pitch.target);
  writer.f64(note.edit.pitch.amount);
  writer.f64(note.edit.pitch.speed_ms);
  writer.f64(note.edit.pitch.max_correction_semitones);
  writer.f64(note.edit.pitch.transpose_semitones);
  writer.f64(note.edit.pitch.drift_scale);
  writer.f64(note.edit.pitch.vibrato_scale);
  writer.i64(note.edit.destination_start_sample);
  writer.i64(note.edit.destination_length_samples);
  writer.f64(note.edit.gain_db);
  writer.byte(static_cast<uint8_t>(note.edit.muted ? 1 : 0));
  writer.u64(static_cast<uint64_t>(note.edit.amplitude_envelope.size()));
  for (const float value : note.edit.amplitude_envelope) writer.f32(value);
  writer.u32(static_cast<uint32_t>(note.edit.formant.mode));
  writer.f64(note.edit.formant.shift_semitones);
}

VocalPitchTarget read_pitch_target(Reader& reader) {
  const uint32_t mode = reader.u32("pitch.target.mode");
  if (!valid_pitch_target_mode(mode)) invalid("pitch.target.mode", "unknown target mode");
  VocalPitchTarget target;
  target.mode = static_cast<PitchTargetMode>(mode);
  target.center_midi = reader.f64("pitch.target.center_midi");
  const uint64_t count = reader.count(kMaxPitchPoints, 16, "pitch.target.points");
  target.points.reserve(static_cast<size_t>(count));
  for (uint64_t index = 0; index < count; ++index) {
    target.points.push_back({reader.f64("pitch.target.points.source_sample"),
                             reader.f64("pitch.target.points.target_midi")});
  }
  return target;
}

VocalNote read_note(Reader& reader) {
  VocalNote note;
  note.id = reader.u32("note.id");
  note.source_range.start = reader.i64("note.source_range.start");
  note.source_range.end = reader.i64("note.source_range.end");
  note.analysis_frame_start = reader.u32("note.analysis_frame_start");
  note.analysis_frame_end = reader.u32("note.analysis_frame_end");
  note.has_pitch = reader.boolean("note.has_pitch");
  note.centre_midi = reader.f64("note.centre_midi");
  note.median_hz = reader.f64("note.median_hz");
  note.f0_stability = reader.f64("note.f0_stability");
  note.edit.pitch.target = read_pitch_target(reader);
  note.edit.pitch.amount = reader.f64("pitch.amount");
  note.edit.pitch.speed_ms = reader.f64("pitch.speed_ms");
  note.edit.pitch.max_correction_semitones = reader.f64("pitch.max_correction_semitones");
  note.edit.pitch.transpose_semitones = reader.f64("pitch.transpose_semitones");
  note.edit.pitch.drift_scale = reader.f64("pitch.drift_scale");
  note.edit.pitch.vibrato_scale = reader.f64("pitch.vibrato_scale");
  note.edit.destination_start_sample = reader.i64("destination.start");
  note.edit.destination_length_samples = reader.i64("destination.length");
  note.edit.gain_db = reader.f64("gain_db");
  note.edit.muted = reader.boolean("muted");
  const uint64_t envelope_count = reader.count(kMaxEnvelopeSamples, 4, "amplitude_envelope");
  note.edit.amplitude_envelope.reserve(static_cast<size_t>(envelope_count));
  for (uint64_t index = 0; index < envelope_count; ++index) {
    note.edit.amplitude_envelope.push_back(reader.f32("amplitude_envelope"));
  }
  const uint32_t formant_mode = reader.u32("formant.mode");
  if (!valid_formant_mode(formant_mode)) invalid("formant.mode", "unknown formant mode");
  note.edit.formant.mode = static_cast<FormantMode>(formant_mode);
  note.edit.formant.shift_semitones = reader.f64("formant.shift_semitones");
  return note;
}

void write_transition(Writer& writer, const PitchTransition& transition) {
  writer.u32(transition.left_note_id);
  writer.u32(transition.right_note_id);
  writer.i64(transition.left_window_samples);
  writer.i64(transition.right_window_samples);
  writer.f64(transition.strength);
  writer.u32(static_cast<uint32_t>(transition.curve));
}

PitchTransition read_transition(Reader& reader) {
  PitchTransition transition;
  transition.left_note_id = reader.u32("transition.left_note_id");
  transition.right_note_id = reader.u32("transition.right_note_id");
  transition.left_window_samples = reader.i64("transition.left_window_samples");
  transition.right_window_samples = reader.i64("transition.right_window_samples");
  transition.strength = reader.f64("transition.strength");
  const uint32_t curve = reader.u32("transition.curve");
  if (!valid_transition_curve(curve)) invalid("transition.curve", "unknown transition curve");
  transition.curve = static_cast<TransitionCurve>(curve);
  return transition;
}

}  // namespace

std::vector<uint8_t> encode_vocal_state(const VocalPersistedState& state) {
  validate_for_codec(state);

  Writer writer;
  writer.raw(reinterpret_cast<const uint8_t*>("SVE1"), 4);
  writer.u32(kVocalStateSchemaVersion);
  writer.u32(0);  // flags are reserved and must remain zero.
  writer.raw(state.session_id.data(), state.session_id.size());
  writer.u64(state.next_note_id);
  writer.u64(state.committed_revision);

  writer.u32(state.source.sample_rate);
  writer.i64(state.source.sample_count);
  writer.raw(state.source.digest.data(), state.source.digest.size());
  writer.i64(state.output_length_samples);

  const auto& analysis = state.analysis;
  writer.f64(analysis.grid.frame_origin_sample);
  writer.f64(analysis.grid.samples_per_frame);
  writer.u32(analysis.grid.frame_length_samples);
  writer.f64(analysis.settings.fmin_hz);
  writer.f64(analysis.settings.fmax_hz);
  writer.f64(analysis.settings.yin_threshold);
  writer.f64(analysis.settings.voiced_threshold);
  writer.byte(static_cast<uint8_t>(analysis.settings.centered ? 1 : 0));
  writer.f64(analysis.settings.segmentation_threshold_cents);
  writer.f64(analysis.settings.minimum_note_ms);
  writer.f64(analysis.settings.reference_hz);
  writer.string(analysis.algorithm_id);
  writer.u32(analysis.algorithm_version);
  writer.raw(analysis.digest.data(), analysis.digest.size());
  writer.u64(static_cast<uint64_t>(analysis.f0_hz.size()));
  for (const float value : analysis.f0_hz) writer.f32(value);
  for (const float value : analysis.amplitude) writer.f32(value);
  for (const uint8_t value : analysis.voiced) writer.byte(value);

  writer.u32(static_cast<uint32_t>(state.render_settings.profile));
  writer.u32(state.render_settings.algorithm_version);
  writer.f64(state.render_settings.edge_fade_ms);
  writer.f64(state.render_settings.vibrato_cutoff_hz);

  writer.u64(static_cast<uint64_t>(state.edit_state.notes.size()));
  for (const auto& note : state.edit_state.notes) write_note(writer, note);
  writer.u64(static_cast<uint64_t>(state.edit_state.transitions.size()));
  for (const auto& transition : state.edit_state.transitions) write_transition(writer, transition);
  return std::move(writer).finish();
}

VocalPersistedState decode_vocal_state(const uint8_t* bytes, const size_t size) {
  Reader reader(bytes, size);
  if (size < 4 || bytes == nullptr || std::memcmp(bytes, "SVE1", 4) != 0) {
    invalid("magic", "state does not have the SVE1 magic");
  }
  // The magic is checked above before Reader consumes it, so an empty input
  // and a null pointer never participate in pointer arithmetic.
  static_cast<void>(reader.byte("magic"));
  static_cast<void>(reader.byte("magic"));
  static_cast<void>(reader.byte("magic"));
  static_cast<void>(reader.byte("magic"));
  if (reader.u32("schema") != kVocalStateSchemaVersion) {
    invalid("schema", "unsupported SVE1 schema version");
  }
  if (reader.u32("flags") != 0) invalid("flags", "unknown SVE1 flags");

  VocalPersistedState state;
  for (size_t index = 0; index < state.session_id.size(); ++index) {
    state.session_id[index] = reader.byte("session_id");
  }
  state.next_note_id = reader.u64("next_note_id");
  state.committed_revision = reader.u64("committed_revision");
  state.source.sample_rate = reader.u32("source.sample_rate");
  state.source.sample_count = reader.i64("source.sample_count");
  for (size_t index = 0; index < state.source.digest.size(); ++index) {
    state.source.digest[index] = reader.byte("source.digest");
  }
  state.output_length_samples = reader.i64("output_length_samples");

  auto& analysis = state.analysis;
  analysis.grid.frame_origin_sample = reader.f64("analysis.grid.frame_origin_sample");
  analysis.grid.samples_per_frame = reader.f64("analysis.grid.samples_per_frame");
  analysis.grid.frame_length_samples = reader.u32("analysis.grid.frame_length_samples");
  analysis.settings.fmin_hz = reader.f64("analysis.settings.fmin_hz");
  analysis.settings.fmax_hz = reader.f64("analysis.settings.fmax_hz");
  analysis.settings.yin_threshold = reader.f64("analysis.settings.yin_threshold");
  analysis.settings.voiced_threshold = reader.f64("analysis.settings.voiced_threshold");
  analysis.settings.centered = reader.boolean("analysis.settings.centered");
  analysis.settings.segmentation_threshold_cents =
      reader.f64("analysis.settings.segmentation_threshold_cents");
  analysis.settings.minimum_note_ms = reader.f64("analysis.settings.minimum_note_ms");
  analysis.settings.reference_hz = reader.f64("analysis.settings.reference_hz");
  analysis.algorithm_id = reader.string(kMaxAlgorithmBytes, "analysis.algorithm_id");
  analysis.algorithm_version = reader.u32("analysis.algorithm_version");
  for (size_t index = 0; index < analysis.digest.size(); ++index) {
    analysis.digest[index] = reader.byte("analysis.digest");
  }
  const uint64_t frame_count = reader.count(kMaxFrames, 9, "analysis.frame_count");
  analysis.f0_hz.reserve(static_cast<size_t>(frame_count));
  analysis.amplitude.reserve(static_cast<size_t>(frame_count));
  analysis.voiced.reserve(static_cast<size_t>(frame_count));
  for (uint64_t index = 0; index < frame_count; ++index) {
    analysis.f0_hz.push_back(reader.f32("analysis.f0_hz"));
  }
  for (uint64_t index = 0; index < frame_count; ++index) {
    analysis.amplitude.push_back(reader.f32("analysis.amplitude"));
  }
  for (uint64_t index = 0; index < frame_count; ++index) {
    analysis.voiced.push_back(reader.byte("analysis.voiced"));
  }

  const uint32_t profile = reader.u32("render_settings.profile");
  if (!valid_render_profile(profile)) invalid("render_settings.profile", "unknown render profile");
  state.render_settings.profile = static_cast<RenderProfile>(profile);
  state.render_settings.algorithm_version = reader.u32("render_settings.algorithm_version");
  state.render_settings.edge_fade_ms = reader.f64("render_settings.edge_fade_ms");
  state.render_settings.vibrato_cutoff_hz = reader.f64("render_settings.vibrato_cutoff_hz");

  const uint64_t note_count = reader.count(kMaxNotes, 166, "notes");
  state.edit_state.notes.reserve(static_cast<size_t>(note_count));
  for (uint64_t index = 0; index < note_count; ++index) {
    state.edit_state.notes.push_back(read_note(reader));
  }
  const uint64_t transition_count = reader.count(kMaxTransitions, 36, "transitions");
  state.edit_state.transitions.reserve(static_cast<size_t>(transition_count));
  for (uint64_t index = 0; index < transition_count; ++index) {
    state.edit_state.transitions.push_back(read_transition(reader));
  }
  reader.require_end();
  validate_for_codec(state);
  return state;
}

}  // namespace sonare::editing::vocal_edit
