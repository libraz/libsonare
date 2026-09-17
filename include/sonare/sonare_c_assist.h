#pragma once

/// @file sonare_c_assist.h
/// @brief Composition assist: the built-in generative modules, previewed or
///        applied over a project. Included via @ref sonare_c.h.
///
/// The library ships a set of RULE-BASED modules -- no trained model, no
/// statistical classifier, no learned parameter, no clock and no ambient
/// randomness. Every note a module proposes carries a written reason, and the
/// reasons come back with the result rather than being summarised away.
///
/// Two entry points, in the same shape as the tempo pair in
/// @ref sonare_c_project_annotate.h: @ref sonare_project_assist_preview_json
/// runs the modules and reports what they would do WITHOUT touching the project,
/// and @ref sonare_project_assist_apply_json runs them and commits the result
/// through the project's own undo history. Nothing collapses the two into one
/// call that applies without having been previewed.
///
/// **The modules write into a clip that already exists.** They never add a
/// track, a source or a clip: where a generated part should live is a structural
/// decision, and the request has to name the clip. A request naming none is
/// REFUSED (see @c "rejected" below) rather than answered with a quiet nothing.
///
/// **This seam is `@libraz/libcantus`'s plug-in point, and note-level generation
/// is libcantus's layer rather than this one's.** The modules shipped behind it
/// are the seam's reference implementation -- enough to test a seam with, not a
/// note-generation library -- and they are reachable from the C ABI and from C++
/// alone. No Python, Node, WASM or CLI binding exposes these entry points, and
/// `assist 0/3` in `tools/parity/surface-coverage.md` is that decision rather
/// than a coverage gap.
///
/// What a host integrates through is the RESULT shape, which every binding
/// already reaches: @c patches comes back as @ref SonareMidiEventPod triples
/// that merge through @ref sonare_project_set_midi_events.
///
/// **Known gap, so nobody reads more into "plug-in point" than it currently
/// holds: there is no way to REGISTER a host module from outside C++.** The
/// module interfaces in `src/midi/assist/` are same-build C++ abstracts, so this
/// ABI runs the built-ins and nothing else; @c unrenderedCommands below is
/// written for a host-installed module that no surface can yet install. Closing
/// that is what would make this seam a plug-in point in fact and not only in
/// intent.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_project_core.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Returns the built-in module ids, separated by '\n'.
/// @details The pointer is thread-local storage rebuilt on every call: copy it
///          before the next call on the same thread. NULL when the library was
///          built without the assist seam.
const char* sonare_assist_module_ids(void);

/// @brief Runs the built-in assist modules over @p project and reports what they
///        would do, WITHOUT modifying the project.
///
/// @param request_json A JSON object. All fields are optional except the target
///        clip, which lives in @c params:
///   - @c modules: array of module ids to run. Absent runs every built-in one.
///     An unknown id is an error, not a silent skip.
///   - @c seed: unsigned 32-bit determinism seed. The built-in modules consume
///     none of it today, so their output is a function of the project and the
///     request alone; it is carried because the seam's contract requires it.
///   - @c scope: @c { "trackIds": [...], "startPpq": n, "endPpq": n }. Absent
///     bounds mean the whole timeline.
///   - @c budget: @c { "maxTimeMs": n, "maxIterations": n }. 0 on either axis
///     means no cap on that axis.
///   - @c params: the module parameter object. @c target_clip_id is required;
///     @c source_clip_id, @c low_note, @c high_note, @c base_velocity and
///     @c velocity_scale are optional.
///
/// @param out_json Receives the result document:
///   - @c status: @c "ok", @c "empty", @c "budgetTruncated", @c "discarded" or
///     @c "rejected".
///
///     @c "rejected" is the only one that is ALSO an error return: a module that
///     refused the request -- a parameter outside its domain, a @c params blob
///     it could not read -- makes the call answer
///     @c SONARE_ERROR_INVALID_PARAMETER with the field named in
///     @ref sonare_last_error_message, and commits nothing. It is kept apart
///     from @c "empty" because a caller's typo and a module that genuinely had
///     nothing to add would otherwise arrive in the same shape, leaving
///     string-matching the reason as the only way to find one's own mistake.
///
///     The document is still written on this path, but do not read more into
///     that than it holds: measured, a rejected run comes back with empty
///     @c patches, empty @c payloads and @c iterationsConsumed 0, because the
///     built-in modules refuse a request BEFORE doing any work, and all of them
///     refuse the same request identically. A binding that drops the document
///     and raises on the error return loses nothing by it.
///   - @c reason: what the contributing modules said for themselves, newline
///     joined and never repeated -- two modules reporting the SAME fault say it
///     once, which is what a request-level mistake every module refuses alike
///     would otherwise turn into N copies of. Or the driver's own account when
///     IT ended the run (a budget it enforced between slots, a module it
///     discarded). Present on an @c "ok" run too, and deliberately so: a run
///     where one module contributed and another declined is a success that
///     still owes an account of the module that did nothing. Read it as "what
///     happened", not as "what went wrong".
///   - @c iterationsConsumed, @c slotsDiscarded: the driver's own counters.
///   - @c patches: one entry per proposed clip edit, @c { "clipId": n, "add":
///     [ { "ppq": n, "data0": n, "data1": n } ] }. The event triples are exactly
///     @ref SonareMidiEventPod, so a caller applies a suggestion by merging them
///     into the clip's event list and calling
///     @ref sonare_project_set_midi_events.
///   - @c unrenderedCommands: type names of any proposed command this surface
///     cannot describe as a patch. Always empty for the built-in modules, and
///     present so a host-installed module's output is reported as unreadable
///     rather than silently dropped.
///   - @c payloads: each module's own decision record, one JSON string per
///     contributing module, naming every note it accepted or refused and why.
///   Release with @ref sonare_free_string.
///
/// @note SONARE_ERROR_NOT_SUPPORTED when the library was built without the
///       assist seam or without the arrangement subsystem.
SonareError sonare_project_assist_preview_json(const SonareProject* project,
                                               const char* request_json, char** out_json);

/// @brief Runs the built-in assist modules over @p project and COMMITS what they
///        propose, through the project's own undo history.
/// @details Same request and same result document as
///          @ref sonare_project_assist_preview_json, which is the call to make
///          first: this one is undoable but it is not a dry run. A run whose
///          status is not @c "ok" commits nothing.
/// @note SONARE_ERROR_NOT_SUPPORTED when the library was built without the
///       assist seam or without the arrangement subsystem.
SonareError sonare_project_assist_apply_json(SonareProject* project, const char* request_json,
                                             char** out_json);

#ifdef __cplusplus
}  // extern "C"
#endif
