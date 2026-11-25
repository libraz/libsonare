import { describe, expect, it } from 'vitest';
import { zeroFallbackSites } from './_zero_fallback_sources';

// All literal scalar fallbacks must answer what omission means. ABI sentinels
// are marked in the record; the remaining reasons describe a field or a guard.
const ZERO_FALLBACK_REASONS: ReadonlyMap<string, string> = new Map([
  ['project/project_arrange.cpp:anchors', 'Zero is the derived length of an absent anchor list.'],
  [
    'project/project_arrange.cpp:audioSampleRate',
    'ABI sentinel. The rate hint is used only above zero.',
  ],
  [
    'realtime/transport.cpp:clickSeconds',
    'ABI sentinel. Zero selects the initialized click duration.',
  ],
  ['mixing/mixing.cpp:pan', 'A fresh strip is centered before its positioning mode is applied.'],
  ['realtime/processing.cpp:startPpq', 'The default transport placement is the origin.'],
  [
    'analysis/room_acoustics.cpp:materialPreset',
    "The material catalogue's default is the first material preset.",
  ],
  [
    'analysis/room_acoustics.cpp:mode',
    'The room impulse response mode defaults to the first enum value.',
  ],
  [
    'analysis/room_acoustics.cpp:receiverSpacingM',
    'The preceding presence test keeps an absent spacing as std::nullopt.',
  ],
  [
    'common/note_val.h:pitchSemitones',
    'Zero is the documented identity adjustment for the note edit.',
  ],
  ['common/note_val.h:gainDb', 'Zero is the documented identity adjustment for the note edit.'],
  [
    'common/note_val.h:formantSemitones',
    'Zero is the documented identity adjustment for the note edit.',
  ],
  [
    'common/note_val.h:vibratoDepthChange',
    'Zero is the documented identity adjustment for the note edit.',
  ],
  [
    'common/note_val.h:driftChange',
    'Zero is the documented identity adjustment for the note edit.',
  ],
  ['common/note_val.h:muted', 'An omitted note mute leaves the note audible.'],
  [
    'common/synth_patch_val.h:<key>',
    'The helper is invoked only in a supplied-field branch and leaves absent members initialized.',
  ],
  [
    'editing/vocal_edit.cpp:gainDb',
    'The vocal note-edit initializer uses unity gain and no formant shift.',
  ],
  [
    'editing/vocal_edit.cpp:shiftSemitones',
    'The vocal note-edit initializer uses unity gain and no formant shift.',
  ],
  [
    'editing/vocal_edit.cpp:outputLengthSamples',
    'ABI sentinel. C zero selects the source length instead of requesting an empty render.',
  ],
  [
    'editing/vocal_edit.cpp:frameOriginSample',
    'The facade validates supplied analysis grid scalars before copying the analysis to embind; the raw fallback cannot substitute for a facade omission.',
  ],
  [
    'editing/vocal_edit.cpp:samplesPerFrame',
    'The facade validates supplied analysis grid scalars before copying the analysis to embind; the raw fallback cannot substitute for a facade omission.',
  ],
  [
    'editing/vocal_edit.cpp:frameLengthSamples',
    'The facade validates supplied analysis grid scalars before copying the analysis to embind; the raw fallback cannot substitute for a facade omission.',
  ],
  ['effects/notes.cpp:pitchSemitones', 'Zero semitones is no pitch shift.'],
  ['effects/notes.cpp:muted', 'A missing mute means the pitch command leaves the note audible.'],
  ['effects/realtime_voice_changer.cpp:withCarrier', 'The carrier output is opt-in.'],
  ['effects/repair.cpp:clipThresholdDb', 'The option is presence-gated before the C++ read.'],
  ['effects/repair.cpp:thresholdDb', 'The option is presence-gated before the C++ read.'],
  ['features/core.cpp:withReport', 'The extended result is selected by an explicit true.'],
  ['features/core.cpp:length', 'ABI sentinel. Zero is the automatic output length for clicks.'],
  [
    'features/spectral.cpp:k',
    'ABI sentinel. The C domain admits zero as the default neighbour-count request.',
  ],
  [
    'features/spectral.cpp:k',
    'ABI sentinel. The C domain admits zero as the default neighbour-count request.',
  ],
  ['features/spectrogram.cpp:db', 'Linear magnitude without a phase plane is the default.'],
  ['features/spectrogram.cpp:phase', 'Linear magnitude without a phase plane is the default.'],
  [
    'mixing/mixing_topology.cpp:truePeakOversample',
    'ABI sentinel. C metering uses its default true-peak oversample factor at zero.',
  ],
  ['project/project.cpp:withDiagnostics', 'Diagnostics are opt-in on document load.'],
  [
    'project/project_arrange.cpp:trackId',
    'The zero id is invalid on a new clip and omission is already refused downstream.',
  ],
  [
    'project/project_arrange.cpp:lengthPpq',
    'The C PPQ-length domain already refuses zero, which cannot become a stored clip length.',
  ],
  [
    'project/project_arrange.cpp:sourceOffsetPpq',
    'The advertised placement and source offsets start at the origin.',
  ],
  [
    'project/project_arrange.cpp:loopLengthPpq',
    'The C PPQ-length domain already refuses zero, which cannot become a stored clip length.',
  ],
  ['project/project_arrange.cpp:id', 'ABI sentinel. Zero is the auto-assigned sequence id.'],
  [
    'project/project_bounce.cpp:failed',
    'An internal callback outcome flag, rather than a public request default.',
  ],
  [
    'project/project_bounce.cpp:withTimeline',
    'The rendered timeline is included only when requested.',
  ],
  [
    'project/project_bounce.cpp:destinationId',
    'The public facade requires these binding fields before it calls embind.',
  ],
  [
    'project/project_bounce.cpp:presetName',
    'The public facade requires these binding fields before it calls embind.',
  ],
  [
    'project/project_bounce.cpp:channel',
    'The public facade requires these binding fields before it calls embind.',
  ],
  [
    'project/project_bounce.cpp:preferModelForModeledFamilies',
    'The instrument model preference and clearing a bank rig are opt-in.',
  ],
  [
    'project/project_bounce.cpp:clearBankRig',
    'The instrument model preference and clearing a bank rig are opt-in.',
  ],
  [
    'project/project_meta.cpp:name',
    'ABI sentinel. The optional marker name maps to the native unnamed marker representation.',
  ],
  [
    'project/project_midi.cpp:useGmPrograms',
    'The preceding presence flag tracks whether a GM-selection override was supplied.',
  ],
  [
    'project/project_midi.cpp:group',
    'The default route in the MIDI edit uses the first group and channel.',
  ],
  [
    'project/project_midi.cpp:channel',
    'The default route in the MIDI edit uses the first group and channel.',
  ],
  ['project/project_midi.cpp:withSourceIndex', 'Including source indices is opt-in.'],
  [
    'project/project_midi.cpp:ccLsbNumber',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  [
    'project/project_midi.cpp:selectorMsb',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  [
    'project/project_midi.cpp:selectorLsb',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  ['project/project_midi.cpp:minValue', 'The documented default lower end of the mapping is 0.'],
  ['project/project_midi.cpp:consume', 'Consumption and thru routing are opt-in in the C records.'],
  ['project/project_midi.cpp:thru', 'Consumption and thru routing are opt-in in the C records.'],
  [
    'project/project_transcribe.cpp:group',
    'The transcribed MIDI uses the first group and channel when unspecified.',
  ],
  [
    'project/project_transcribe.cpp:channel',
    'The transcribed MIDI uses the first group and channel when unspecified.',
  ],
  [
    'project/project_vocal_edit.cpp:takeId',
    "ABI sentinel. Zero is C's base-clip binding selector.",
  ],
  [
    'project/project_vocal_edit.cpp:failed',
    'Internal callback outcome flags, rather than public request fields.',
  ],
  [
    'project/project_vocal_edit.cpp:cancelled',
    'Internal callback outcome flags, rather than public request fields.',
  ],
  [
    'realtime/clips.cpp:trackId',
    'Presence is checked; the unassigned track is explicit in the other branch.',
  ],
  ['realtime/clips.cpp:fadeInSamples', 'The clip schedule starts with a zero-length fade.'],
  ['realtime/clips.cpp:fadeOutSamples', 'The clip schedule starts with a zero-length fade.'],
  [
    'realtime/midi.cpp:wordCount',
    'ABI sentinel. Zero selects UMP word-count inference; the pending parity patch replaces this read.',
  ],
  [
    'realtime/midi_control.cpp:ccLsbNumber',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  [
    'realtime/midi_control.cpp:selectorMsb',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  [
    'realtime/midi_control.cpp:selectorLsb',
    'The CC binding record defaults its inactive selector bytes to zero.',
  ],
  ['realtime/midi_control.cpp:minValue', 'The controller mapping default lower end is 0.'],
  ['realtime/midi_control.cpp:consume', 'MIDI CC consumption is opt-in.'],
  ['realtime/midi_control.cpp:lo', 'The controller mapping default lower end is 0.'],
  [
    'realtime/midi_io.cpp:bankMsb',
    'The bank-valid bit distinguishes an absent bank from a bank of zero.',
  ],
  [
    'realtime/midi_io.cpp:bankLsb',
    'The bank-valid bit distinguishes an absent bank from a bank of zero.',
  ],
  [
    'realtime/params.cpp:id',
    'Omission and a supplied zero are refused; there is no parameter with id 0.',
  ],
  [
    'realtime/params.cpp:name',
    'ParameterInfo initializes both display strings empty and its minimum/default values to zero.',
  ],
  [
    'realtime/params.cpp:unit',
    'ParameterInfo initializes both display strings empty and its minimum/default values to zero.',
  ],
  [
    'realtime/params.cpp:minValue',
    'ParameterInfo initializes both display strings empty and its minimum/default values to zero.',
  ],
  [
    'realtime/params.cpp:defaultValue',
    'ParameterInfo initializes both display strings empty and its minimum/default values to zero.',
  ],
  ['realtime/params.cpp:smoothed', 'The unqualified getter reads the unsmoothed control value.'],
  [
    'realtime/processing.cpp:type',
    'The graph defaults are passThrough type and unity gain, as documented.',
  ],
  [
    'realtime/processing.cpp:gainDb',
    'The graph defaults are passThrough type and unity gain, as documented.',
  ],
  [
    'realtime/processing.cpp:inputNode',
    'The public setGraph facade validates both required endpoint names before embind.',
  ],
  [
    'realtime/processing.cpp:outputNode',
    'The public setGraph facade validates both required endpoint names before embind.',
  ],
  ['realtime/processing.cpp:withMonitor', 'Monitoring is opt-in.'],
  [
    'realtime/processing.cpp:totalFrames',
    'The render length is required and zero is already refused.',
  ],
  [
    'realtime/processing.cpp:blockSize',
    "ABI sentinel. C's offline options reserve zero to select the prepared/default block or sample rate.",
  ],
  [
    'realtime/processing.cpp:sourceSampleRate',
    "ABI sentinel. C's offline options reserve zero to select the prepared/default block or sample rate.",
  ],
  [
    'realtime/processing.cpp:targetSampleRate',
    "ABI sentinel. C's offline options reserve zero to select the prepared/default block or sample rate.",
  ],
  [
    'realtime/processing.cpp:dither',
    'Offline bounce uses no dither and no loudness normalization by default.',
  ],
  [
    'realtime/processing.cpp:normalizeLufs',
    'Offline bounce uses no dither and no loudness normalization by default.',
  ],
  [
    'realtime/processing.cpp:blockSize',
    "ABI sentinel. C's offline options reserve zero to select the prepared/default block or sample rate.",
  ],
  [
    'realtime/transport.cpp:blockSize',
    'ABI sentinel. Zero selects the automatic block length or the click duration in seconds.',
  ],
  [
    'realtime/transport.cpp:name',
    "ABI sentinel. The optional name maps to the engine's unnamed marker representation.",
  ],
  ['realtime/transport.cpp:enabled', 'The metronome is disabled by default.'],
  [
    'realtime/transport.cpp:clickSamples',
    'ABI sentinel. Zero selects the automatic block length or the click duration in seconds.',
  ],
]);

describe('literal fallbacks on WASM scalar inputs', () => {
  it('accounts for every literal fallback in the binding sources', () => {
    const sites = zeroFallbackSites();
    expect(sites.some(({ id }) => id.startsWith('realtime/'))).toBe(true);
    expect(
      sites
        .filter(({ id }) => !ZERO_FALLBACK_REASONS.has(id))
        .map(({ id, line, literal }) => `${id} (line ${line}, literal ${literal})`),
    ).toEqual([]);
  });

  it('expires each reason when its read is removed', () => {
    const current = new Set(zeroFallbackSites().map(({ id }) => id));
    expect([...ZERO_FALLBACK_REASONS.keys()].filter((id) => !current.has(id))).toEqual([]);
  });
});
