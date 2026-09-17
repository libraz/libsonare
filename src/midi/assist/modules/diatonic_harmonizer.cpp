#include "midi/assist/modules/diatonic_harmonizer.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "arrangement/edit_command.h"
#include "midi/assist/modules/music_theory.h"
#include "midi/ump.h"
#include "util/json.h"

namespace sonare::midi::assist::modules {

namespace {

namespace json = sonare::util::json;

/// One sounding note recovered from a clip's event list.
struct SourceNote {
  double ppq = 0.0;
  double length_ppq = 0.0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  uint8_t group = 0;
  uint8_t channel = 0;
};

sonare::midi::Ump ump_from_event(const arrangement::MidiClipEvent& event) noexcept {
  sonare::midi::Ump ump;
  ump.words[0] = event.data0;
  ump.words[1] = event.data1;
  ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
  ump.group = sonare::midi::ump_group_from_word0(event.data0);
  return ump;
}

/// Pairs note-ons with their note-offs on the same (group, channel, note).
/// A note-on never closed is dropped: a note with no end has no length, and
/// guessing one would put a note in the output the source never had.
std::vector<SourceNote> read_notes(const arrangement::MidiClipEventList& events) {
  struct OpenKey {
    uint8_t group;
    uint8_t channel;
    uint8_t note;
    bool operator<(const OpenKey& o) const noexcept {
      if (group != o.group) return group < o.group;
      if (channel != o.channel) return channel < o.channel;
      return note < o.note;
    }
  };
  std::map<OpenKey, SourceNote> open;
  std::vector<SourceNote> notes;
  for (const arrangement::MidiClipEvent& event : events) {
    const sonare::midi::Ump ump = ump_from_event(event);
    const OpenKey key{ump.group, ump.channel(), ump.note_number()};
    if (ump.is_note_off()) {
      const auto it = open.find(key);
      if (it == open.end()) continue;
      SourceNote note = it->second;
      open.erase(it);
      if (event.ppq <= note.ppq) continue;
      note.length_ppq = event.ppq - note.ppq;
      notes.push_back(note);
    } else if (ump.is_note_on()) {
      open[key] = SourceNote{event.ppq, 0.0, ump.note_number(), ump.data2_7bit(), ump.group,
                             ump.channel()};
    }
  }
  std::sort(notes.begin(), notes.end(), [](const SourceNote& a, const SourceNote& b) noexcept {
    if (a.ppq != b.ppq) return a.ppq < b.ppq;
    return a.note < b.note;
  });
  return notes;
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

}  // namespace

AssistResult DiatonicHarmonizer::derive(const arrangement::ProjectView& view,
                                        const AssistRequest& request,
                                        const std::vector<VoiceModel>& cantus,
                                        const AssistQueryContext& queries) {
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

  const arrangement::MidiClipEventList* source_events = view.clip_events(source_clip_id);
  if (source_events == nullptr || source_events->empty()) {
    return empty_with_reason("source clip " + std::to_string(source_clip_id) +
                             " carries no MIDI events");
  }
  const std::vector<SourceNote> notes = read_notes(*source_events);
  if (notes.empty()) {
    return empty_with_reason("source clip " + std::to_string(source_clip_id) +
                             " carries no completed notes");
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
  bool truncated = false;

  for (const SourceNote& note : notes) {
    if (request.budget.has_iteration_cap() && iterations >= request.budget.max_iterations) {
      truncated = true;
      break;
    }
    ++iterations;

    if (request.scope.start_ppq.has_value() && note.ppq < *request.scope.start_ppq) continue;
    if (request.scope.end_ppq.has_value() && note.ppq >= *request.scope.end_ppq) continue;

    json::Object decision;
    decision["sourcePpq"] = json::Value(note.ppq);
    decision["sourceNote"] = json::Value(static_cast<int>(note.note));

    const std::vector<uint8_t> scale =
        queries.harmony != nullptr ? queries.harmony->scale_pitch_classes(view, note.ppq)
                                   : std::vector<uint8_t>{};
    if (scale.empty()) {
      decision["accepted"] = json::Value(false);
      decision["reason"] = json::Value(
          "no key is annotated here, so there are no scale steps to move through");
      decisions.push_back(json::Value(std::move(decision)));
      continue;
    }

    const int derived = theory::transpose_scale_steps(scale, note.note, config_.interval_steps);
    if (derived < 0) {
      decision["accepted"] = json::Value(false);
      decision["reason"] = json::Value("moving " + std::to_string(config_.interval_steps) +
                                       " scale steps leaves the MIDI range");
      decisions.push_back(json::Value(std::move(decision)));
      continue;
    }

    CandidateNote candidate;
    candidate.ppq = note.ppq;
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
        continue;
      }
      candidate = verdict.adjusted;
    }

    decision["accepted"] = json::Value(true);
    decision["note"] = json::Value(static_cast<int>(candidate.note));
    decision["velocity"] = json::Value(static_cast<int>(candidate.velocity));
    decision["reason"] = json::Value(reason);
    decisions.push_back(json::Value(std::move(decision)));

    const sonare::midi::Ump on = sonare::midi::make_midi1_note_on(note.group, note.channel,
                                                                  candidate.note,
                                                                  candidate.velocity);
    const sonare::midi::Ump off =
        sonare::midi::make_midi1_note_off(note.group, note.channel, candidate.note, 0);
    patch.add.push_back(arrangement::MidiClipEvent{candidate.ppq, on.words[0], on.words[1], 0});
    patch.add.push_back(arrangement::MidiClipEvent{candidate.ppq + candidate.length_ppq,
                                                   off.words[0], off.words[1], 0});
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
  if (truncated) {
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason = "stopped after the request's iteration budget";
  } else if (result.commands.empty()) {
    result.diagnostics.status = AssistStatus::kEmpty;
    result.diagnostics.reason = "every derived note was refused; see the candidate payload";
  } else {
    result.diagnostics.status = AssistStatus::kOk;
  }
  return result;
}

}  // namespace sonare::midi::assist::modules
