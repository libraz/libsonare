#include <sonare/sonare_c_transcribe.h>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
#include "analysis/bpm_analyzer.h"
#include "editing/note_model/note_transcriber.h"
#endif

// ============================================================================
// Transcription
// ============================================================================

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
namespace {

namespace ntm = sonare::editing::note_model;

constexpr int32_t kTranscribeConfigVersion = 1;

/// Records which field was refused and why, so a caller reading
/// sonare_last_error_message is told the field rather than left with a bare
/// "invalid parameter". Every rejection below goes through this: a refusal that
/// does not name its field is the one a caller cannot act on.
SonareError refuse(const char* message) {
  sonare_c_detail::set_last_error(message);
  return SONARE_ERROR_INVALID_PARAMETER;
}

/// Reads the public config into the core one. Every numeric field spells its
/// default as 0, so a zero-filled struct is the defaults; a value outside the
/// field's domain is REFUSED rather than replaced, because a silently
/// substituted default is indistinguishable downstream from a deliberate one.
SonareError read_config(const SonareTranscribeConfig* in, ntm::TranscribeConfig* out,
                        uint8_t* out_group, uint8_t* out_channel) {
  *out = ntm::TranscribeConfig{};
  *out_group = 0;
  *out_channel = 0;
  if (in == nullptr) return SONARE_OK;
  if (in->struct_version != kTranscribeConfigVersion) {
    return refuse("struct_version must be 1");
  }

  out->source = in->polyphonic != 0 ? ntm::TranscribeSource::kPolyphonic
                                    : ntm::TranscribeSource::kMonophonic;

  // Each of these is "0 keeps the default, anything else must be valid on its
  // own terms". The core validates again; refusing here is what names the
  // offending field before the audio is touched.
  if (in->reference_hz != 0.0f) {
    if (!finite_positive(in->reference_hz)) return refuse("reference_hz must be a positive number");
    out->reference_hz = in->reference_hz;
  }
  if (in->fmin != 0.0f) {
    if (!finite_positive(in->fmin)) return refuse("fmin must be a positive number");
    out->fmin = in->fmin;
  }
  if (in->fmax != 0.0f) {
    if (!finite_positive(in->fmax)) return refuse("fmax must be a positive number");
    out->fmax = in->fmax;
  }
  if (out->fmax <= out->fmin) return refuse("fmax must be above fmin");
  if (in->min_note_ms != 0.0f) {
    if (!finite_positive(in->min_note_ms)) return refuse("min_note_ms must be a positive number");
    out->min_note_ms = in->min_note_ms;
  }
  if (in->segmentation_threshold_cents != 0.0f) {
    if (!finite_positive(in->segmentation_threshold_cents)) {
      return refuse("segmentation_threshold_cents must be a positive number");
    }
    out->segmentation_threshold_cents = in->segmentation_threshold_cents;
  }
  if (in->velocity_floor_db != 0.0f) {
    if (!std::isfinite(in->velocity_floor_db) || in->velocity_floor_db >= 0.0f) {
      return refuse("velocity_floor_db must be a finite negative number");
    }
    out->velocity_floor_db = in->velocity_floor_db;
  }
  if (in->fixed_velocity != 0) {
    if (in->fixed_velocity < 1 || in->fixed_velocity > 127) {
      return refuse("fixed_velocity must be an integer in [1, 127]");
    }
    out->fixed_velocity = in->fixed_velocity;
  }
  if (in->group < 0 || in->group > 15) return refuse("group must be an integer in [0, 15]");
  if (in->channel < 0 || in->channel > 15) {
    return refuse("channel must be an integer in [0, 15]");
  }
  *out_group = static_cast<uint8_t>(in->group);
  *out_channel = static_cast<uint8_t>(in->channel);
  return SONARE_OK;
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
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) noexcept {
    if (a.ppq != b.ppq) return a.ppq < b.ppq;
    return a.order < b.order;
  });

  std::vector<SonareMidiEventPod> events;
  events.reserve(entries.size());
  for (const Entry& entry : entries) events.push_back(entry.pod);
  return events;
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

