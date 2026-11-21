#include <sonare/sonare_c_transcribe.h>

#include <cmath>
#include <limits>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
#include "analysis/bpm_analyzer.h"
#include "editing/note_model/note_transcriber.h"
#include "editing/polyphony/multi_f0.h"
#include "feature/chroma.h"
#include "feature/pitch.h"
#include "util/insertion_sort.h"
#include "util/parameter_checks.h"
#endif

using sonare::constants::kCentsPerSemitone;

// ============================================================================
// Transcription
// ============================================================================

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
namespace {

namespace ntm = sonare::editing::note_model;

constexpr int32_t kMaxMinNoteDivision = 128;

/// Refuses @p subject, named by its C field, so each binding respells it in its own keys.
[[noreturn]] void refuse(const char* subject, const char* predicate) {
  throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::InvalidParameter, subject,
                                                  predicate);
}

[[noreturn]] void refuse_pair(const char* a, const char* b, const char* text) {
  sonare::util::check::refuse_relation({a, b}, text);
}

/// Reads the public config into the core one. A NaN float and an UNSET integer are absent and
/// resolve from the selected source; every other value is checked against the field's domain and
/// REFUSED when outside it, because a silently substituted default is indistinguishable
/// downstream from a deliberate one. The core spells some defaults as 0 (and a real 0 as a
/// negative), which is mapped here and nowhere else.
void read_config(const SonareTranscribeConfig* in, ntm::TranscribeConfig* out, uint8_t* out_group,
                 uint8_t* out_channel, bool* out_reference_auto) {
  *out = ntm::TranscribeConfig{};
  *out_group = 0;
  *out_channel = 0;
  *out_reference_auto = false;
  if (in == nullptr) return;
  // Under run_validation_under("config"), so the subject is the bare field.
  if (in->struct_size < sizeof(SonareTranscribeConfig)) {
    refuse("struct_size", "must be set by sonare_transcribe_config_init");
  }
  *out_reference_auto = in->reference_auto != 0;

  out->source =
      in->polyphonic != 0 ? ntm::TranscribeSource::kPolyphonic : ntm::TranscribeSource::kMonophonic;

  if (!finite_positive(in->reference_hz)) refuse("reference_hz", "must be a positive number");
  out->reference_hz = in->reference_hz;
  if (!std::isnan(in->f0_min_hz)) {
    if (!finite_positive(in->f0_min_hz)) refuse("f0_min_hz", "must be a positive number");
    out->fmin = in->f0_min_hz;
  }
  if (!std::isnan(in->f0_max_hz)) {
    if (!finite_positive(in->f0_max_hz)) refuse("f0_max_hz", "must be a positive number");
    out->fmax = in->f0_max_hz;
  }
  const ntm::TranscribeF0Range range = ntm::resolve_transcribe_f0_range(*out);
  if (range.fmax <= range.fmin) {
    refuse_pair("f0_max_hz", "f0_min_hz", "{0} must be finite and greater than {1}");
  }
  const bool min_note_ms_given = !std::isnan(in->min_note_ms);
  if (min_note_ms_given) {
    if (!finite_positive(in->min_note_ms)) refuse("min_note_ms", "must be a positive number");
    out->min_note_ms = in->min_note_ms;
  }
  if (!finite_positive(in->segmentation_threshold_cents)) {
    refuse("segmentation_threshold_cents", "must be a positive number");
  }
  out->segmentation_threshold_cents = in->segmentation_threshold_cents;
  if (!std::isfinite(in->velocity_floor_db) || in->velocity_floor_db >= 0.0f) {
    refuse("velocity_floor_db", "must be a finite negative number");
  }
  out->velocity_floor_db = in->velocity_floor_db;
  if (in->fixed_velocity != SONARE_FIXED_VELOCITY_UNSET) {
    if (in->fixed_velocity < 1 || in->fixed_velocity > 127) {
      refuse("fixed_velocity", "must be an integer in [1, 127]");
    }
    out->fixed_velocity = in->fixed_velocity;
  }
  if (in->group < 0 || in->group > 15) refuse("group", "must be an integer in [0, 15]");
  if (in->channel < 0 || in->channel > 15) {
    refuse("channel", "must be an integer in [0, 15]");
  }
  *out_group = static_cast<uint8_t>(in->group);
  *out_channel = static_cast<uint8_t>(in->channel);

  const bool polyphonic = out->source == ntm::TranscribeSource::kPolyphonic;
  if (in->max_polyphony != SONARE_MAX_POLYPHONY_UNSET &&
      (in->max_polyphony < 1 ||
       in->max_polyphony > sonare::editing::polyphony::kMaxPolyphonyVoices)) {
    refuse("max_polyphony", "must be an integer in [1, 64]");
  }
  if (!std::isnan(in->min_frame_peak_ratio) &&
      !(in->min_frame_peak_ratio >= 0.0f && in->min_frame_peak_ratio <= 1.0f)) {
    refuse("min_frame_peak_ratio", "must be a finite number in [0, 1]");
  }
  if (!std::isnan(in->min_ridge_peak_ratio) &&
      !(in->min_ridge_peak_ratio >= 0.0f && in->min_ridge_peak_ratio <= 1.0f)) {
    refuse("min_ridge_peak_ratio", "must be a finite number in [0, 1]");
  }
  if (!std::isnan(in->reattack_ratio) &&
      (!std::isfinite(in->reattack_ratio) || in->reattack_ratio <= 1.0f)) {
    refuse("reattack_ratio", "must be a finite number above 1");
  }
  if (in->min_note_division != SONARE_NOTE_DIVISION_UNSET &&
      (in->min_note_division < 1 || in->min_note_division > kMaxMinNoteDivision)) {
    refuse("min_note_division", "must be an integer in [1, 128]");
  }
  if (in->min_note_division != SONARE_NOTE_DIVISION_UNSET && min_note_ms_given) {
    refuse_pair("min_note_division", "min_note_ms", "{0} and {1} cannot both be set");
  }
  out->min_note_division = in->min_note_division;
  if (!polyphonic) {
    if (in->max_polyphony != SONARE_MAX_POLYPHONY_UNSET) {
      refuse("max_polyphony", "requires polyphonic transcription");
    }
    if (!std::isnan(in->min_frame_peak_ratio)) {
      refuse("min_frame_peak_ratio", "requires polyphonic transcription");
    }
    if (!std::isnan(in->min_ridge_peak_ratio)) {
      refuse("min_ridge_peak_ratio", "requires polyphonic transcription");
    }
    if (!std::isnan(in->reattack_ratio)) {
      refuse("reattack_ratio", "requires polyphonic transcription");
    }
  }
  out->max_polyphony = in->max_polyphony;
  // The core reads 0 as its default and a negative as a real 0.
  const auto core_ratio = [](float given) {
    if (std::isnan(given)) return 0.0f;
    return given == 0.0f ? -1.0f : given;
  };
  out->min_frame_peak_ratio = core_ratio(in->min_frame_peak_ratio);
  out->min_ridge_peak_ratio = core_ratio(in->min_ridge_peak_ratio);
  // The core spells "no split" as a negative ratio and reads 0 as its own default.
  out->reattack_ratio =
      std::isnan(in->reattack_ratio) ? (polyphonic ? -1.0f : 0.0f) : in->reattack_ratio;

  if (in->monophonic_model != 0 && in->monophonic_model != 1) {
    refuse("monophonic_model", "must be 0 (segments) or 1 (singing)");
  }
  if (polyphonic && in->monophonic_model != 0) {
    refuse("monophonic_model", "requires monophonic transcription");
  }
  out->monophonic_model =
      in->monophonic_model == 1 ? ntm::MonophonicModel::kSinging : ntm::MonophonicModel::kSegments;
}

