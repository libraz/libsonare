#pragma once

/// @file tunable.h
/// @brief Development-only runtime override for voice calibration constants.
/// @details Calibration constants are file-scope `constexpr float`s, so fitting
/// one costs a rebuild per candidate. `SONARE_TUNABLE(name, value)` expands to
/// exactly that `constexpr float` in a normal build — no storage, no lookup, no
/// runtime cost — and only under `-DBUILD_TUNING=ON` becomes a `const float`
/// seeded from an override table, which lets `tools/voicematch/autofit.py` sweep
/// a knob through an environment variable instead.
///
/// A knob is `<scope>.<name>`, the scope being the declaring file's stem —
/// `brass_voice.kBreathBase`. It comes from `__FILE__` rather than being written
/// per file because one calibration name is used by several voices
/// (`kBreathBase` by four), each in its own anonymous namespace, while the
/// override table has one flat key space.
///
/// The override table is read once, on first use, from `SONARE_TUNING_OVERRIDES`:
///
///     SONARE_TUNING_OVERRIDES='piano_voice.kHammerWidthHarmonics=2.7,brass_voice.kLipCouple=4.9'
///     SONARE_TUNING_OVERRIDES='@/path/to/knobs.txt'   # one `key=value` per line
///
/// A key absent from the table keeps its compiled-in default, and a key in the
/// table that nothing ever asks for is silently ignored — the fitter validates
/// its knob names against the source instead, since a typo would otherwise read
/// as "this knob has no effect".
///
/// Never guard behaviour on `SONARE_TUNING`: the two configurations must
/// compute identical audio for identical values, or a fit transfers nothing
/// back to the shipped build.

namespace sonare {
namespace tuning {

/// Resolve a tunable's value: the override table's entry for
/// `<stem of file>.<name>`, else `default_value`. Only ever called from
/// `SONARE_TUNABLE` in a `BUILD_TUNING=ON` build.
float tunable_value(const char* file, const char* name, float default_value);

/// Resolve an explicitly-keyed override, else `default_value`. Used by override
/// layers whose keys are not file-scoped (the per-program patch fields).
float tunable_keyed(const char* key, float default_value);

/// Record that GM melodic program @p program is voiced by the patch addressed
/// as @p key, for the `SONARE_TUNING_DUMP` catalogue. Which patch a program
/// resolves to is a switch statement's business and not something an external
/// fitter should have to re-derive by parsing it. No-op unless a dump was
/// requested.
///
/// @p bank is the GS variation number. A program's variations are separate
/// patches with separate knobs - program 19 is a six-rank principal chorus at
/// bank 0, three flute ranks at bank 8 and a full organ with reeds at bank 16 -
/// so a catalogue keyed by program alone can only ever offer the capital tone's
/// knobs, whatever bank the render selected.
void note_program_key(int program, int bank, const char* key);

/// Record the admissible range of the patch field addressed as @p path (the
/// key without its patch prefix, e.g. `bowed_string.bow_force`), for the
/// `SONARE_TUNING_DUMP` catalogue.
///
/// A fitter needs a search range per knob, and the library already owns one:
/// `clamp_synth_patch` clamps nearly every patch field to the interval the
/// engine accepts. Reporting it is what lets the fitter search the real space
/// instead of guessing a range from the default's magnitude — a guess that is
/// both too wide (wasting the budget outside the space, where the clamp makes
/// the loss flat) and too narrow (a knob whose best value is 5x its default is
/// unreachable).
///
/// Thirteen of the fields the override layer walks have no bound to report,
/// because `clamp_synth_patch` does not bound them: the eight Karplus-Strong
/// extensions (`ks.body_coupling`, `pluck_style`, `nail`, `pickup_pos`,
/// `dispersion`, `tension_mod`, `octave_mix`, `keyoff_noise`),
/// `bowed_string.stribeck`, `bowed_string.sympathetic`,
/// `bowed_string.polarization` and `pipe_organ.keytrack` — each clamped instead
/// by its own voice at `start()`, where it is read — and
/// `percussion.strike_theta`, an angle that reaches a cosine and so only has to
/// be finite. The audio is safe either way; the consequence is confined to the
/// catalogue, where such a field is reported as unbounded and a fitter's
/// auto-range falls back to its heuristic. No-op unless a dump was requested.
void note_bound(const char* path, float lo, float hi);

}  // namespace tuning
}  // namespace sonare

#if defined(SONARE_TUNING) && SONARE_TUNING
/// Declare a runtime-overridable calibration constant (tuning build).
#define SONARE_TUNABLE(name, value) \
  const float name = ::sonare::tuning::tunable_value(__FILE__, #name, (value))
#else
/// Declare a calibration constant (normal build: a plain `constexpr float`).
#define SONARE_TUNABLE(name, value) constexpr float name = (value)
#endif

#if defined(SONARE_TUNING) && SONARE_TUNING
/// Qualify a builder whose result is a compile-time constant only in a shipped
/// build. A tunable resolves from the environment while the process runs, so a
/// table built from one cannot be constant-initialised in a tuning build, and a
/// `constexpr` that can never fold is ill-formed. Values are unaffected either
/// way: the same builder runs over the same inputs, only later.
///
/// `inline` rather than nothing, because `constexpr` on a function carries it:
/// dropping the qualifier outright gives a builder defined in a header external
/// linkage, and the link fails on a duplicate symbol as soon as a second
/// translation unit includes it. Nothing announces which build that will be --
/// a shipped build links either way, and the tuning build is the one nobody
/// runs in CI.
#define SONARE_TUNED_CONSTEXPR inline
#else
/// Qualify a builder whose result is a compile-time constant (normal build).
#define SONARE_TUNED_CONSTEXPR constexpr
#endif
