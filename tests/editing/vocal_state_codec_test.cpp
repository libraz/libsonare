#include "editing/vocal_edit/state_codec.h"
#include "editing/vocal_edit/analysis.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

using sonare::editing::vocal_edit::AnalysisGrid;
using sonare::editing::vocal_edit::FormantMode;
using sonare::editing::vocal_edit::PitchTargetMode;
using sonare::editing::vocal_edit::RenderProfile;
using sonare::editing::vocal_edit::VocalAnalysisData;
using sonare::editing::vocal_edit::VocalEditState;
using sonare::editing::vocal_edit::VocalNote;
using sonare::editing::vocal_edit::VocalNoteEdit;
using sonare::editing::vocal_edit::VocalPersistedState;
using sonare::editing::vocal_edit::decode_vocal_state;
using sonare::editing::vocal_edit::digest_analysis;
using sonare::editing::vocal_edit::encode_vocal_state;

namespace {

std::vector<uint8_t> bytes_from_hex(const std::string_view hex) {
  if (hex.size() % 2 != 0) return {};
  const auto nibble = [](const char value) -> uint8_t {
    if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<uint8_t>(value - 'A' + 10);
    return 0xff;
  };
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const uint8_t high = nibble(hex[i]);
    const uint8_t low = nibble(hex[i + 1]);
    if (high == 0xff || low == 0xff) return {};
    bytes.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return bytes;
}

std::vector<uint8_t> golden_bytes() {
  // Independent SVE1 fixture. This literal is generated from the wire
  // contract, rather than by calling encode_vocal_state.
  return bytes_from_hex(
      "535645310100000000000000101112131415161718191a1b1c1d1e1f0900000000000000110000000000000080bb0000e001"
      "000000000000a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf000200000000000000000000"
      "000029400000000000086e400004000000000000000054400000000000209c40000000000000c03f000000000000e43f0100"
      "0000000000454000000000000039400000000000a07b400e0000006c6962736f6e6172652e7079696e01000000d64617bcfc"
      "9dc3b12e0c74dc535792f42b3f951cbb1bbd2489b316fcffae5646020000000000000012d08243000000000000803e000000"
      "3e0100010000000100000000000000000012400000000000000c400100000000000000010000000000000000000000e00100"
      "00000000000000000002000000010000000000004e403480b740025a7040000000000000ec3f020000000000000000000000"
      "020000000000000000000000000000000000000000004e400000000000007e400000000000c04e40000000000000e83f0000"
      "000000003e400000000000001440000000000000d0bf000000000000e03f000000000000f43f0400000000000000e0010000"
      "00000000000000000000f8bf0002000000000000000000003f0000803f0100000000000000000000400000000000000000");
}

void write_u32_le(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  REQUIRE(offset + 4 <= bytes.size());
  for (size_t shift = 0; shift < 32; shift += 8) {
    bytes[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
  }
}

void write_u64_le(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value) {
  REQUIRE(offset + 8 <= bytes.size());
  for (size_t shift = 0; shift < 64; shift += 8) {
    bytes[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
  }
}

VocalPersistedState fixture() {
  VocalPersistedState state;
  for (size_t i = 0; i < state.session_id.size(); ++i) {
    state.session_id[i] = static_cast<uint8_t>(0x10u + i);
  }
  state.next_note_id = 9;
  state.committed_revision = 17;
  state.source.sample_rate = 48000;
  state.source.sample_count = 480;
  for (size_t i = 0; i < state.source.digest.size(); ++i) {
    state.source.digest[i] = static_cast<uint8_t>(0xa0u + i);
  }
  state.output_length_samples = 512;

  state.analysis.grid = AnalysisGrid{12.5, 240.25, 1024};
  state.analysis.settings.fmin_hz = 80.0;
  state.analysis.settings.fmax_hz = 1800.0;
  state.analysis.settings.yin_threshold = 0.125;
  state.analysis.settings.voiced_threshold = 0.625;
  state.analysis.settings.centered = true;
  state.analysis.settings.segmentation_threshold_cents = 42.0;
  state.analysis.settings.minimum_note_ms = 25.0;
  state.analysis.settings.reference_hz = 442.0;
  state.analysis.f0_hz = {261.62555f, 0.0f};
  state.analysis.amplitude = {0.25f, 0.125f};
  state.analysis.voiced = {1, 0};
  state.analysis.algorithm_id = "libsonare.pyin";
  state.analysis.algorithm_version = 1;
  state.analysis.digest = digest_analysis(state.analysis);

  VocalNote note;
  note.id = 1;
  note.content_generation = 99;  // runtime invalidation hint; never persisted.
  note.source_range = {0, 480};
  note.analysis_frame_start = 0;
  note.analysis_frame_end = 2;
  note.has_pitch = true;
  note.centre_midi = 60.0;
  note.median_hz = 261.62555;
  note.f0_stability = 0.875;
  note.edit = VocalNoteEdit::identity_for(note.source_range);
  note.edit.pitch.target.mode = PitchTargetMode::kCurve;
  note.edit.pitch.target.points = {{0.0, 60.0}, {480.0, 61.5}};
  note.edit.pitch.amount = 0.75;
  note.edit.pitch.speed_ms = 30.0;
  note.edit.pitch.max_correction_semitones = 5.0;
  note.edit.pitch.transpose_semitones = -0.25;
  note.edit.pitch.drift_scale = 0.5;
  note.edit.pitch.vibrato_scale = 1.25;
  note.edit.destination_start_sample = 4;
  note.edit.destination_length_samples = 480;
  note.edit.gain_db = -1.5;
  note.edit.amplitude_envelope = {0.5f, 1.0f};
  note.edit.formant.mode = FormantMode::kShift;
  note.edit.formant.shift_semitones = 2.0;
  state.edit_state.notes = {note};

  state.render_settings.profile = RenderProfile::kVocalPsolaV1;
  state.render_settings.algorithm_version = 1;
  state.render_settings.edge_fade_ms = 4.5;
  state.render_settings.vibrato_cutoff_hz = 3.5;
  return state;
}

void check_equal(const VocalPersistedState& lhs, const VocalPersistedState& rhs) {
  CHECK(lhs.session_id == rhs.session_id);
  CHECK(lhs.next_note_id == rhs.next_note_id);
  CHECK(lhs.committed_revision == rhs.committed_revision);
  CHECK(lhs.source.sample_rate == rhs.source.sample_rate);
  CHECK(lhs.source.sample_count == rhs.source.sample_count);
  CHECK(lhs.source.digest == rhs.source.digest);
  CHECK(lhs.output_length_samples == rhs.output_length_samples);
  CHECK(lhs.analysis.grid.frame_origin_sample == rhs.analysis.grid.frame_origin_sample);
  CHECK(lhs.analysis.grid.samples_per_frame == rhs.analysis.grid.samples_per_frame);
  CHECK(lhs.analysis.grid.frame_length_samples == rhs.analysis.grid.frame_length_samples);
  CHECK(lhs.analysis.settings.fmin_hz == rhs.analysis.settings.fmin_hz);
  CHECK(lhs.analysis.settings.fmax_hz == rhs.analysis.settings.fmax_hz);
  CHECK(lhs.analysis.settings.yin_threshold == rhs.analysis.settings.yin_threshold);
  CHECK(lhs.analysis.settings.voiced_threshold == rhs.analysis.settings.voiced_threshold);
  CHECK(lhs.analysis.settings.centered == rhs.analysis.settings.centered);
  CHECK(lhs.analysis.settings.segmentation_threshold_cents ==
        rhs.analysis.settings.segmentation_threshold_cents);
  CHECK(lhs.analysis.settings.minimum_note_ms == rhs.analysis.settings.minimum_note_ms);
  CHECK(lhs.analysis.settings.reference_hz == rhs.analysis.settings.reference_hz);
  CHECK(lhs.analysis.f0_hz == rhs.analysis.f0_hz);
  CHECK(lhs.analysis.amplitude == rhs.analysis.amplitude);
  CHECK(lhs.analysis.voiced == rhs.analysis.voiced);
  CHECK(lhs.analysis.algorithm_id == rhs.analysis.algorithm_id);
  CHECK(lhs.analysis.algorithm_version == rhs.analysis.algorithm_version);
  CHECK(lhs.analysis.digest == rhs.analysis.digest);
  CHECK(lhs.edit_state.notes.size() == rhs.edit_state.notes.size());
  REQUIRE(lhs.edit_state.notes.size() == rhs.edit_state.notes.size());
  for (size_t i = 0; i < lhs.edit_state.notes.size(); ++i) {
    const auto& a = lhs.edit_state.notes[i];
    const auto& b = rhs.edit_state.notes[i];
    CHECK(a.id == b.id);
    CHECK(a.content_generation == 0);
    CHECK(b.content_generation == 0);
    CHECK(a.source_range.start == b.source_range.start);
    CHECK(a.source_range.end == b.source_range.end);
    CHECK(a.analysis_frame_start == b.analysis_frame_start);
    CHECK(a.analysis_frame_end == b.analysis_frame_end);
    CHECK(a.has_pitch == b.has_pitch);
    CHECK(a.centre_midi == b.centre_midi);
    CHECK(a.median_hz == b.median_hz);
    CHECK(a.f0_stability == b.f0_stability);
    CHECK(a.edit.pitch.target.mode == b.edit.pitch.target.mode);
    CHECK(a.edit.pitch.target.center_midi == b.edit.pitch.target.center_midi);
    CHECK(a.edit.pitch.target.points.size() == b.edit.pitch.target.points.size());
    REQUIRE(a.edit.pitch.target.points.size() == b.edit.pitch.target.points.size());
    for (size_t point = 0; point < a.edit.pitch.target.points.size(); ++point) {
      CHECK(a.edit.pitch.target.points[point].source_sample ==
            b.edit.pitch.target.points[point].source_sample);
      CHECK(a.edit.pitch.target.points[point].target_midi ==
            b.edit.pitch.target.points[point].target_midi);
    }
    CHECK(a.edit.pitch.amount == b.edit.pitch.amount);
    CHECK(a.edit.pitch.speed_ms == b.edit.pitch.speed_ms);
    CHECK(a.edit.pitch.max_correction_semitones == b.edit.pitch.max_correction_semitones);
    CHECK(a.edit.pitch.transpose_semitones == b.edit.pitch.transpose_semitones);
    CHECK(a.edit.pitch.drift_scale == b.edit.pitch.drift_scale);
    CHECK(a.edit.pitch.vibrato_scale == b.edit.pitch.vibrato_scale);
    CHECK(a.edit.destination_start_sample == b.edit.destination_start_sample);
    CHECK(a.edit.destination_length_samples == b.edit.destination_length_samples);
    CHECK(a.edit.gain_db == b.edit.gain_db);
    CHECK(a.edit.muted == b.edit.muted);
    CHECK(a.edit.amplitude_envelope == b.edit.amplitude_envelope);
    CHECK(a.edit.formant.mode == b.edit.formant.mode);
    CHECK(a.edit.formant.shift_semitones == b.edit.formant.shift_semitones);
  }
  REQUIRE(lhs.edit_state.transitions.size() == rhs.edit_state.transitions.size());
  for (size_t i = 0; i < lhs.edit_state.transitions.size(); ++i) {
    const auto& a = lhs.edit_state.transitions[i];
    const auto& b = rhs.edit_state.transitions[i];
    CHECK(a.left_note_id == b.left_note_id);
    CHECK(a.right_note_id == b.right_note_id);
    CHECK(a.left_window_samples == b.left_window_samples);
    CHECK(a.right_window_samples == b.right_window_samples);
    CHECK(a.strength == b.strength);
    CHECK(a.curve == b.curve);
    CHECK(a.content_generation == 0);
    CHECK(b.content_generation == 0);
  }
  CHECK(lhs.render_settings.profile == rhs.render_settings.profile);
  CHECK(lhs.render_settings.algorithm_version == rhs.render_settings.algorithm_version);
  CHECK(lhs.render_settings.edge_fade_ms == rhs.render_settings.edge_fade_ms);
  CHECK(lhs.render_settings.vibrato_cutoff_hz == rhs.render_settings.vibrato_cutoff_hz);
}

}  // namespace

TEST_CASE("SVE1 golden bytes are canonical little endian", "[vocal_state_codec]") {
  const auto bytes = encode_vocal_state(fixture());
  const auto expected = golden_bytes();
  REQUIRE(expected.size() == 499);
  CHECK(bytes == expected);
}

TEST_CASE("SVE1 round trip preserves authored state and drops runtime generation",
          "[vocal_state_codec]") {
  const auto decoded = decode_vocal_state(encode_vocal_state(fixture()));
  auto expected = fixture();
  expected.edit_state.notes.front().content_generation = 0;
  check_equal(expected, decoded);
}

TEST_CASE("SVE1 accepts the built-in host analysis provider", "[vocal_state_codec]") {
  auto state = fixture();
  state.analysis.algorithm_id = "host";
  state.analysis.digest = digest_analysis(state.analysis);
  const auto decoded = decode_vocal_state(encode_vocal_state(state));
  CHECK(decoded.analysis.algorithm_id == "host");
  CHECK(decoded.analysis.digest == state.analysis.digest);
}

TEST_CASE("SVE1 rejects truncation and trailing bytes", "[vocal_state_codec]") {
  const auto encoded = encode_vocal_state(fixture());
  REQUIRE(encoded.size() > 1);
  for (size_t size = 0; size < encoded.size(); ++size) {
    CHECK_THROWS(decode_vocal_state(encoded.data(), size));
  }
  auto trailing = encoded;
  trailing.push_back(0);
  CHECK_THROWS(decode_vocal_state(trailing));
}

TEST_CASE("SVE1 rejects unsupported schema, flags, enums, algorithm and malformed bools",
          "[vocal_state_codec]") {
  const auto encoded = encode_vocal_state(fixture());
  auto schema = encoded;
  write_u32_le(schema, 4, 2);
  CHECK_THROWS(decode_vocal_state(schema));
  auto flags = encoded;
  write_u32_le(flags, 8, 1);
  CHECK_THROWS(decode_vocal_state(flags));
  auto bad_utf8 = encoded;
  bad_utf8[177] = 0xff;  // algorithm_id payload
  CHECK_THROWS(decode_vocal_state(bad_utf8));
  auto unknown_algorithm = encoded;
  unknown_algorithm[177] = 'x';
  CHECK_THROWS(decode_vocal_state(unknown_algorithm));
  auto unknown_analysis_version = encoded;
  write_u32_le(unknown_analysis_version, 191, 2);
  CHECK_THROWS(decode_vocal_state(unknown_analysis_version));
  auto unknown_render_version = encoded;
  write_u32_le(unknown_render_version, 257, 2);
  CHECK_THROWS(decode_vocal_state(unknown_render_version));
  auto bad_centered = encoded;
  bad_centered[148] = 2;
  CHECK_THROWS(decode_vocal_state(bad_centered));
  auto bad_profile = encoded;
  write_u32_le(bad_profile, 253, 2);
  CHECK_THROWS(decode_vocal_state(bad_profile));
  auto bad_target = encoded;
  write_u32_le(bad_target, 338, 99);
  CHECK_THROWS(decode_vocal_state(bad_target));
  auto bad_formant = encoded;
  write_u32_le(bad_formant, 479, 99);
  CHECK_THROWS(decode_vocal_state(bad_formant));
  auto bad_has_pitch = encoded;
  bad_has_pitch[313] = 2;
  CHECK_THROWS(decode_vocal_state(bad_has_pitch));
  auto bad_muted = encoded;
  bad_muted[462] = 2;
  CHECK_THROWS(decode_vocal_state(bad_muted));
  auto bad_digest = encoded;
  bad_digest[195] ^= 1;
  CHECK_THROWS(decode_vocal_state(bad_digest));
}

TEST_CASE("SVE1 checks counts before allocating", "[vocal_state_codec]") {
  auto encoded = encode_vocal_state(fixture());
  REQUIRE(encoded.size() == 499);
  auto frames = encoded;
  write_u64_le(frames, 227, std::numeric_limits<uint64_t>::max());
  CHECK_THROWS(decode_vocal_state(frames));
  auto notes = encoded;
  write_u64_le(notes, 277, std::numeric_limits<uint64_t>::max());
  CHECK_THROWS(decode_vocal_state(notes));
  auto points = encoded;
  write_u64_le(points, 350, std::numeric_limits<uint64_t>::max());
  CHECK_THROWS(decode_vocal_state(points));
  auto envelope = encoded;
  write_u64_le(envelope, 463, std::numeric_limits<uint64_t>::max());
  CHECK_THROWS(decode_vocal_state(envelope));
}