/// Converts a sample index in the SOURCE's rate to PPQ on @p map, whose own rate
/// is the project's and need not match. Going through seconds is what keeps a
/// 44.1 kHz take from landing on a 48 kHz project's grid a percent early.
double sample_to_ppq_on(const sonare::transport::TempoMap& map, int64_t sample,
                        int source_sample_rate) {
  const double seconds = static_cast<double>(sample) / static_cast<double>(source_sample_rate);
  return map.sample_to_ppq(static_cast<int64_t>(std::llround(seconds * map.sample_rate())));
}

/// Builds the note-on / note-off pairs for @p notes on @p map.
///
/// Ordering is (ppq, note-off before note-on). A note-off sharing a tick with
/// the next note's on has to come first: a consumer that plays the events in
/// order otherwise starts the new note and immediately stops it when the two are
/// the same pitch, which is what a legato repeat looks like.
std::vector<SonareMidiEventPod> build_events(const std::vector<ntm::TranscribedNote>& notes,
                                             const sonare::transport::TempoMap& map,
                                             int source_sample_rate, uint8_t group,
                                             uint8_t channel) {
  struct Entry {
    double ppq;
    int order;  // 0 = note-off, 1 = note-on
    SonareMidiEventPod pod;
  };
  std::vector<Entry> entries;
  entries.reserve(notes.size() * 2u);
  for (const ntm::TranscribedNote& note : notes) {
    const double on_ppq = sample_to_ppq_on(map, note.onset_sample, source_sample_rate);
    const double off_ppq = sample_to_ppq_on(map, note.offset_sample, source_sample_rate);
    if (!sonare::transport::valid_public_ppq(on_ppq) ||
        !sonare::transport::valid_public_ppq(off_ppq) || off_ppq <= on_ppq) {
      continue;
    }
    entries.push_back({on_ppq, 1,
                       pod_from_ump(on_ppq, sonare::midi::make_midi1_note_on(
                                                group, channel, note.note, note.velocity))});
    entries.push_back(
        {off_ppq, 0,
         pod_from_ump(off_ppq, sonare::midi::make_midi1_note_off(group, channel, note.note, 0))});
  }
  sonare::insertion_sort(entries.begin(), entries.end(),
                         [](const Entry& a, const Entry& b) noexcept {
                           if (a.ppq != b.ppq) return a.ppq < b.ppq;
                           return a.order < b.order;
                         });

  std::vector<SonareMidiEventPod> events;
  events.reserve(entries.size());
  for (const Entry& entry : entries) events.push_back(entry.pod);
  return events;
}

