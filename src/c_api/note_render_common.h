#pragma once

/// @file note_render_common.h
/// @brief Shared C-ABI NoteRenderConfig conversion for note render entry points.

#include <sonare/sonare_c_effects.h>

#include "editing/note_model/note_renderer.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// Checks the C config and copies it onto note-render defaults.
///
/// A null config deliberately leaves @p out untouched, matching the public
/// entry points' default-constructed core config.
inline SonareError resolve_note_render_config(const SonareNoteRenderConfig* config,
                                              sonare::editing::note_model::NoteRenderConfig& out) {
  if (config == nullptr) return SONARE_OK;
  const SonareError error = run_validation([&] {
    require_struct_size(config->struct_size, sizeof(SonareNoteRenderConfig), "config",
                        "sonare_note_render_config_init");
    sonare::util::check::greater_than(config->fade_ms, 0.0, "config.fade_ms");
    sonare::util::check::greater_than(config->vibrato_cutoff_hz, 0.0, "config.vibrato_cutoff_hz");
  });
  if (error != SONARE_OK) return error;
  out.fade_ms = config->fade_ms;
  out.decomposition.vibrato_cutoff_hz = config->vibrato_cutoff_hz;
  return SONARE_OK;
}

}  // namespace sonare_c_detail
