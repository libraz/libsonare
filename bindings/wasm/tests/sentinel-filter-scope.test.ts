/**
 * A field whose documented sentinel is `0` is tested with `== 0`, never `> 0`.
 *
 * `> 0` answers a NEGATIVE or non-finite request with the library default rather
 * than with the refusal that value had earned, and the caller gets SONARE_OK and
 * a result computed from a value it never asked for. {@link REMOVED_FILTERS} is
 * every field the filter has been taken off across every pass, not the most
 * recent one, and this file is what keeps them from coming back.
 *
 * WHAT A GREEN RUN DOES AND DOES NOT MEAN. Green says every `> 0` filter in the
 * scanned trees is answered and no removed one has returned. It does NOT say the
 * refusals still dominate their sites: {@link refusalMentioning} is a presence
 * check over the file, not a proof the rejection runs first on every path, and
 * the scan is blind to three spellings and to Python entirely, for the reasons
 * `_sentinel_filter_sources.ts` records.
 *
 * A REASON IS WRITTEN ABOUT THE FIELD, NEVER ABOUT THE COMPARISON. A site is
 * keyed `file:field`, so a reason arguing from the surrounding code ("the line
 * above rejects it") keeps applying verbatim after that line is deleted. Where
 * the reason really is "this field is refused", {@link REFUSED_OPTION_FIELDS}
 * drives a check that the refusal is still in the file, so deleting it turns
 * this red rather than leaving a sentence that quietly stopped being true.
 *
 * Every assertion below names the production edit that breaks it: a ratchet
 * nobody has aimed at a real edit is a ratchet nobody knows the range of.
 */

import { describe, expect, it } from 'vitest';
import {
  comparisonsAgainstZero,
  evaluateSentinelFilterScope,
  filterSites,
  refusalMentioning,
  type SentinelSource,
  sentinelSources,
  subjectsOf,
} from './_sentinel_filter_sources';

/**
 * EVERY field this defect has been removed from, each with what its zero selects
 * and what a negative did instead of being refused.
 *
 * Deliberately not scoped to one round of removals. A field fixed in an earlier
 * pass and left out of here is protected by nothing: its filter coming back
 * reads to the scan as a newly appeared one, and the reviewer who then writes it
 * a reason has no way to know the field had already been decided. Every entry
 * below asserts an absence that already holds, verified against the tree rather
 * than taken from whatever recorded the fix.
 *
 * `fields` lists every identifier the comparison could come back under -- the
 * local the value is read into as well as the struct field it feeds -- because
 * a restored filter does not have to be restored under the same name.
 *
 * THIS LIST IS A RATCHET: an entry leaves only when the field itself goes away.
 */