SonareTranscribeConfig sonare_transcribe_config_default(void) {
  SonareTranscribeConfig config = {};
  config.struct_version = 1;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  // Seeded from the core defaults so the two cannot drift apart.
  const ntm::TranscribeConfig defaults;
  config.polyphonic = defaults.source == ntm::TranscribeSource::kPolyphonic ? 1 : 0;
  config.reference_hz = defaults.reference_hz;
  config.fmin = defaults.fmin;
  config.fmax = defaults.fmax;
  config.min_note_ms = defaults.min_note_ms;
  config.segmentation_threshold_cents = defaults.segmentation_threshold_cents;
  config.velocity_floor_db = defaults.velocity_floor_db;
  config.fixed_velocity = defaults.fixed_velocity;
#endif
  return config;
}

void sonare_free_transcribe_result(SonareTranscribeResult* result) {
  if (result == nullptr) return;
  delete[] result->events;
  result->events = nullptr;
  result->count = 0;
  result->note_count = 0;
  result->tempo_bpm = 0.0f;
}

SonareError sonare_transcribe(const float* samples, size_t length, int sample_rate,
                              float tempo_bpm, const SonareTranscribeConfig* config,
                              SonareTranscribeResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  // The family's buffer policy, non-finite scan included. Transcription needs it
  // as much as any of them: a buffer of NaN tracks no pitch, so without the scan
  // it would answer SONARE_OK with zero notes -- an answer a caller cannot tell
  // apart from silence.
  const SonareError audio_error = validate_audio_params(samples, length, sample_rate);
  if (audio_error != SONARE_OK) return audio_error;
  ntm::TranscribeConfig core_config;
  uint8_t group = 0;
  uint8_t channel = 0;
  const SonareError config_error = read_config(config, &core_config, &group, &channel);
  if (config_error != SONARE_OK) return config_error;

  SONARE_C_TRY
  const sonare::Audio audio = sonare::Audio::from_buffer(samples, length, sample_rate);
  const float tempo = resolve_tempo(tempo_bpm, audio);

  sonare::transport::TempoMap map;
  map.prepare(sample_rate);
  map.set_segments({sonare::transport::TempoSegment{0.0, static_cast<double>(tempo), 0.0}});
  map.set_time_signatures({sonare::transport::TimeSignatureSegment{0.0, {4, 4}}});

  const std::vector<ntm::TranscribedNote> notes = ntm::transcribe_notes(audio, core_config);
  const std::vector<SonareMidiEventPod> events =
      build_events(notes, map, sample_rate, group, channel);

  out->tempo_bpm = tempo;
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
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
  if (out_note_count) *out_note_count = 0;
  if (project == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const SonareError audio_error = validate_audio_params(samples, length, sample_rate);
  if (audio_error != SONARE_OK) return audio_error;
  ntm::TranscribeConfig core_config;
  uint8_t group = 0;
  uint8_t channel = 0;
  const SonareError config_error = read_config(config, &core_config, &group, &channel);
  if (config_error != SONARE_OK) return config_error;

  std::vector<SonareMidiEventPod> events;
  SONARE_C_TRY
  const sonare::Audio audio = sonare::Audio::from_buffer(samples, length, sample_rate);
  // The project's own map, not a second detection: a project whose tempo was
  // installed by sonare_project_auto_tempo transcribes onto that grid.
  sonare::transport::TempoMap map;
  fill_project_tempo_map(project->history.project(), &map);
  events = build_events(ntm::transcribe_notes(audio, core_config), map, sample_rate, group,
                        channel);
  SONARE_C_CATCH

  // Committed through the public setter so the clip validation, the undo entry
  // and the content-store bookkeeping are the same ones a hand-built event list
  // goes through.
  const SonareError error = sonare_project_set_midi_events(
      project, clip_id, events.empty() ? nullptr : events.data(), events.size());
  if (error != SONARE_OK) return error;
  if (out_note_count) *out_note_count = events.size() / 2u;
  return SONARE_OK;
#else
  if (out_note_count) *out_note_count = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, samples, length, sample_rate, config,
                              out_note_count);
#endif
}