/// Settles the tuning reference against @p audio: measured under @p reference_auto, otherwise
/// the one @p config carries. Returns the tuning it amounts to, in fractions of a semitone.
float resolve_reference(const sonare::Audio& audio, bool reference_auto,
                        ntm::TranscribeConfig* config) {
  if (reference_auto) {
    config->reference_hz = sonare::tuning_to_reference_hz(sonare::measure_tuning(audio));
  }
  return sonare::reference_hz_to_tuning(config->reference_hz);
}

/// The tempo the grid is built on. A caller's value is taken as given; otherwise
/// it is detected, and a detector that answers with nothing usable falls back to
/// the project default rather than refusing the transcription.
float resolve_tempo(float requested, const sonare::Audio& audio) {
  if (std::isfinite(requested) && requested > 0.0f) return requested;
  const float detected = sonare::BpmAnalyzer(audio).bpm();
  if (std::isfinite(detected) && detected > 0.0f) return detected;
  return static_cast<float>(sonare::constants::kDefaultBpm);
}

}  // namespace
#endif

void sonare_transcribe_config_init(SonareTranscribeConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  config->f0_min_hz = kNaN;
  config->f0_max_hz = kNaN;
  config->min_note_ms = kNaN;
  config->min_frame_peak_ratio = kNaN;
  config->min_ridge_peak_ratio = kNaN;
  config->reattack_ratio = kNaN;
  config->fixed_velocity = SONARE_FIXED_VELOCITY_UNSET;
  config->max_polyphony = SONARE_MAX_POLYPHONY_UNSET;
  config->min_note_division = SONARE_NOTE_DIVISION_UNSET;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  // Seeded from the core defaults so the two cannot drift apart. The range, minimum note and
  // the polyphonic-only fields stay absent: the selected source resolves each, so flipping
  // polyphonic must not pin the monophonic values.
  const ntm::TranscribeConfig defaults;
  config->polyphonic = defaults.source == ntm::TranscribeSource::kPolyphonic ? 1 : 0;
  config->reference_hz = defaults.reference_hz;
  config->monophonic_model = defaults.monophonic_model == ntm::MonophonicModel::kSinging ? 1 : 0;
  config->segmentation_threshold_cents = defaults.segmentation_threshold_cents;
  config->velocity_floor_db = defaults.velocity_floor_db;
#endif
}

