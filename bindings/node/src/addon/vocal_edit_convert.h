#ifndef SONARE_NODE_VOCAL_EDIT_CONVERT_H_
#define SONARE_NODE_VOCAL_EDIT_CONVERT_H_

// Readers and result converters behind the vocal-edit handles in vocal_edit.cpp.

#include <napi.h>
#include <sonare/sonare_c_vocal_edit.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sonare_node::vocal_edit_detail {

struct EditStorage {
  std::vector<SonareVocalPitchPoint> points;
  std::vector<float> envelope;
};

struct ParsedOperation {
  SonareVocalOperation operation{};
  EditStorage edit_storage;
  std::vector<uint32_t> note_ids;
};

struct NotesGuard {
  SonareVocalNotesResult value{};
  NotesGuard() { sonare_vocal_notes_result_init(&value); }
  ~NotesGuard() { sonare_free_vocal_notes_result(&value); }
};

struct AnalysisGuard {
  SonareVocalAnalysisResult value{};
  AnalysisGuard() { sonare_vocal_analysis_result_init(&value); }
  ~AnalysisGuard() { sonare_free_vocal_analysis_result(&value); }
};

struct EditGuard {
  SonareVocalEditResult value{};
  EditGuard() { sonare_vocal_edit_result_init(&value); }
  ~EditGuard() { sonare_free_vocal_edit_result(&value); }
};

struct PitchGuard {
  SonareVocalPitchResult value{};
  PitchGuard() { sonare_vocal_pitch_result_init(&value); }
  ~PitchGuard() { sonare_free_vocal_pitch_result(&value); }
};

struct RenderGuard {
  SonareVocalRenderResult value{};
  RenderGuard() { sonare_vocal_render_result_init(&value); }
  ~RenderGuard() { sonare_free_vocal_render_result(&value); }
};

struct StateGuard {
  SonareVocalStateBytes value{};
  StateGuard() { sonare_vocal_state_bytes_init(&value); }
  ~StateGuard() { sonare_free_vocal_state_bytes(&value); }
};

bool ReadRange(Napi::Env env, const Napi::Value& value, SonareVocalRange* out);
bool ReadNoteId(Napi::Env env, const Napi::Value& value, const char* field, uint32_t* out);
bool ReadOperations(Napi::Env env, const Napi::Value& value, std::vector<ParsedOperation>* parsed);
std::string Uint64String(uint64_t value);
Napi::Object TokenToObject(Napi::Env env, const SonareVocalStateToken& token);
Napi::Object NotesResultToObject(Napi::Env env, const SonareVocalNotesResult& result);
Napi::Object AnalysisResultToObject(Napi::Env env, const SonareVocalAnalysisResult& result);
Napi::Object CapabilitiesToObject(Napi::Env env, const SonareVocalCapabilities& capabilities);
Napi::Object EditResultToObject(Napi::Env env, const SonareVocalEditResult& result);
Napi::Object PitchResultToObject(Napi::Env env, const SonareVocalPitchResult& result);
Napi::Object RenderResultToObject(Napi::Env env, const SonareVocalRenderResult& result);
bool CheckVocalError(Napi::Env env, SonareError error);
Napi::Promise RejectedVocalPromise(Napi::Env env, SonareError error);
bool ReadSampleRate(Napi::Env env, const Napi::Value& value, int* out);
bool ReadSessionLimits(Napi::Env env, const Napi::Object& object, uint64_t* max_history_bytes,
                       uint64_t* max_cache_bytes, uint32_t* max_undo_depth,
                       uint32_t* max_render_jobs);
bool ReadCreateOptions(Napi::Env env, const Napi::Value& value, SonareVocalCreateOptions* options,
                       SonareVocalAnalysis* analysis_storage, std::vector<float>* f0_storage,
                       std::vector<uint8_t>* voiced_storage, std::string* algorithm_storage);

}  // namespace sonare_node::vocal_edit_detail

#endif  // SONARE_NODE_VOCAL_EDIT_CONVERT_H_