const REMOVED_FILTERS: ReadonlyArray<{
  file: string;
  fields: readonly string[];
  reason: string;
}> = [
  {
    file: 'src/c_api/sonare_c_engine_capture.cpp',
    fields: ['num_ports'],
    reason:
      "A graph node's port count, where 0 asks for the spec's channel width. A negative count used to reach that same derivation, so a request Graph::add_node would have refused outright came back as a node silently built at the spec's width.",
  },
  {
    file: 'src/wasm/bindings/realtime/processing.cpp',
    fields: ['requested_ports', 'num_ports', 'numPorts'],
    reason:
      'The same port count on the WASM surface, which mirrors the C ABI deliberately. Fixing either alone would turn a uniform defect into a cross-surface divergence, so the two entries move together.',
  },
  {
    file: 'src/c_api/sonare_c_engine_clips.cpp',
    fields: ['length_samples'],
    reason:
      "A clip's length in samples, where 0 asks for the source length past clip_offset_samples. A negative used to take that derivation too, and because the derived value is positive the rejection below it never fired -- so a clip asking for -1 samples played its whole source. The WASM clip path already read this field with `== 0`, so the fix is the C ABI catching up rather than a third behaviour.",
  },
  {
    file: 'src/wasm/bindings/realtime/engine.cpp',
    fields: ['requested', 'command_capacity', 'telemetry_capacity'],
    reason:
      'The engine command and telemetry queue depths, whose 0 selects the internal minimum. Reachable only from this surface: the C ABI declares both as size_t, so only the WASM facade can express the negative that used to read as the sentinel.',
  },
  {
    file: 'src/wasm/bindings/analysis/quick.cpp',
    fields: ['seed_in', 'seed'],
    reason:
      'The deterministic late-tail seed, whose 0 keeps the library default. The asymmetry was the sharp part: a seed past the uint32 range threw while a negative one was substituted, inside the function whose own comment says a silent default is what made the earlier gap invisible.',
  },
  {
    file: 'src/wasm/bindings/mastering/chain.cpp',
    fields: ['true_peak_oversample', 'release_ms'],
    reason:
      'The loudness maximizer\'s oversample factor and release time, both taking 0 as "use the library default". Every other value belongs to the shared loudness validator, which is what a `> 0` filter denied them: a negative factor or release never reached the rejection it had earned.',
  },
  {
    file: 'src/wasm/bindings/metering/metering.cpp',
    fields: ['db_ref', 'db_amin', 'dbRef', 'dbAmin'],
    reason:
      'The spectrum dB reference and floor, whose 0 keeps the core default. Only these two fields of this file are asserted absent: nFft and octaveFraction still filter on `> 0` here and are answered in REFUSED_OPTION_FIELDS, so a claim over the whole file would be false.',
  },
  {
    file: 'src/wasm/bindings/realtime/midi.cpp',
    fields: ['gain', 'polyphony'],
    reason:
      'The SoundFont player\'s gain and voice count, both documented as "0 or omit => default". The player substitutes its own value for a non-positive gain in silence, so a filter here meant a caller could ask for a gain it never got and read SONARE_OK.',
  },
  {
    file: 'src/c_api/features_spectrogram_mel.cpp',
    fields: ['fmin', 'fmax'],
    reason:
      'The Mel band edges, where 0 keeps the librosa defaults (fmin 0, fmax sr/2). The Mel core swaps its own default in for a negative or non-finite bound and reports nothing, so the refusal has to be at this boundary or nowhere.',
  },
  {
    file: 'src/c_api/sonare_c_editing.cpp',
    fields: ['reference_midi'],
    reason:
      "The scale quantizer's reference pitch, whose 0 keeps the core default. Only this field is asserted absent: n_fft, octaveFraction, db_ref and db_amin still filter on `> 0` in this file and are answered in REFUSED_OPTION_FIELDS.",
  },
  {
    file: 'bindings/node/src/addon/effects/mastering.cpp',
    fields: ['oversample', 'true_peak_oversample', 'release_ms'],
    reason:
      'The addon door onto the same maximizer fields as the WASM chain entry. This call reaches the core directly rather than through the C ABI, so the sentinel semantics have to be re-stated here or the two surfaces answer the same request differently.',
  },
  {
    file: 'src/c_api/sonare_c_engine_midi.cpp',
    fields: ['gain', 'polyphony'],
    reason:
      'The C ABI door onto the SoundFont player config, same two fields and same contract as the WASM one.',
  },
  {
    file: 'src/c_api/project_bounce.cpp',
    fields: ['gain', 'attack_ms', 'decay_ms', 'sustain', 'release_ms', 'polyphony'],
    reason:
      'The built-in synth patch fields and the SF2 player config, every one documented as "0 => default". clamp_synth_config reads a non-positive or non-finite field as the default and reports nothing, so a request that got that far came back as a successful call at a level the caller never chose. Only these fields are asserted absent: block_size, num_channels and sample_rate filter on `> 0` in this file under a documented `<= 0` contract and are answered in NOT_SENTINEL_FILTERS.',
  },
];

/**
 * Caller-supplied optional fields still filtered with `> 0`, where a negative or
 * non-finite value is refused before it can reach the substitution.
 *
 * Kept apart from {@link NOT_SENTINEL_FILTERS} because the two answer different
 * questions. That map says "this subject is not a caller's optional at all";
 * this one says "it is, and the refusal it needs exists" -- a claim that can
 * stop being true, and so is checked rather than recorded.
 */
