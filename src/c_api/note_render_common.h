#pragma once

/// @file note_render_common.h
/// @brief Shared C-ABI NoteRenderConfig conversion for note render entry points.

#include <sonare/sonare_c_effects.h>

#include "editing/note_model/note_renderer.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// Resolves the versioned C config onto note-render defaults.
///
/// A null config deliberately leaves @p out untouched, matching the public
/// entry points' default-constructed core config. Zero-valued fields likewise
/// retain their core defaults; validation happens before either field is copied.
inline SonareError resolve_note_render_config(const SonareNoteRenderConfig* config,
                                              sonare::editing::note_model::NoteRenderConfig& out) {
  if (config == nullptr) return SONARE_OK;
  const SonareError error = run_validation([&] {
    sonare::util::check::integer_in_range(config->struct_version, 0, 1, "config.struct_version");
    sonare::util::check::at_least(config->fade_ms, 0.0, "config.fade_ms");
    sonare::util::check::at_least(config->vibrato_cutoff_hz, 0.0, "config.vibrato_cutoff_hz");
  });
  if (error != SONARE_OK) return error;
  if (config->fade_ms > 0.0f) out.fade_ms = config->fade_ms;
  if (config->vibrato_cutoff_hz > 0.0f) {
    out.decomposition.vibrato_cutoff_hz = config->vibrato_cutoff_hz;
  }
  return SONARE_OK;
}

}  // namespace sonare_c_detail
