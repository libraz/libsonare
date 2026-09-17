#include "midi/assist/modules/chord_tone_generator.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "midi/assist/modules/generator_params.h"
#include "midi/assist/modules/music_theory.h"
#include "midi/ump.h"
#include "util/json.h"

namespace sonare::midi::assist::modules {

namespace {

namespace json = sonare::util::json;

constexpr int kMaxMidiNote = 127;
constexpr int kMaxVoices = 8;

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

/// The chord tones that fit the voice, lowest first, starting at or above
/// @p low_note. Ascends through the pitch-class set and wraps by octaves, so a
/// three-voice request on a triad gives root / third / fifth rather than three
/// copies of whichever tone happened to be lowest.
std::vector<int> voicing(const std::vector<uint8_t>& tones, int low_note, int high_note,
                         int voice_count) {
  std::vector<int> result;
  if (tones.empty() || voice_count <= 0) return result;
  std::vector<uint8_t> sorted(tones);
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  int note = low_note;
  while (note <= high_note && static_cast<int>(result.size()) < voice_count) {
    if (std::find(sorted.begin(), sorted.end(), static_cast<uint8_t>(note % 12)) != sorted.end()) {
      result.push_back(note);
    }
    ++note;
  }
  return result;
}

}  // namespace

ChordToneGenerator::ChordToneGenerator(const IHarmonyContext* harmony,
                                       const INotePlacementJudge* judge,
                                       ChordToneGeneratorConfig config) noexcept
    : harmony_(harmony != nullptr ? harmony : &own_harmony_),
      own_dissonance_(harmony_),
      own_judge_(harmony_, &own_dissonance_),
      judge_(judge != nullptr ? judge : &own_judge_),
      config_(config) {}

AssistResult ChordToneGenerator::generate(const arrangement::ProjectView& view,
                                          const AssistRequest& request) {
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
  if (config_.voice_count <= 0 || config_.voice_count > kMaxVoices) {
    return empty_with_reason("voice_count must be within 1.." + std::to_string(kMaxVoices));
  }

  const std::vector<arrangement::ChordSymbol>& chords = view.harmony().chords;
  if (chords.empty()) {
    return empty_with_reason(
        "the project annotates no chords, and this module does not guess harmony");
  }

  VoiceModel voice;
  voice.low_note = params.low_note;
  voice.high_note = params.high_note;
  voice.role = "comp";
  voice.max_polyphony = static_cast<uint8_t>(config_.voice_count);

  arrangement::MidiClipPatch patch;
  patch.clip_id = params.target_clip_id;
  json::Array decisions;
  uint32_t iterations = 0;
  bool truncated = false;

  for (const arrangement::ChordSymbol& chord : chords) {
    if (request.budget.has_iteration_cap() && iterations >= request.budget.max_iterations) {
      truncated = true;
      break;
    }
    ++iterations;
    if (chord.length_ppq() <= 0.0) continue;
    if (request.scope.start_ppq.has_value() && chord.start_ppq < *request.scope.start_ppq) continue;
    if (request.scope.end_ppq.has_value() && chord.start_ppq >= *request.scope.end_ppq) continue;

    json::Object decision;
    decision["startPpq"] = json::Value(chord.start_ppq);
    if (!chord.roman_numeral.empty()) decision["roman"] = json::Value(chord.roman_numeral);

    const std::vector<uint8_t> tones = theory::chord_pitch_classes(chord);
    if (tones.empty()) {
      decision["accepted"] = json::Value(false);
      decision["reason"] = json::Value("the chord symbol carries no usable root or quality");
      decisions.push_back(json::Value(std::move(decision)));
      continue;
    }

    json::Array voiced;
    for (const int note : voicing(tones, voice.low_note, voice.high_note, config_.voice_count)) {
      CandidateNote candidate;
      candidate.ppq = chord.start_ppq;
      candidate.length_ppq = chord.length_ppq();
      candidate.note = static_cast<uint8_t>(std::clamp(note, 0, kMaxMidiNote));
      candidate.velocity = params.base_velocity;

      json::Object placed;
      placed["note"] = json::Value(static_cast<int>(candidate.note));
      if (judge_ != nullptr) {
        const PlacementVerdict verdict = judge_->judge(view, voice, candidate);
        placed["accepted"] = json::Value(verdict.accepted);
        placed["reason"] = json::Value(verdict.reason);
        if (!verdict.accepted) {
          voiced.push_back(json::Value(std::move(placed)));
          continue;
        }
        candidate = verdict.adjusted;
        placed["note"] = json::Value(static_cast<int>(candidate.note));
      } else {
        placed["accepted"] = json::Value(true);
        placed["reason"] = json::Value("no placement judge installed");
      }
      voiced.push_back(json::Value(std::move(placed)));

      const sonare::midi::Ump on =
          sonare::midi::make_midi1_note_on(0, 0, candidate.note, candidate.velocity);
      const sonare::midi::Ump off = sonare::midi::make_midi1_note_off(0, 0, candidate.note, 0);
      patch.add.push_back(arrangement::MidiClipEvent{candidate.ppq, on.words[0], on.words[1], 0});
      patch.add.push_back(arrangement::MidiClipEvent{candidate.ppq + candidate.length_ppq,
                                                     off.words[0], off.words[1], 0});
    }
    decision["accepted"] = json::Value(!voiced.empty());
    decision["voices"] = json::Value(std::move(voiced));
    decisions.push_back(json::Value(std::move(decision)));
  }

  AssistResult result;
  json::Object payload;
  payload["module"] = json::Value(std::string(kModuleId));
  payload["voiceCount"] = json::Value(config_.voice_count);
  payload["targetClipId"] = json::Value(static_cast<double>(params.target_clip_id));
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
    result.diagnostics.reason = "no chord produced a placeable note; see the candidate payload";
  } else {
    result.diagnostics.status = AssistStatus::kOk;
  }
  return result;
}

}  // namespace sonare::midi::assist::modules