const REFUSED_OPTION_FIELDS: ReadonlyArray<{
  file: string;
  field: string;
  reason: string;
  /**
   * The spelling the refusal uses, when it differs from the spelling the filter
   * uses. Needed only where the two sit in different functions; an entry without
   * it is checked against the filter's own subject, which is the stricter form.
   */
  refusedAs?: string;
}> = [
  {
    file: 'src/c_api/features_spectral_pitch.cpp',
    field: 'segmentation_threshold_cents',
    reason: 'Note-segmenter threshold in cents; 0 keeps the core default, negative is refused.',
  },
  {
    file: 'src/c_api/features_spectral_pitch.cpp',
    field: 'min_note_ms',
    reason: 'Minimum note length; 0 keeps the core default, negative is refused.',
  },
  {
    file: 'src/c_api/features_spectral_pitch.cpp',
    field: 'reference_hz',
    reason: 'Tuning reference; 0 keeps the core default, negative is refused.',
  },
  {
    file: 'src/c_api/features_spectral_pitch.cpp',
    field: 'voiced_threshold',
    reason: 'Voicing probability threshold; 0 keeps the core default, outside [0,1] is refused.',
  },
  {
    file: 'src/c_api/mixing_strip.cpp',
    field: 'true_peak_oversample',
    reason:
      'True-peak oversampling factor; the C ABI documents 0 as the default (4) and refuses a factor outside [0, 16].',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'segmentation_threshold_cents',
    reason: 'As the features_spectral_pitch field of the same name; one config, two doors.',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'min_note_ms',
    reason: 'As the features_spectral_pitch field of the same name.',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'reference_hz',
    reason: 'As the features_spectral_pitch field of the same name.',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'voiced_threshold',
    reason: 'As the features_spectral_pitch field of the same name.',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'fade_ms',
    reason: 'Note-render fade length; 0 keeps the core default, negative and non-finite refused.',
  },
  {
    file: 'src/c_api/sonare_c_daw.cpp',
    field: 'vibrato_cutoff_hz',
    reason:
      'Pitch-decomposition cutoff; 0 keeps the core default, negative and non-finite refused.',
  },
  {
    file: 'src/c_api/sonare_c_editing.cpp',
    field: 'n_fft',
    reason:
      'Spectrum window size; 0 keeps the core default and the resolved value must be a power of two. Negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_editing.cpp',
    field: 'octave_fraction',
    reason: 'Octave-smoothing denominator; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_editing.cpp',
    field: 'db_ref',
    reason: 'dB reference level; 0 keeps the core default, negative and non-finite refused.',
  },
  {
    file: 'src/c_api/sonare_c_editing.cpp',
    field: 'db_amin',
    reason: 'dB floor; 0 keeps the core default, negative and non-finite refused.',
  },
  {
    file: 'src/c_api/sonare_c_effects.cpp',
    field: 'n_components',
    reason: 'Stem-decomposition component count; 0 keeps the core default (4), negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_effects.cpp',
    field: 'n_fft',
    reason: 'Stem-decomposition window size; 0 keeps the core default (2048), negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_effects.cpp',
    field: 'hop_length',
    reason: 'Stem-decomposition hop; 0 keeps the core default (512), negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_effects.cpp',
    field: 'n_iter',
    reason: 'Stem-decomposition iterations; 0 keeps the core default (100), negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_effects.cpp',
    field: 'mask_power',
    reason:
      'Stem mask exponent; 0 keeps the core default (1), negative and non-finite refused and the resolved value must be at least 1.',
  },
  {
    file: 'src/c_api/sonare_c_engine.cpp',
    field: 'click_seconds',
    reason:
      'Metronome click duration; 0 keeps the 2 ms default. THE ONE ENTRY HERE WHOSE REFUSAL IS NOT ABOVE ITS FILTER: the filter sits in metronome_from_c and the rejection of anything outside [0, kMaxMetronomeClickSeconds] sits 433 lines below it in sonare_engine_set_metronome, a different function. It holds only because metronome_from_c is in an anonymous namespace with exactly one caller, which is that entry point after it has validated -- a property of the current call graph, not of the code. A second caller added later makes this field unrefused and nothing here would say so, because the check below only asks whether the file states a refusal.',
    refusedAs: 'config->click_seconds',
  },
  {
    file: 'src/c_api/sonare_c_engine_render.cpp',
    field: 'dither_bits',
    reason: 'Dither target word length; 0 keeps the default (16), negative refused.',
  },
  {
    file: 'src/c_api/sonare_c_polyphony.cpp',
    field: 'fade_ms',
    reason: 'As the sonare_c_daw field of the same name; one config resolved the same way.',
  },
  {
    file: 'src/c_api/sonare_c_polyphony.cpp',
    field: 'vibrato_cutoff_hz',
    reason: 'As the sonare_c_daw field of the same name.',
  },
  {
    file: 'src/c_api/synth_patch_common.h',
    field: 'engine_mode',
    reason:
      'A +1-biased synth enum where 0 means unspecified, so the ordinal carries the sentinel. Refused outside the enum, negatives included.',
  },
  {
    file: 'src/c_api/synth_patch_common.h',
    field: 'waveform',
    reason: 'A +1-biased synth enum, as engine_mode.',
  },
  {
    file: 'src/c_api/synth_patch_common.h',
    field: 'filter_model',
    reason: 'A +1-biased synth enum, as engine_mode.',
  },
  {
    file: 'src/c_api/synth_patch_common.h',
    field: 'filter_output',
    reason: 'A +1-biased synth enum, as engine_mode.',
  },
  {
    file: 'src/c_api/synth_patch_common.h',
    field: 'body',
    reason: 'A +1-biased synth enum, as engine_mode.',
  },
  {
    file: 'src/wasm/bindings/editing/polyphony.cpp',
    field: 'fade_ms',
    reason: 'The WASM door onto the same note-render config; same sentinel, same refusal.',
  },
  {
    file: 'src/wasm/bindings/editing/polyphony.cpp',
    field: 'vibrato_cutoff_hz',
    reason: 'The WASM door onto the same note-render config.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'threshold_cents',
    reason: 'WASM spelling of segmentation_threshold_cents; same sentinel, same refusal.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'min_note_ms',
    reason: 'WASM note-extractor config field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'reference_hz',
    reason: 'WASM note-extractor config field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'voiced_threshold',
    reason: 'WASM note-extractor config field; 0 keeps the core default, outside [0,1] refused.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'fade_ms',
    reason: 'WASM note-render config field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/notes.cpp',
    field: 'vibrato_cutoff_hz',
    reason: 'WASM pitch-decomposition cutoff; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/decomposition.cpp',
    field: 'n_components',
    reason: 'WASM stem-decomposition field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/decomposition.cpp',
    field: 'n_fft',
    reason: 'WASM stem-decomposition field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/decomposition.cpp',
    field: 'hop_length',
    reason: 'WASM stem-decomposition field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/decomposition.cpp',
    field: 'n_iter',
    reason: 'WASM stem-decomposition field; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/effects/decomposition.cpp',
    field: 'mask_power',
    reason: 'WASM stem mask exponent; 0 keeps the core default, negative refused.',
  },
  {
    file: 'src/wasm/bindings/metering/metering.cpp',
    field: 'n',
    reason:
      'The spectrum window size read off the metering options bag; 0 keeps the core default, negative refused by name.',
  },
  {
    file: 'src/wasm/bindings/metering/metering.cpp',
    field: 'f',
    reason:
      'The octave-smoothing denominator read off the same bag; 0 keeps the core default, negative refused by name.',
  },
  {
    file: 'src/wasm/bindings/realtime/transport.cpp',
    field: 'click_seconds',
    reason: 'The WASM door onto the metronome click duration; same sentinel, same refusal.',
  },
];

