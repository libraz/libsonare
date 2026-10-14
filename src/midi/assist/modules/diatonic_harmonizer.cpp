#include "midi/assist/modules/diatonic_harmonizer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "midi/assist/assist_budget_detail.h"
#include "midi/assist/clip_ppq.h"
#include "midi/assist/modules/music_theory.h"
#include "midi/note_pairing.h"
#include "midi/ump.h"
#include "util/json.h"

namespace sonare::midi::assist::modules {

namespace {

namespace json = sonare::util::json;

/// One sounding note recovered from a clip's event list.
struct SourceNote {
  double content_ppq = 0.0;
  double length_ppq = 0.0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
};

/// Candidate order: onset, note, group, channel, then end. Notes equal in all
/// five keep their pairing order under std::stable_sort, so every standard
/// library yields the same sequence.
bool source_note_less(const SourceNote& left, const SourceNote& right) noexcept {
  if (left.content_ppq != right.content_ppq) return left.content_ppq < right.content_ppq;
  if (left.note != right.note) return left.note < right.note;
  if (left.group != right.group) return left.group < right.group;
  if (left.channel != right.channel) return left.channel < right.channel;
  return left.length_ppq < right.length_ppq;
}

sonare::midi::Ump ump_from_event(const arrangement::MidiClipEvent& event) noexcept {
  sonare::midi::Ump ump;
  ump.words[0] = event.data0;
  ump.words[1] = event.data1;
  ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
  ump.group = sonare::midi::ump_group_from_word0(event.data0);
  return ump;
}

AssistResult empty_with_reason(std::string reason) {
  AssistResult result;
  result.diagnostics.status = AssistStatus::kEmpty;
  result.diagnostics.reason = std::move(reason);
  return result;
}

/// A request this module will not answer. Kept apart from empty_with_reason so a
/// caller's typo cannot arrive looking like a musical outcome.
AssistResult rejected_with_reason(std::string reason) {
  AssistResult result;
  result.diagnostics.status = AssistStatus::kRejected;
  result.diagnostics.reason = std::move(reason);
  return result;
}

uint8_t scaled_velocity(uint8_t source, float scale) noexcept {
  const float scaled = static_cast<float>(source) * scale;
  if (scaled < 1.0f) return 1;
  if (scaled > 127.0f) return 127;
  return static_cast<uint8_t>(scaled + 0.5f);
}

SourceNote source_note_from_pair(const sonare::midi::detail::NotePair& pair) noexcept {
  return SourceNote{pair.start_ppq, pair.end_ppq - pair.start_ppq,
                    pair.note,      pair.note_on.data2_7bit(),
                    pair.group,     pair.channel};
}

template <typename OnPair>
bool pair_source_events(const arrangement::MidiClipEventList& events, OnPair on_pair) {
  return sonare::midi::detail::pair_notes(
      events,
      [](const arrangement::MidiClipEvent& event) noexcept {
        return sonare::midi::detail::DecodedNoteEvent{event.ppq, ump_from_event(event)};
      },
      std::move(on_pair),
      []() noexcept { return !sonare::midi::assist::detail::deadline_expired(); });
}

}  // namespace

AssistResult DiatonicHarmonizer::derive(const arrangement::ProjectView& view,
                                        const AssistRequest& request,
                                        const std::vector<VoiceModel>& cantus,
                                        const AssistQueryContext& queries) {
  sonare::midi::assist::detail::RequestDeadlineScope deadline_scope(request);

  GeneratorParams params;
  std::string params_error;
  if (!read_generator_params(request.params_json, &params, &params_error)) {
    return rejected_with_reason(params_error);
  }
  if (params.target_clip_id == 0) {
    return rejected_with_reason(
        "params_json must name target_clip_id: this module writes into an existing clip and "
        "does not create one");
  }
  const arrangement::ClipId source_clip_id =
      params.source_clip_id != 0 ? params.source_clip_id : params.target_clip_id;

  const arrangement::EditClip* target_clip = view.find_clip(params.target_clip_id);
  if (target_clip == nullptr) {
    return empty_with_reason("target clip " + std::to_string(params.target_clip_id) +
                             " does not exist");
  }
  const arrangement::EditClip* source_clip = view.find_clip(source_clip_id);
  if (source_clip == nullptr) {
    return empty_with_reason("source clip " + std::to_string(source_clip_id) + " does not exist");
  }
  const arrangement::MidiClipEventList* source_events = view.clip_events(source_clip_id);
  if (source_events == nullptr || source_events->empty()) {
    return empty_with_reason("source clip " + std::to_string(source_clip_id) +
                             " carries no MIDI events");
  }
  // An explicitly modelled voice outranks the params' defaults: a host that
  // built a VoiceModel has said more about the part than a fallback range has.
  VoiceModel voice;
  voice.low_note = params.low_note;
  voice.high_note = params.high_note;
  voice.role = "harmony";
  if (!cantus.empty()) {
    voice.low_note = cantus.front().low_note;
    voice.high_note = cantus.front().high_note;
    voice.track_id = cantus.front().track_id;
  }

  arrangement::MidiClipPatch patch;
  patch.clip_id = params.target_clip_id;
  json::Array decisions;
  uint32_t iterations = 0;
  bool iteration_truncated = false;
  bool time_truncated = false;

  std::vector<SourceNote> qualified;
  bool has_completed_note = false;
  const bool first_pass_completed =
      pair_source_events(*source_events, [&](const sonare::midi::detail::NotePair& pair) {
        has_completed_note = true;
        const SourceNote note = source_note_from_pair(pair);
        const double timeline_ppq = clip_content_to_timeline_ppq(*source_clip, note.content_ppq);
        if (request.scope.start_ppq.has_value() && timeline_ppq < *request.scope.start_ppq) return;
        if (request.scope.end_ppq.has_value() && timeline_ppq >= *request.scope.end_ppq) return;
        qualified.push_back(note);
      });
  time_truncated = !first_pass_completed || sonare::midi::assist::detail::deadline_expired();

  if (!has_completed_note && !time_truncated) {
    return empty_with_reason("source clip " + std::to_string(source_clip_id) +
                             " carries no completed notes");
  }

  // Preparation checks the deadline every 64 entries and around the sort.
  constexpr size_t kPreparationCheckCadence = 64;
  const auto preparation_deadline = [&]() noexcept {
    if (sonare::midi::assist::detail::deadline_expired()) {
      time_truncated = true;
      return true;
    }
    return false;
  };

  if (!time_truncated) {
    bool sorted = true;
    for (size_t i = 1; i < qualified.size(); ++i) {
      if ((i & (kPreparationCheckCadence - 1)) == 0 && preparation_deadline()) break;
      if (source_note_less(qualified[i], qualified[i - 1])) {
        sorted = false;
        break;
      }
    }
    if (!time_truncated) preparation_deadline();
    if (!time_truncated && !sorted && !preparation_deadline()) {
      std::stable_sort(qualified.begin(), qualified.end(), source_note_less);
      preparation_deadline();
    }
  }

  for (size_t candidate_index = 0; candidate_index < qualified.size(); ++candidate_index) {
    const SourceNote& note = qualified[candidate_index];
    const bool has_more_candidates = candidate_index + 1 < qualified.size();
    const auto stop_for_expired_deadline = [&]() noexcept {
      if (!sonare::midi::assist::detail::deadline_expired()) return false;
      if (has_more_candidates && request.budget.has_iteration_cap() &&
          iterations >= request.budget.max_iterations) {
        iteration_truncated = true;
      } else {
        time_truncated = true;
      }
      return true;
    };

    if (request.budget.has_iteration_cap() && iterations >= request.budget.max_iterations) {
      iteration_truncated = true;
      break;
    }
    if (time_truncated) break;
    if (stop_for_expired_deadline()) break;
    iterations = sonare::midi::assist::detail::saturating_add_u32(iterations, 1);

    const double source_timeline_ppq = clip_content_to_timeline_ppq(*source_clip, note.content_ppq);
    json::Object decision;
    decision["sourcePpq"] = json::Value(note.content_ppq);
    decision["sourceNote"] = json::Value(static_cast<int>(note.note));

    const std::vector<uint8_t> scale =
        queries.harmony != nullptr ? queries.harmony->scale_pitch_classes(view, source_timeline_ppq)
                                   : std::vector<uint8_t>{};
    if (stop_for_expired_deadline()) {
      break;
    }
    if (scale.empty()) {
      decision["accepted"] = json::Value(false);
      decision["reason"] =
          json::Value("no key is annotated here, so there are no scale steps to move through");
      decisions.push_back(json::Value(std::move(decision)));
      if (stop_for_expired_deadline()) break;
      continue;
    }

    const int derived = theory::transpose_scale_steps(scale, note.note, config_.interval_steps);
    if (derived < 0) {
      decision["accepted"] = json::Value(false);
      decision["reason"] = json::Value("moving " + std::to_string(config_.interval_steps) +
                                       " scale steps leaves the MIDI range");
      decisions.push_back(json::Value(std::move(decision)));
      if (stop_for_expired_deadline()) break;
      continue;
    }

    CandidateNote candidate;
    candidate.ppq = source_timeline_ppq;
    candidate.length_ppq = note.length_ppq;
    candidate.note = static_cast<uint8_t>(derived);
    candidate.velocity = scaled_velocity(note.velocity != 0 ? note.velocity : params.base_velocity,
                                         params.velocity_scale);

    std::string reason = "moved " + std::to_string(config_.interval_steps) + " scale steps";
    if (queries.judge != nullptr) {
      const PlacementVerdict verdict = queries.judge->judge(view, voice, candidate);
      reason += "; " + verdict.reason;
      if (!verdict.accepted) {
        decision["accepted"] = json::Value(false);
        decision["reason"] = json::Value(reason);
        decisions.push_back(json::Value(std::move(decision)));
        if (stop_for_expired_deadline()) break;
        continue;
      }
      candidate = verdict.adjusted;
    }
    if (stop_for_expired_deadline()) break;

    decision["accepted"] = json::Value(true);
    decision["note"] = json::Value(static_cast<int>(candidate.note));
    decision["velocity"] = json::Value(static_cast<int>(candidate.velocity));
    decision["reason"] = json::Value(reason);
    decisions.push_back(json::Value(std::move(decision)));

    const sonare::midi::Ump on = sonare::midi::make_midi1_note_on(
        note.group, note.channel, candidate.note, candidate.velocity);
    const sonare::midi::Ump off =
        sonare::midi::make_midi1_note_off(note.group, note.channel, candidate.note, 0);
    const double on_content_ppq = timeline_to_clip_content_ppq(*target_clip, candidate.ppq);
    const double off_content_ppq =
        timeline_to_clip_content_ppq(*target_clip, candidate.ppq + candidate.length_ppq);
    patch.add.push_back(arrangement::MidiClipEvent{on_content_ppq, on.words[0], on.words[1], 0});
    patch.add.push_back(arrangement::MidiClipEvent{off_content_ppq, off.words[0], off.words[1], 0});
    if (stop_for_expired_deadline()) break;
  }

  if (!time_truncated && !iteration_truncated && sonare::midi::assist::detail::deadline_expired()) {
    time_truncated = true;
  }

  AssistResult result;
  json::Object payload;
  payload["module"] = json::Value(std::string(kModuleId));
  payload["intervalSteps"] = json::Value(config_.interval_steps);
  payload["targetClipId"] = json::Value(static_cast<double>(params.target_clip_id));
  payload["sourceClipId"] = json::Value(static_cast<double>(source_clip_id));
  payload["decisions"] = json::Value(std::move(decisions));
  result.candidate_payload = json::dump(json::Value(std::move(payload)));
  result.candidate_payloads.push_back(result.candidate_payload);
  result.diagnostics.iterations_consumed = iterations;

  if (!patch.add.empty()) {
    result.commands.push_back(std::make_unique<arrangement::PatchMidiClip>(std::move(patch)));
  }
  if (iteration_truncated || time_truncated) {
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason = iteration_truncated ? "stopped after the request's iteration budget"
                                                    : "stopped after the request's time budget";
  } else if (result.commands.empty()) {
    result.diagnostics.status = AssistStatus::kEmpty;
    result.diagnostics.reason = "every derived note was refused; see the candidate payload";
  } else {
    result.diagnostics.status = AssistStatus::kOk;
  }
  return result;
}

}  // namespace sonare::midi::assist::modules