void sonare_free_transcribe_result(SonareTranscribeResult* result) {
  if (result == nullptr) return;
  delete[] result->events;
  *result = {};
}

SonareError sonare_transcribe(const float* samples, size_t length, int sample_rate, float tempo_bpm,
                              const SonareTranscribeConfig* config, SonareTranscribeResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_ZERO_OUTS(out);
  // The family's buffer policy, non-finite scan included. Transcription needs it
  // as much as any of them: a buffer of NaN tracks no pitch, so without the scan
  // it would answer SONARE_OK with zero notes -- an answer a caller cannot tell
  // apart from silence.
  const SonareError audio_error =
      validate_audio_params("transcribe", "samples", samples, length, sample_rate);
  if (audio_error != SONARE_OK) return audio_error;
  ntm::TranscribeConfig core_config;
  uint8_t group = 0;
  uint8_t channel = 0;
  bool reference_auto = false;
  const SonareError config_error = sonare_c_detail::run_validation_under(
      "config", [&] { read_config(config, &core_config, &group, &channel, &reference_auto); });
  if (config_error != SONARE_OK) return config_error;
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  const sonare::Audio audio = sonare::Audio::from_buffer(samples, length, sample_rate);
  const float tuning = resolve_reference(audio, reference_auto, &core_config);
  const float tempo = resolve_tempo(tempo_bpm, audio);
  core_config.tempo_bpm = tempo;

  sonare::transport::TempoMap map;
  map.prepare(sample_rate);
  map.set_segments({sonare::transport::TempoSegment{0.0, static_cast<double>(tempo), 0.0}});
  map.set_time_signatures({sonare::transport::TimeSignatureSegment{0.0, {4, 4}}});

  const std::vector<ntm::TranscribedNote> notes = ntm::transcribe_notes(audio, core_config);
  const std::vector<SonareMidiEventPod> events =
      build_events(notes, map, sample_rate, group, channel);

  out->tempo_bpm = tempo;
  out->tuning_cents = tuning * kCentsPerSemitone;
  out->note_count = events.size() / 2u;
  if (events.empty()) return SONARE_OK;
  auto owned = std::make_unique<SonareMidiEventPod[]>(events.size());
  std::copy(events.begin(), events.end(), owned.get());
  out->count = events.size();
  out->events = owned.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, tempo_bpm, config, out);
#endif
}

SonareError sonare_project_transcribe_to_clip(SonareProject* project, uint32_t clip_id,
                                              const float* samples, size_t length, int sample_rate,
                                              const SonareTranscribeConfig* config,
                                              size_t* out_note_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_note_count);
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(project);
  const SonareError audio_error =
      validate_audio_params("project_transcribe_to_clip", "samples", samples, length, sample_rate);
  if (audio_error != SONARE_OK) return audio_error;
  ntm::TranscribeConfig core_config;
  uint8_t group = 0;
  uint8_t channel = 0;
  bool reference_auto = false;
  const SonareError config_error = sonare_c_detail::run_validation_under(
      "config", [&] { read_config(config, &core_config, &group, &channel, &reference_auto); });
  if (config_error != SONARE_OK) return config_error;

  SONARE_C_TRY
  const sonare::Audio audio = sonare::Audio::from_buffer(samples, length, sample_rate);
  // The project's own map, not a second detection: a project whose tempo was
  // installed by sonare_project_auto_tempo transcribes onto that grid.
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  // Note values are read at the tempo the clip starts on.
  core_config.tempo_bpm = static_cast<float>(map.bpm_at_sample(0));
  resolve_reference(audio, reference_auto, &core_config);
  const std::vector<SonareMidiEventPod> events =
      build_events(ntm::transcribe_notes(audio, core_config), map, sample_rate, group, channel);

  // Committed through the public setter so the clip validation, the undo entry
  // and the content-store bookkeeping are the same ones a hand-built event list
  // goes through.
  const SonareError error = sonare_project_set_midi_events(
      project, clip_id, events.empty() ? nullptr : events.data(), events.size());
  if (error != SONARE_OK) return error;
  if (out_note_count) *out_note_count = events.size() / 2u;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, samples, length, sample_rate, config,
                              out_note_count);
#endif
}