/**
 * `> 0` comparisons whose subject is not a caller's optional at all, so no value
 * of it can select a library default.
 *
 * These are the sites the structural filter over-includes: a guard whose body
 * happens to pass its count onward reads like the idiom and is not it. An entry
 * expires with its comparison.
 */
const NOT_SENTINEL_FILTERS: ReadonlyMap<string, string> = new Map([
  [
    'bindings/node/src/addon/features/rhythm.cpp:n_frames',
    'A divisor guard around a frame count derived from the result, not read from the caller; 0 rows means an empty result rather than a default.',
  ],
  [
    'bindings/node/src/addon/sonare_wrap_features.cpp:n_bins',
    'A divisor guard over a bin count the core returned; the false branch reports an empty shape, not a default.',
  ],
  [
    'bindings/node/src/addon/sonare_wrap_polyphony.cpp:frame_count_',
    'An allocation size taken from analysis state the handle already holds; 0 allocates nothing.',
  ],
  [
    'bindings/node/src/addon/sonare_wrap_streaming.cpp:written',
    'The number of samples the C ABI reported writing; it bounds a memcpy and is never a request.',
  ],
  [
    'bindings/node/src/addon/sonare_wrap_synth_patch.h:num_mod_routings',
    'The routing count on a patch the core produced, sizing the JS array it is copied into.',
  ],
  [
    'src/c_api/mixing_mixer.cpp:n',
    'A loop index deciding whether a separator precedes the next warning line.',
  ],
  [
    'src/c_api/mixing_strip.cpp:distance',
    'UNSETTLED. A surround-pan distance whose non-positive values are treated as "keep the core default (1.0)" ON PURPOSE, so a C host that zero-initialises SonareSurroundPan matches the Node and Python facades, which inject 1.0. That makes 0 a real sentinel and the site a deliberate <= 0 contract rather than a mis-spelled == 0 one -- but a NaN distance also lands on 1.0 here, and whether that should be refused instead has not been decided. Recorded as unsettled rather than cleared.',
  ],
  [
    'src/c_api/project_bounce.cpp:block_size',
    'The project bounce documents this one as "<= 0 => 128", so a non-positive value is a documented default request rather than a mis-spelled sentinel.',
  ],
  [
    'src/c_api/project_bounce.cpp:num_channels',
    'Documented as "<= 0 => 2", and the resolved width is then refused unless it is 1 or 2.',
  ],
  [
    'src/c_api/project_bounce.cpp:sample_rate',
    'Documented as "<= 0 => the project\'s", and any positive value other than the project\'s own rate is refused.',
  ],
  [
    'src/c_api/project_bounce_internal.h:max_block_size',
    'The block size the engine prepared a hosted instrument with, not a caller field; the floor of 1 keeps a pending-event buffer non-empty.',
  ],
  [
    'src/c_api/project_bounce_internal.h:num_samples',
    'The current block length, used to place a pending event at its last frame.',
  ],
  [
    'src/c_api/sonare_c_daw.cpp:total_amplitude',
    'A summed allocation size; 0 means there is nothing to allocate.',
  ],
  ['src/c_api/sonare_c_daw.cpp:total_envelope', 'A summed allocation size, as total_amplitude.'],
  [
    'src/c_api/sonare_c_engine_clips.cpp:anchor_index',
    'A loop index selecting whether a warp anchor has a predecessor to be ordered against.',
  ],
  [
    'src/wasm/bindings/features/spectral.cpp:rows',
    'A divisor guard over a row count the core returned; the false branch reports an empty shape.',
  ],
  [
    'src/wasm/bindings/mixing/mixing_processing.cpp:count',
    'The number of input channels staged for one mixer block; it selects a pointer or null, not a default.',
  ],
  [
    'src/wasm/bindings/mixing/mixing_processing.cpp:length',
    'The block length of the render just performed, bounding the copy back into JS.',
  ],
  [
    'src/wasm/bindings/project/project_bounce.cpp:total',
    'A count the C ABI reported through an out-parameter, sizing the second call that fills it.',
  ],
  [
    'src/wasm/bindings/realtime/capture.cpp:count',
    'The number of captured frames the engine reported, bounding the copy back into JS.',
  ],
]);

