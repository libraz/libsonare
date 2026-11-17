#pragma once

/// @file capability_features.h
/// @brief The key set of the capability descriptor's `features` object.
/// @details Each row is X(json key, predicate suffix, gate), where the gate is the
///          BUILD_<gate> option behind the key (FFMPEG names SONARE_WITH_FFMPEG).
///          A TU expanding the list defines one `capability_<suffix>_enabled()`
///          per row, so a new row fails to compile until every reader answers it.
///          tests/conformance/test_capability_feature_gates.py holds the rows
///          against the CMake option lists.

#define SONARE_CAPABILITY_FEATURES(X)                                      \
  X("mastering", mastering, MASTERING)                                     \
  X("mixing", mixing, MIXING)                                              \
  X("mixingAssistant", mixing_assistant, MIXING_ASSISTANT)                 \
  X("graph", graph, GRAPH)                                                 \
  X("fx", fx, FX)                                                          \
  X("ffmpeg", ffmpeg, FFMPEG)                                              \
  X("instrumentParamAutomation", instrument_param_automation, ARRANGEMENT) \
  X("arrangement", arrangement, ARRANGEMENT)                               \
  X("project", project, ARRANGEMENT)                                       \
  X("acousticSim", acoustic_sim, ACOUSTIC_SIM)                             \
  X("playback", playback, PLAYBACK)                                        \
  X("pitchEditor", pitch_editor, PITCH_EDITOR)                             \
  X("vocalEdit", vocal_edit, PITCH_EDITOR)                                 \
  X("voiceChanger", voice_changer, VOICE_CHANGER)                          \
  X("assist", assist, ASSIST)
