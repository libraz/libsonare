#ifndef SONARE_NODE_SONARE_WRAP_TRANSCRIBE_H_
#define SONARE_NODE_SONARE_WRAP_TRANSCRIBE_H_

#include <napi.h>
#include <sonare/sonare_c.h>

namespace sonare_node {

/// @brief `transcribe(request)` — audio to MIDI events on a constant-tempo grid.
/// @details Takes one request object carrying `samples`, `sampleRate`, an
///          optional `tempoBpm`, and the transcription config keys; answers
///          `{ events, noteCount, tempoBpm }`.
Napi::Value Transcribe(const Napi::CallbackInfo& info);

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_TRANSCRIBE_H_