/** Both registers, which together must answer every site the scan finds. */
const ACCOUNTED: ReadonlyMap<string, string> = new Map([
  ...REFUSED_OPTION_FIELDS.map(
    ({ file, field, reason }) => [`${file}:${field}`, reason] as [string, string],
  ),
  ...NOT_SENTINEL_FILTERS,
]);

/** A tree whose only filter is the defect, unanswered. */
const UNANSWERED: SentinelSource[] = [
  {
    file: 'fake.cpp',
    text: 'void f(const Options& o) { int n = o.n_fft > 0 ? o.n_fft : 2048; }\n',
  },
];

describe('a `> 0` sentinel filter is removed or answered', () => {
  it('reports nothing about the scanned trees as they stand', () => {
    // RED WHEN: any `> 0` filter is added without an entry in REFUSED_OPTION_FIELDS
    // or NOT_SENTINEL_FILTERS, or an entry stops matching a live comparison.
    expect(evaluateSentinelFilterScope(sentinelSources(), ACCOUNTED)).toEqual([]);
  });

  it('clears the floor it is sized for, so the sweep swept something', () => {
    // RED WHEN: the scan's regexes stop matching (a clang-format change that
    // wraps these comparisons, a rename of the shapes). Without this, a dead
    // regex would report an empty population and read as a clean tree.
    expect(filterSites().length).toBeGreaterThan(50);
    expect(new Set(filterSites().map((site) => site.file)).size).toBeGreaterThan(10);
  });

  it('sees both spellings of the idiom', () => {
    // RED WHEN: either half of the scan breaks. A population made only of
    // guards, or only of ternaries, would clear the floor above while being
    // blind to the other form.
    const forms = new Set(filterSites().map((site) => site.form));
    expect([...forms].sort()).toEqual(['guard', 'ternary']);
  });

  it('finds no `> 0` comparison on any field whose filter was removed', () => {
    // RED WHEN: any removed filter is restored, in either spelling and under any
    // of the names its entry lists. This is the per-site ratchet, and it is the
    // assertion to aim an ablation at.
    const restored = REMOVED_FILTERS.flatMap(({ file, fields }) =>
      fields.flatMap((field) =>
        comparisonsAgainstZero(file, field).map((line) => `${file}:${line} ${field} > 0`),
      ),
    );
    expect(
      restored,
      'A field whose sentinel filter was removed is compared against 0 again. Its zero selects a ' +
        'library default, so `> 0` sends a negative or non-finite request to that default instead ' +
        'of to the refusal it earned.',
    ).toEqual([]);
  });

  it('still finds the refusal every remaining option field depends on', () => {
    // RED WHEN: a validation block guarding one of these fields is deleted while
    // its `> 0` filter stays. That is the edit this register exists to catch --
    // the filter alone is harmless, the filter without the refusal is the defect.
    // Checked per SPELLING, not per identifier: a bare-name match is answered by
    // an unrelated refusal of a same-named parameter elsewhere in the file, which
    // is how deleting a real refusal first went unnoticed here.
    const unrefused = REFUSED_OPTION_FIELDS.flatMap(({ file, field, refusedAs }) =>
      subjectsOf(`${file}:${field}`)
        .filter((subject) => refusalMentioning(file, refusedAs ?? subject).length === 0)
        .map((subject) => `${file}:${subject}`),
    );
    expect(
      unrefused,
      'These fields are filtered with `> 0` and their file no longer states a refusal spelled the ' +
        'way the filter spells them. Either the refusal was deleted, or the field moved and this ' +
        'entry is stale.',
    ).toEqual([]);
  });

  it('keeps the refused-field register free of fields that no longer filter', () => {
    // RED WHEN: a field is converted to `== 0` (or its filter deleted) without
    // its entry going with it. A dead entry keeps asserting a reviewed decision
    // about a name, which the next field to take that name would inherit.
    const live = new Set(filterSites().map((site) => site.id));
    const stale = REFUSED_OPTION_FIELDS.map(({ file, field }) => `${file}:${field}`).filter(
      (id) => !live.has(id),
    );
    expect(stale).toEqual([]);
  });
});

describe('each failure class fires on its own', () => {
  const only = (findings: { heading: string; lines: string[] }[], fragment: string) => {
    expect(findings.map((finding) => finding.heading)).toHaveLength(1);
    expect(findings[0].heading).toContain(fragment);
    return findings[0].lines;
  };

  it('says nothing about a tree whose only filter is answered', () => {
    const reasons = new Map([['fake.cpp:n_fft', 'why this one is refused first']]);
    expect(evaluateSentinelFilterScope(UNANSWERED, reasons, 0)).toEqual([]);
  });

  it('reports an unanswered filter, and only that', () => {
    const lines = only(
      evaluateSentinelFilterScope(UNANSWERED, new Map(), 0),
      'carry no recorded reason',
    );
    expect(lines).toEqual(['fake.cpp:n_fft (fake.cpp:1 ternary)']);
  });

  it('reports a reason that excuses nothing, and only that', () => {
    const reasons = new Map([
      ['fake.cpp:n_fft', 'why this one is refused first'],
      ['gone.cpp:hop_length', 'why'],
    ]);
    const lines = only(
      evaluateSentinelFilterScope(UNANSWERED, reasons, 0),
      'matched no `> 0` filter',
    );
    expect(lines).toEqual(['gone.cpp:hop_length']);
  });

  it('reports a shrunken population, and only that', () => {
    const reasons = new Map([['fake.cpp:n_fft', 'why this one is refused first']]);
    const lines = only(
      evaluateSentinelFilterScope(UNANSWERED, reasons, 2),
      'no longer finds the population',
    );
    expect(lines).toEqual(['sites: found 1, floor is 2']);
  });
});

describe('the scanner sees what it claims to', () => {
  it('reads the guard form as well as the ternary form', () => {
    const both: SentinelSource[] = [
      {
        file: 'fake.cpp',
        text: [
          'void a(const O& o) { x = o.n_fft > 0 ? o.n_fft : 2048; }',
          'void b(const O* o) { if (o->hop_length > 0) cfg.hop_length = o->hop_length; }',
        ].join('\n'),
      },
    ];
    expect(filterSites(both).map((site) => `${site.field}:${site.form}`)).toEqual([
      'n_fft:ternary',
      'hop_length:guard',
    ]);
  });

  it('ignores a guard whose body does not apply the compared value', () => {
    // The structural filter that keeps every output-count memcpy out of the
    // register. Without it the population is four times larger and every extra
    // entry is a memcpy, which is how a register stops being read.
    const copy: SentinelSource[] = [
      {
        file: 'fake.cpp',
        text: 'void a() { if (count > 0) std::memcpy(out.data(), scratch.data(), bytes); }\n',
      },
    ];
    expect(filterSites(copy)).toEqual([]);
  });

  it('reads a ternary whose true branch calls a qualified name', () => {
    // The `:` of a `::` used to end the true branch, so every site calling into
    // a namespace fell out of the scan and owed no reason.
    const qualified: SentinelSource[] = [
      {
        file: 'fake.cpp',
        text: 'void a() { n = frames > 0 ? std::min<int64_t>(frames, cap) : 0; }\n',
      },
    ];
    expect(filterSites(qualified).map((site) => site.field)).toEqual(['frames']);
  });

  it('keys a member read and a bare read of one field as one entry', () => {
    const chained: SentinelSource[] = [
      {
        file: 'fake.cpp',
        text: [
          'void a(const O* o) { if (o->fade_ms > 0) cfg.fade_ms = o->fade_ms; }',
          'void b(float fade_ms) { if (fade_ms > 0) cfg.fade_ms = fade_ms; }',
        ].join('\n'),
      },
    ];
    expect([...new Set(filterSites(chained).map((site) => site.id))]).toEqual(['fake.cpp:fade_ms']);
  });

  it('reads a float comparison spelled with a suffix', () => {
    const suffixed: SentinelSource[] = [
      { file: 'fake.cpp', text: 'void a(float g) { if (g > 0.0f) cfg.g = g; }\n' },
    ];
    expect(filterSites(suffixed).map((site) => site.field)).toEqual(['g']);
  });

  it('separates a stated refusal from a file that merely names the field', () => {
    const sources: SentinelSource[] = [
      { file: 'guarded.cpp', text: 'if (!std::isfinite(fade_ms) || fade_ms < 0.0f) return;\n' },
      { file: 'bare.cpp', text: 'cfg.fade_ms = fade_ms;\n' },
    ];
    expect(refusalMentioning('guarded.cpp', 'fade_ms', sources)).toEqual([1]);
    expect(refusalMentioning('bare.cpp', 'fade_ms', sources)).toEqual([]);
  });

  it('does not accept a refusal of a same-named value under a different spelling', () => {
    // The miss that made the refusal check worth tightening: one entry point
    // refuses a positional `n_components`, another filters `config->n_components`
    // -- and a bare-identifier match let the first answer for the second, so
    // deleting the second refusal stayed green.
    const shared: SentinelSource[] = [
      {
        file: 'fake.cpp',
        text: [
          'SonareError a(int n_components) { if (n_components <= 0) return kBad; return kOk; }',
          'void b(const C* config) { if (config->n_components > 0) cfg.n = config->n_components; }',
        ].join('\n'),
      },
    ];
    // Querying the subject finds nothing; querying the bare field finds the
    // other function's refusal. Those two lines are the laundering: the site's
    // own subject is the qualified spelling, so a bare-name check would have
    // read line 1 as answering for line 2.
    expect(refusalMentioning('fake.cpp', 'config->n_components', shared)).toEqual([]);
    expect(refusalMentioning('fake.cpp', 'n_components', shared)).toEqual([1]);
    expect(subjectsOf('fake.cpp:n_components', shared)).toEqual(['config->n_components']);
  });

  it('finds a restored filter in a spelling the population scan ignores', () => {
    // The removed-filter ratchet is deliberately wider than filterSites(): a
    // filter coming back inside a compound condition would be invisible to the
    // population scan, and this is the assertion that still catches it.
    const compound: SentinelSource[] = [
      { file: 'fake.cpp', text: 'if (num_ports > 0 && ready) ports = num_ports;\n' },
    ];
    expect(filterSites(compound)).toEqual([]);
    expect(comparisonsAgainstZero('fake.cpp', 'num_ports', compound)).toEqual([1]);
  });
});
