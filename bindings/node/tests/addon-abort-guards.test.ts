/**
 * The addon is built with NAPI_DISABLE_CPP_EXCEPTIONS, so a failed typed read
 * (`value.As<Napi::Number>().DoubleValue()` on a missing field) does not raise a
 * C++ exception — it leaves a pending JS exception and returns a dummy value.
 * Three consequences make bad input lethal rather than merely wrong:
 *
 *  1. A second N-API throw while an exception is pending is a
 *     `FATAL ERROR ... napi_throw` abort. An entry point that keeps parsing
 *     after the first bad field kills the process through any `try`/`catch`.
 *  2. A C++ exception inside the callback — a `std::length_error` from
 *     `std::vector<T> v(n)` after a negative count wrapped to SIZE_MAX —
 *     terminates it for the same reason.
 *  3. The dummy value is an ordinary `0` / `""` / `false`, so a C-ABI call built
 *     from it runs and succeeds. `ThrowIfError` cannot stop that: the native
 *     call is its ARGUMENT, so it has already run. The caller gets a TypeError
 *     with the gain already zeroed and the play head already back at 0.
 *
 * These tests pin all three shut: a covered entry point must turn the FIRST
 * offending field into exactly one catchable `TypeError` (or `RangeError` for an
 * out-of-domain count or a MIDI byte the narrowing cast would wrap), issue no
 * C-ABI call, and still be alive after. State is asserted by snapshotting either
 * side of the rejected call, and the matrix also runs in a child process, so
 * "survived" is a real exit code rather than the runner happening to continue.
 *
 * The table itself is `_abort_guard_cases.ts` and the handles it drives are
 * `_abort_guard_handles.ts`; what stays here is the reasoned registers and the
 * assertions. The table's membership is not decided here either: the addon
 * sources are scanned for every entry point reading a positional argument
 * through the bail-out reader family, and one missing from the table or the
 * reasoned register fails.
 */

import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';
import { CASES, clickTrack } from './_abort_guard_cases.js';
import {
  BLOCK,
  BUS_ID,
  engineStateSnapshot,
  type NativeEngine,
  PARAM_ID,
  PREFETCH_FRAMES,
  PROJECT_SR,
  PROJECT_TRACK_GAIN,
  pump,
  RECORD_OFFSET,
  rms,
  SR,
  TRACK_ID,
  trackStripJson,
  transportSnapshot,
  withConfiguredEngine,
  withConfiguredProject,
  withEngine,
  withPreparedEngine,
  withProject,
} from './_abort_guard_handles.js';
import {
  bailoutReaderCalls,
  inlineTypedArgumentReads,
  isBailoutGuarded,
  positionalArgEntryPoints,
  positionalReaderDefinitions,
  SHARED_READER_FILE,
} from './_addon_sources.js';

/**
 * Definitions with the `(Napi::CallbackInfo, index, ...)` shape that may live
 * outside {@link SHARED_READER_FILE}, each with the reason the shared bail-out
 * family does not cover it. Keyed by `file:name`. A definition that is neither
 * here nor in the shared header is a file-local positional reader, and that is
 * exactly what went wrong four times: `Uint32Arg`, `NumberArg`, `OptionalInt64`
 * and `MidiByteArg` each read `info[i].As<Napi::Number>()` with no type check
 * and no pending-exception guard, so a rejected argument arrived at the C ABI
 * as a dummy 0 and a second reader's throw aborted the process.
 */
const POSITIONAL_READER_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'sonare_wrap_utils.h:RequireFloat32Array',
    'Type predicate over a Float32Array argument, reported with a message the call site supplies, not a scalar read. Shared from the other addon-wide header, not a per-file copy.',
  ],
  [
    'engine/common.h:ReadParameter',
    'Fills a whole SonareParameterInfo struct from an object argument; the scalar family has nothing to delegate to. Already guards on IsExceptionPending before returning true, and is shared by the engine TUs from one header.',
  ],
  [
    'engine/common.h:ReadChannels',
    'Copies an array of Float32Array planes into a ChannelBlock; not a scalar read, and shared by the engine TUs from one header.',
  ],
  [
    'effects/mastering_pair.cpp:AssistantConfigFromParams',
    'Not a positional scalar reader: the index names an options OBJECT it flattens into mastering params. Matched only because the shape check is deliberately loose about what follows the index.',
  ],
]);

/**
 * Entry points that can reject a positional argument but are not driven by the
 * table, each with why. This is the visible-gap register: adding a new
 * same-shaped entry point without covering it fails the coverage test rather
 * than silently shipping untested.
 */
/**
 * The reason the ten GM/GM2 name lookups in `addon.cpp` may keep their
 * accessor-less `info[i].As<Napi::Number>()` reads for now.
 *
 * They became visible when the scanner's premise was corrected: an
 * accessor-less `.As<>()` "cannot fail" is true of `Napi::Object` and false of
 * `Napi::Number`, whose conversion operators run the same conversion the
 * explicit accessor would. So these are genuine unchecked reads and the scanner
 * is right to see them — but every one was driven with a wrong-typed argument
 * and with no argument at all, and each answers a single catchable
 * `TypeError: A number was expected` with the addon still usable afterwards.
 * The conversion sets a pending exception and node-addon-api surfaces it as one
 * throw rather than the abort this family exists to prevent.
 *
 * That measurement is what this entry rests on, so the measured STATE is
 * pinned rather than described: see {@link GM_NAME_LOOKUP_POPULATION} and the
 * case that asserts it. A suppression whose expiry condition exists only as a
 * sentence is a declared level, not a verified one — it goes on blessing the
 * file while the shape it excused quietly changes underneath.
 */
const GM_NAME_LOOKUP_REASON =
  'Accessor-less Napi::Number read, so the conversion is implicit rather than absent. Driven with a wrong-typed argument and with none: answers one catchable TypeError and leaves the addon usable. Scoped to the exact population pinned by GM_NAME_LOOKUP_POPULATION, so a new read or a new body fails rather than inheriting this.';

/**
 * The exact shape this suppression was measured against, not a target.
 *
 * An eleventh function, or one more read inside any of the ten, moves the
 * population off these numbers and fails the case below — which is the point.
 * The entries above excuse a set of reads that were each driven and found to
 * answer one catchable error; they do not excuse `addon.cpp`, and they must not
 * become a blanket blessing that the next read to land there inherits
 * unexamined. Re-measure and re-justify, or move the body onto the shared
 * reader family and delete its entry.
 */
const GM_NAME_LOOKUP_POPULATION = { reads: 15, functions: 10 } as const;

/**
 * Inline typed positional reads that may stay inline, each with the reason the
 * accessor cannot fail there — or, for the implicit-conversion form, the reason
 * its failure is already safe. Keyed by `file:enclosing-function`. The scan is
 * deliberately body-local — a guard living in a helper is a real coupling
 * hazard, since editing the helper silently unguards every call site — so a
 * site guarded from a distance has to say so here rather than read as safe.
 */
const INLINE_READ_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  ...[
    'MidiGmInstrumentName',
    'MidiGmFamilyName',
    'MidiGmFamilyFirstProgram',
    'MidiGm2InstrumentName',
    'MidiGmDrumName',
    'MidiGm2DrumSetName',
    'MidiGm2DrumName',
    'MidiCcName',
    'MidiPerNoteControllerName',
    'MidiBankProgram',
  ].map((name) => [`addon.cpp:${name}`, GM_NAME_LOOKUP_REASON] as [string, string]),
]);

const UNCOVERED_POSITIONAL_GUARDS: ReadonlyMap<string, string> = new Map([
  [
    'midiCcLearn',
    'Stateless free function over an event array; it owns no project or engine state to compare, and its argument rejection is driven by public-input-conformance.test.ts.',
  ],
  [
    'noteAmplitude',
    'Instance method on PolyphonicAnalysis, which needs an analysed chord; its note-index rejection is driven by polyphonic-analysis.test.ts.',
  ],
  [
    'noteEnvelope',
    'Instance method on PolyphonicAnalysis, which needs an analysed chord; its note-index rejection is driven by polyphonic-analysis.test.ts.',
  ],
  [
    'noteF0',
    'Instance method on PolyphonicAnalysis, which needs an analysed chord; its note-index rejection is driven by polyphonic-analysis.test.ts.',
  ],
  [
    'noteSalience',
    'Instance method on PolyphonicAnalysis, which needs an analysed chord; its note-index rejection is driven by polyphonic-analysis.test.ts.',
  ],
  [
    'processWithOffset',
    'Instance method on StreamAnalyzer, which needs a configured session; its sample-offset rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'readFramesI16',
    'Instance method on StreamAnalyzer, which needs a configured session; its frame-count rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'readFramesSoa',
    'Instance method on StreamAnalyzer, which needs a configured session; its frame-count rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'readFramesU8',
    'Instance method on StreamAnalyzer, which needs a configured session; its frame-count rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'readGoniometerLatest',
    'Instance method on Mixer, which needs a configured strip and a running meter; covered by metering-and-scale.test.ts.',
  ],
  [
    'reset',
    'Instance method on StreamAnalyzer, which needs a configured session; its base-offset rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'setBand',
    'Instance method on StreamingEqualizer, which needs an initialized equalizer; its band-index rejection is driven by basic_streaming_metering.test.ts.',
  ],
  [
    'setNoteEdit',
    'Instance method on PolyphonicAnalysis, which needs an analysed chord; its note-index rejection is driven by polyphonic-analysis.test.ts.',
  ],
]);

/** Proves the process is still usable, not merely that no exception escaped. */
function expectNativeStillWorks(): void {
  expect(withEngine((engine) => engine.graphNodeCount())).toBe(0);
  expect(withProject((project) => project.trackCount())).toBe(0);
}

/**
 * The other half of the positional readers' contract, which the table above
 * cannot express: that table pins REJECTIONS, and this is the opposite.
 *
 * `sonare_wrap_options.h` states it for the whole `node_arg_*` family in as many
 * words — "a missing OR present-but-non-number argument at index falls back to
 * fallback (a type-checked fallback, not a presence-only check)" — and until
 * now nothing asserted it, which is exactly how moving these sites onto the
 * strict reader changed the behaviour with nothing going red.
 *
 * The two contracts are separate and both are live on the same argument. A
 * magnitude past the native `int` must be refused, because ToInt32 wraps it into
 * a different, legal kernel and the call then separates on a setting the caller
 * never asked for. A non-number must NOT be refused, because falling back is
 * what this family promises. Asserted by result rather than by "it did not
 * throw", so a fallback that quietly selected something other than the default
 * fails too.
 */
/** The shape a C-ABI-coded refusal reaches JS as; the addon never constructs a class. */
interface SonareErrorShape extends Error {
  code?: number;
}

/** Mirrors the C ABI's SONARE_ERROR_INVALID_PARAMETER, which is what `.code` carries. */
const INVALID_PARAMETER = 4;

const captureError = (run: () => unknown): SonareErrorShape | undefined => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error as SonareErrorShape;
  }
};

describe('the HPSS positional readers keep their type-checked fallback', () => {
  const tone = new Float32Array(2048).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / SR));

  /** Every float field of a result reduced to one comparable string. */
  const digest = (result: unknown): string =>
    Object.entries(result as Record<string, unknown>)
      .filter(([, field]) => field instanceof Float32Array)
      .map(([key, field]) => {
        const values = field as Float32Array;
        let acc = 0;
        for (let i = 0; i < values.length; i++) {
          acc = (acc * 31 + values[i]) % 1e9;
        }
        return `${key}:${values.length}:${acc.toFixed(6)}`;
      })
      .join('|');

  const entries = [
    { name: 'hpss', run: (...rest: unknown[]) => addon.hpss(tone, SR, ...rest) },
    {
      name: 'hpssWithResidual',
      run: (...rest: unknown[]) => addon.hpssWithResidual(tone, SR, ...rest),
    },
  ];

  // The default each argument falls back to, written positionally. A wrong type
  // at one position must produce exactly this run.
  const DEFAULTS = [31, 31, 2048, 512];
  const POSITIONS = ['kernelHarmonic', 'kernelPercussive', 'nFft', 'hopLength'];

  for (const entry of entries) {
    for (const [index, argument] of POSITIONS.entries()) {
      it(`${entry.name}: a wrong-typed ${argument} falls back to its default`, () => {
        const args = [...DEFAULTS];
        expect(digest(entry.run(...args.map((value, at) => (at === index ? 'x' : value))))).toBe(
          digest(entry.run(...args)),
        );
      });
    }

    it(`${entry.name}: omitted arguments fall back to the same defaults`, () => {
      // A short argument list and an explicit default must agree, or the
      // documented fallback is only half implemented.
      expect(digest(entry.run())).toBe(digest(entry.run(...DEFAULTS)));
    });

    // A well-formed number the library does not accept is the core's to refuse,
    // and it answers with a code-carrying SonareError naming the constraint it
    // broke. An addon-side copy of that rule would be redundant and would
    // downgrade the refusal: a caller switching on `.code` cannot classify a
    // bare RangeError, which is what one entry point used to hand back here.
    for (const [index, argument] of ['nFft', 'hopLength'].entries()) {
      for (const bad of [0, -2048]) {
        it(`${entry.name}: a ${argument} of ${bad} is refused by the core with a code`, () => {
          const args = [...DEFAULTS];
          args[index + 2] = bad;
          const error = captureError(() => entry.run(...args)) as SonareErrorShape;
          expect(error?.name).toBe('SonareError');
          expect(error?.code).toBe(INVALID_PARAMETER);
          // Which of the two was wrong, not merely that one of them was.
          expect(error?.message).toContain(argument);
        });
      }
    }

    // A value that cannot be represented at all is the reader's to refuse, and
    // it is the one class that must NOT be deferred: ToInt32 turns every
    // non-finite number into 0, so the core then reports a rule about 0 — a
    // value the caller never wrote, and for a kernel it does not even say which
    // argument it means.
    for (const [index, argument] of POSITIONS.entries()) {
      it(`${entry.name}: a NaN ${argument} is refused by name rather than read as 0`, () => {
        const args: unknown[] = [...DEFAULTS];
        args[index] = Number.NaN;
        expect(() => entry.run(...args)).toThrow(RangeError);
        expect(captureError(() => entry.run(...args))?.message).toContain(argument);
      });
    }

    for (const infinite of [Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      it(`${entry.name}: a kernelPercussive of ${infinite} is refused by name`, () => {
        expect(() => entry.run(31, infinite, 2048, 512)).toThrow(RangeError);
        expect(captureError(() => entry.run(31, infinite, 2048, 512))?.message).toContain(
          'kernelPercussive',
        );
      });
    }

    it(`${entry.name}: the digest separates a run that used different arguments`, () => {
      // Every case above is an invariance claim, and a digest that came out the
      // same for every input would satisfy all of them while checking nothing.
      // Both halves of the argument list are varied, so neither the kernels nor
      // the framing can be the flat one.
      expect(digest(entry.run(1, 1, 2048, 512))).not.toBe(digest(entry.run(...DEFAULTS)));
      expect(digest(entry.run(31, 31, 1024, 256))).not.toBe(digest(entry.run(...DEFAULTS)));
    });
  }
});

describe('addon object readers stop at the first bad field', () => {
  for (const { name, rejectsArgument } of CASES) {
    for (const { argument, call, error } of rejectsArgument ?? []) {
      it(`${name}: a wrong-typed ${argument} throws exactly one catchable error`, () => {
        expect(() => call()).toThrow(error ?? TypeError);
        expectNativeStillWorks();
      });
    }
  }

  it('every table entry drives at least one hostile call', () => {
    // A vacuous table would make every assertion below pass by iterating
    // nothing, so pin the shape of the table itself. The coverage register
    // below is what pins its SIZE — a floor would only ever say the table did
    // not shrink, never that it kept up with the addon.
    expect(
      CASES.filter(
        (entry) =>
          entry.missingRequired.length === 0 &&
          entry.badOptional === undefined &&
          (entry.badArguments?.length ?? 0) === 0 &&
          (entry.badTransportArguments?.length ?? 0) === 0 &&
          (entry.badProjectArguments?.length ?? 0) === 0 &&
          (entry.rejectsArgument?.length ?? 0) === 0,
      ).map((entry) => entry.name),
    ).toEqual([]);
  });

  for (const { name, missingRequired, badOptional } of CASES) {
    for (const { field, call } of missingRequired) {
      it(`${name}: a missing ${field} throws a catchable TypeError`, () => {
        expect(call).toThrow(TypeError);
        expectNativeStillWorks();
      });
    }

    if (badOptional !== undefined) {
      it(`${name}: a wrong-typed optional field on several entries throws once`, () => {
        expect(badOptional).toThrow(TypeError);
        expectNativeStillWorks();
      });
    }
  }
});

describe('addon positional-argument readers bail out before touching native state', () => {
  it('parks the fixture engine off its default state, so the snapshot can move', () => {
    // Every field the snapshot reads has to be able to change, or comparing it
    // either side of a rejected call would assert nothing. This is that proof.
    withConfiguredEngine((engine) => {
      const status = engine.captureStatus();
      expect(status.armed).toBe(true);
      expect(status.punchEnabled).toBe(true);
      expect(status.source).toBe('input');
      expect(status.recordOffsetSamples).toBe(RECORD_OFFSET);
      expect(engine.clipPagePrefetchFrames()).toBe(PREFETCH_FRAMES);
      expect(engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb')).toBeGreaterThan(0);
      expect(engine.resolveBusInsertAutomationId(BUS_ID, 0, 'band0.gainDb')).toBeGreaterThan(0);
      expect(engine.resolveMasterInsertAutomationId(0, 'band0.gainDb')).toBeGreaterThan(0);
    });
  });

  it('moves the snapshot for a well-typed call, so equality is not free', () => {
    withConfiguredEngine((engine) => {
      const before = engineStateSnapshot(engine);
      engine.armCapture(false);
      expect(engineStateSnapshot(engine)).not.toBe(before);
      // The same strip rebuilt without its insert loses the automation id,
      // which is what a swallowed bad `sceneJson` used to cause.
      const withInsert = engineStateSnapshot(engine);
      engine.setTrackStripJson(
        TRACK_ID,
        JSON.stringify({
          version: 1,
          strips: [{ id: `track-${TRACK_ID}` }],
          buses: [],
          connections: [],
        }),
      );
      expect(engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb')).toBe(-1);
      expect(engineStateSnapshot(engine)).not.toBe(withInsert);
    });
  });

  for (const { name, badArguments } of CASES) {
    for (const { argument, call, error } of badArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and moves no engine state`, () => {
        withConfiguredEngine((engine) => {
          const before = engineStateSnapshot(engine);
          expect(() => call(engine)).toThrow(error ?? TypeError);
          expect(engineStateSnapshot(engine)).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }

  // Insert bypass has no addon getter, so the snapshot above cannot see it. The
  // audible difference between a bypassed and an active insert can, and the
  // reversal this guards against (`bypassed` read as `false` from a truthy
  // number) is precisely a swap between those two states.
  it('leaves a bypassed insert bypassed when the bypassed flag is wrong-typed', () => {
    const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
    try {
      const frames = BLOCK * 16;
      const source = new Float32Array(frames).map((_, i) =>
        Math.sin((2 * Math.PI * 1000 * i) / SR),
      );
      engine.setClips([
        { id: 1, trackId: TRACK_ID, channels: [source], startPpq: 0, lengthSamples: frames },
      ]);
      engine.setTrackLanes([TRACK_ID]);
      engine.setTrackStripJson(TRACK_ID, trackStripJson(12));
      engine.play();

      // Rewind before each measurement so every settle reads the same stretch
      // of the clip; only the strip state differs between them.
      const settle = (): number => {
        engine.seekSample(0);
        let block: Float32Array<ArrayBufferLike> = new Float32Array(BLOCK);
        for (let i = 0; i < 8; i++) {
          block = engine.process([new Float32Array(BLOCK)])[0];
        }
        return rms(block);
      };

      engine.setTrackStripInsertBypassed(TRACK_ID, 0, false);
      const activeRms = settle();
      engine.setTrackStripInsertBypassed(TRACK_ID, 0, true);
      const bypassedRms = settle();
      // Positive control: if the boost were inaudible the assertion below would
      // hold no matter what the rejected call did.
      expect(activeRms).toBeGreaterThan(0);
      expect(Math.abs(activeRms - bypassedRms)).toBeGreaterThan(0.05 * activeRms);

      expect(() => engine.setTrackStripInsertBypassed(TRACK_ID, 0, 1)).toThrow(TypeError);
      expect(settle()).toBeCloseTo(bypassedRms, 3);
    } finally {
      engine.destroy();
    }
  });
});

describe('rejected transport and MIDI arguments issue no command', () => {
  it('parks the prepared engine off its default transport state', () => {
    // The snapshot has to be able to move in both directions, or comparing it
    // either side of a rejected call would assert nothing. This is that proof.
    withPreparedEngine((engine) => {
      const transport = engine.getTransportState();
      expect(transport.playing).toBe(false);
      expect(transport.samplePosition).toBeGreaterThan(0);
      expect(engine.midiCcBindingCount()).toBeGreaterThan(0);
      expect(engine.midiInputPendingCount()).toBe(0);
    });
  });

  it('holds still across pumping, and moves for a well-typed call', () => {
    withPreparedEngine((engine) => {
      const before = transportSnapshot(engine);
      // A stopped transport does not advance, so "unchanged" below means the
      // command was never issued rather than "not applied yet".
      pump(engine);
      expect(transportSnapshot(engine)).toBe(before);

      engine.pushMidiInputNoteOn(0, 0, 60, 100);
      expect(transportSnapshot(engine)).not.toBe(before);
      const afterNote = transportSnapshot(engine);

      engine.bindMidiCc(0, 8, PARAM_ID, 0, 1);
      expect(transportSnapshot(engine)).not.toBe(afterNote);
      const afterBinding = transportSnapshot(engine);

      engine.play();
      pump(engine);
      expect(transportSnapshot(engine)).not.toBe(afterBinding);
      const afterPlay = transportSnapshot(engine);

      engine.seekSample(0);
      pump(engine);
      expect(transportSnapshot(engine)).not.toBe(afterPlay);
    });
  });

  for (const { name, badTransportArguments } of CASES) {
    for (const { argument, call, error } of badTransportArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and issues no command`, () => {
        withPreparedEngine((engine) => {
          const before = transportSnapshot(engine);
          expect(() => call(engine)).toThrow(error ?? TypeError);
          // Pump first: a command that WAS enqueued would land here, so the
          // comparison distinguishes "never issued" from "not yet applied".
          pump(engine);
          expect(transportSnapshot(engine)).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }
});

describe('rejected project arguments leave the project byte-identical', () => {
  it('parks the fixture project off its defaults, and moves for a well-typed call', () => {
    withConfiguredProject(({ project, trackId }) => {
      const before = project.toJson();
      expect(before).toContain('"gain":0.5');
      project.setTrackGain(trackId, 0.125);
      expect(project.toJson()).not.toBe(before);
      project.setTrackGain(trackId, PROJECT_TRACK_GAIN);
      expect(project.toJson()).toBe(before);
      // The tempo map is the other half a rejected analysis argument used to
      // rewrite; prove it is reachable too.
      project.autoTempo(clickTrack(), PROJECT_SR, 0, false);
      expect(project.toJson()).not.toBe(before);
    });
  });

  for (const { name, badProjectArguments } of CASES) {
    for (const { argument, call, error } of badProjectArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and leaves the project unchanged`, () => {
        withConfiguredProject((fixture) => {
          const before = fixture.project.toJson();
          expect(() => call(fixture)).toThrow(error ?? TypeError);
          expect(fixture.project.toJson()).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }
});

/**
 * Four scans, and between them they define the population this file is
 * responsible for. Stated rather than left to be inferred, because the boundary
 * is the whole question:
 *
 *  1. `positionalReaderDefinitions` — a reader of positional arguments may only
 *     be DEFINED in the shared header. Closes "someone writes another
 *     `Uint32Arg`".
 *  2. `bailoutReaderCalls` — every call to that family must be consumed as
 *     `if (!Reader(...)) return`. Closes "someone calls it and ignores false".
 *  3. `positionalArgEntryPoints` — every entry point reaching that family must
 *     be driven by CASES or registered. Closes "a new entry point uses the
 *     family correctly but is never tested".
 *  4. `inlineTypedArgumentReads` — no entry point may read a positional
 *     argument with `info[i].As<Napi::X>().Value()` unless it type-checks that
 *     index. Closes the gap the first three share: all of them are anchored on
 *     the shared family, so all three are blind to code that simply never uses
 *     it. That is exactly how this class began.
 *
 * What is still OUTSIDE the population, deliberately: the lenient `node_arg_*`
 * family (type-checks and falls back to a default, so it never leaves a pending
 * exception and never hands the C ABI a dummy alongside one), and `.As<Napi::T>()`
 * with no value accessor (an unchecked cast that cannot itself fail). An author
 * can still leave the population by using `node_arg_*`; that is a lenience
 * decision, not an unguarded read, and it is visible in review as one.
 */
describe('the abort-guard table accounts for every rejecting entry point', () => {
  it('self-checks the source scanners, so the registers are not comparing nothing', () => {
    // Every assertion below is a set difference. If a scanner stopped matching,
    // both sides would empty and every one of them would pass vacuously.
    expect(positionalReaderDefinitions().length).toBeGreaterThan(15);
    expect(bailoutReaderCalls().length).toBeGreaterThan(80);
    expect(positionalArgEntryPoints().length).toBeGreaterThan(80);
    // This one reports its whole population, not just its violations, so the
    // floor is what proves a clean sweep swept something. What remains is the
    // non-integer half: every integer read moved onto the shared narrowing
    // family, either as a hand-written node_arg_int copy folded back onto the
    // reader (outside this population by design, since that family is the
    // lenience decision) or as a bare info[i] read routed through
    // node_narrow_int, which is a call rather than an inline accessor.
    // 100 reads match. The floor sits well under that on purpose: routing an
    // argument onto a reader shrinks this population without touching the
    // violation subset it guards, so such a move must not redden it.
    expect(inlineTypedArgumentReads().length).toBeGreaterThan(80);
    // The floor alone would not notice the implicit-conversion form being
    // dropped again, so pin that form where it is concentrated. Measured on
    // addon.cpp's MIDI lookups: 15 reads match and every one of them is
    // accessor-less, so a scanner that saw only the explicit form would find
    // none. A floor between the two separates them.
    const midiReads = inlineTypedArgumentReads().filter(
      (site) => site.file === 'addon.cpp' && site.name.startsWith('Midi'),
    );
    expect(midiReads.length).toBeGreaterThan(10);
    // Positive control for the bail-out detector: it has to answer "no" to the
    // shape this whole family exists to prevent, or a clean sweep means nothing.
    expect(isBailoutGuarded('  if (!')).toBe(true);
    expect(isBailoutGuarded('  if (!sonare_node::')).toBe(true);
    expect(isBailoutGuarded('      !')).toBe(false);
    expect(isBailoutGuarded('  ThrowIfError(env, sonare_project_set_track_gain(project_, ')).toBe(
      false,
    );
  });

  it('defines no positional-argument reader outside the shared header', () => {
    const unlisted = positionalReaderDefinitions().filter(
      (site) => site.file !== SHARED_READER_FILE && !POSITIONAL_READER_ALLOWLIST.has(site.id),
    );
    expect(
      unlisted.map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      `A reader that takes (Napi::CallbackInfo, index, ...) belongs in ${SHARED_READER_FILE}. ` +
        'Use the Optional*Arg / Required*Arg family there, extend it if the shape you need is ' +
        'missing, or add the site to POSITIONAL_READER_ALLOWLIST with the reason the shared ' +
        'family cannot express it. A file-local copy reads the argument without a pending-' +
        'exception guard and without a bail-out, which is how a wrong-typed argument reached ' +
        'the C ABI as a dummy value and how a second throw aborted the process.',
    ).toEqual([]);
  });

  it('keeps the positional-reader allowlist free of entries that no longer exist', () => {
    const live = new Set(positionalReaderDefinitions().map((site) => site.id));
    expect([...POSITIONAL_READER_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('consumes every bail-out reader call as a bail-out', () => {
    const unguarded = bailoutReaderCalls().filter((site) => !site.guarded);
    expect(
      unguarded.map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      'A bail-out reader returns false without touching its out-parameter, so ignoring that ' +
        'return leaves the caller running on an unread argument with an exception already ' +
        'pending. Every call must read `if (!Reader(...))` and return immediately.',
    ).toEqual([]);
  });

  it('type-checks every inline read of a positional argument', () => {
    const unguarded = inlineTypedArgumentReads().filter(
      (site) => !site.typeChecked && !INLINE_READ_ALLOWLIST.has(site.id),
    );
    expect(
      unguarded.map((site) => `${site.file}:${site.line} ${site.name}`),
      'An `info[i].As<Napi::X>().Value()` with no `info[i].IsX()` check leaves a pending ' +
        'exception and a dummy value when the argument is the wrong type, and the C-ABI call ' +
        'built from it then runs. Read it through the Optional*Arg / Required*Arg family in ' +
        `${SHARED_READER_FILE} and bail out, or type-check the index first. Note that ` +
        'IsUndefined()/IsNull() do not count: every defect in this class was presence-checked ' +
        'and type-blind.',
    ).toEqual([]);
  });

  it('keeps the inline-read allowlist free of entries that no longer exist', () => {
    const live = new Set(
      inlineTypedArgumentReads()
        .filter((site) => !site.typeChecked)
        .map((site) => site.id),
    );
    expect([...INLINE_READ_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('holds the GM lookup suppression to the population it was measured against', () => {
    // The entries excuse a set of reads that were each driven with a wrong-typed
    // argument and with none, and each answered one catchable TypeError. They do
    // not excuse addon.cpp. Without this the written expiry condition has no
    // reader, and an eleventh read added to the file would inherit the blessing
    // unexamined — which is the failure mode of every stale allowlist entry.
    const unguarded = inlineTypedArgumentReads().filter(
      (site) => !site.typeChecked && site.file === 'addon.cpp' && site.name.startsWith('Midi'),
    );
    const functions = new Set(unguarded.map((site) => site.name));
    expect(
      { reads: unguarded.length, functions: functions.size },
      'The measured population behind GM_NAME_LOOKUP_REASON has changed. Re-drive the affected ' +
        'body with a wrong-typed argument and with none: if it still answers ONE catchable ' +
        'TypeError, update GM_NAME_LOOKUP_POPULATION with the new numbers and say so; if a second ' +
        'failing read now makes it abort, move the body onto the shared reader family and delete ' +
        'its allowlist entry instead of widening this.',
    ).toEqual(GM_NAME_LOOKUP_POPULATION);
  });

  it('accounts for every entry point that can reject a positional argument', () => {
    const covered = new Set(CASES.map((entry) => entry.name.split('.').pop()));
    const unaccounted = positionalArgEntryPoints().filter(
      (jsName) => !covered.has(jsName) && !UNCOVERED_POSITIONAL_GUARDS.has(jsName),
    );
    expect(
      unaccounted,
      'A new addon entry point that reads a positional argument through the bail-out family ' +
        'must either be driven by CASES or listed in UNCOVERED_POSITIONAL_GUARDS with a reason. ' +
        'Four generations of this finding were each closed by enumerating the entry points that ' +
        'existed at the time, and each time a new same-shaped one arrived that no table named.',
    ).toEqual([]);
  });

  it('keeps the uncovered register free of stale names', () => {
    const live = new Set(positionalArgEntryPoints());
    expect([...UNCOVERED_POSITIONAL_GUARDS.keys()].filter((name) => !live.has(name))).toEqual([]);
  });
});

const DRAINS = [
  'drainTelemetry',
  'drainMeterTelemetry',
  'drainMeterTelemetryWide',
  'drainScopeTelemetry',
] as const;

describe('telemetry drains bound their allocation by a validated budget', () => {
  for (const drain of DRAINS) {
    it(`${drain}: rejects a negative maxRecords with a RangeError`, () => {
      expect(() => withEngine((engine) => engine[drain](-1))).toThrow(RangeError);
      expectNativeStillWorks();
    });

    it(`${drain}: rejects a non-integer maxRecords with a RangeError`, () => {
      expect(() => withEngine((engine) => engine[drain](1.5))).toThrow(RangeError);
      expect(() => withEngine((engine) => engine[drain](Number.NaN))).toThrow(RangeError);
      expect(() => withEngine((engine) => engine[drain](Number.POSITIVE_INFINITY))).toThrow(
        RangeError,
      );
      expectNativeStillWorks();
    });

    it(`${drain}: rejects a non-number maxRecords with a TypeError`, () => {
      expect(() => withEngine((engine) => engine[drain]('1024'))).toThrow(TypeError);
      expectNativeStillWorks();
    });

    it(`${drain}: returns an empty array for maxRecords 0`, () => {
      expect(withEngine((engine) => engine[drain](0))).toEqual([]);
    });

    it(`${drain}: survives a 2**31 maxRecords without a proportional allocation`, () => {
      const before = process.memoryUsage().rss;
      const drained = withEngine((engine) => engine[drain](2 ** 31));
      const grew = process.memoryUsage().rss - before;
      expect(Array.isArray(drained)).toBe(true);
      // A budget-sized buffer would be 2**31 records: hundreds of gigabytes for
      // the wide meter record, which is what used to kill the process. The
      // working buffer is capped at a fixed chunk instead, so nothing near the
      // budget is reserved.
      expect(grew).toBeLessThan(64 * 1024 * 1024);
      expectNativeStillWorks();
    });

    it(`${drain}: treats an omitted maxRecords as the default budget`, () => {
      expect(withEngine((engine) => engine[drain]())).toEqual([]);
      expect(withEngine((engine) => engine[drain](undefined))).toEqual([]);
    });
  }

  it('drainTelemetry honours the budget exactly and drains past one chunk', () => {
    withEngine((engine) => {
      const left = new Float32Array(BLOCK);
      const right = new Float32Array(BLOCK);
      // More than the 256-record internal chunk, so a huge budget has to loop.
      const blocks = 600;
      for (let i = 0; i < blocks; i++) {
        engine.process([left, right]);
      }
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(blocks);

      for (let i = 0; i < blocks; i++) {
        engine.process([left, right]);
      }
      expect(engine.drainTelemetry(1)).toHaveLength(1);
      expect(engine.drainTelemetry(5)).toHaveLength(5);
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(blocks - 6);
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(0);
    });
  });
});

describe('the hostile-input matrix leaves the process alive', () => {
  it('detects an aborting child, so the exit-code assertion is not blind', () => {
    const result = spawnSync(process.execPath, ['-e', 'process.abort()'], { encoding: 'utf8' });
    expect(result.status === 134 || result.signal === 'SIGABRT').toBe(true);
  });

  it('exits 0 in a child process (not 134)', () => {
    // vitest reports a dead worker, but only as an opaque failure. Asserting a
    // real exit code is what distinguishes "threw and recovered" from "aborted".
    const script = `
      const addon = require(${JSON.stringify(new URL('../build/Release/sonare-node.node', import.meta.url).pathname)});
      const swallow = (fn) => { try { fn(); } catch { /* a catchable error is the point */ } };
      for (let round = 0; round < 2; round++) {
        // Constructors first: each of these terminated the process before the
        // constructors carried a catch harness, so they are the rows this
        // script's exit code is most worth spending.
        swallow(() => new addon.PolyphonicAnalysis(new Float32Array(1024), 22050, { nFft: 2 ** 32 }));
        swallow(() => new addon.PolyphonicAnalysis(new Float32Array(1024), 22050, { nFft: 0.5 }));
        swallow(() => new addon.RealtimeEngine(${SR}, 2 ** 32));
        swallow(() => new addon.RealtimeEngine(${SR}, ${BLOCK}, 2 ** 63));
        swallow(() => new addon.Mixer('{"tracks":[]}', 2 ** 32, 512));
        swallow(() => new addon.Mixer('{"tracks":[]}', ${SR}, 2 ** 32));
        swallow(() => new addon.StreamingRetune({ grainSize: 2 ** 32 }));
        swallow(() => new addon.StreamingRetune({ grain_size: 2 ** 32 }));
        swallow(() => new addon.StreamingRetune({ semitones: 1e40 }));
        const e = new addon.RealtimeEngine(${SR}, ${BLOCK});
        swallow(() => e.setGraph({ nodes: [{}, {}], connections: [{}, {}] }));
        swallow(() => e.setGraph({ nodes: [{ id: {} }, { id: {} }], connections: [], inputNode: {}, outputNode: {} }));
        swallow(() => e.setClips([{}, {}]));
        swallow(() => e.setTrackLanes([{}, {}]));
        swallow(() => e.setTrackBuses([{}, {}]));
        swallow(() => e.setTempoSegments([{}, {}]));
        swallow(() => e.setTimeSignatureSegments([{}, {}]));
        for (const drain of ${JSON.stringify(DRAINS)}) {
          for (const arg of [-1, -2147483648, 1.5, Number.NaN, Number.POSITIVE_INFINITY, 'x', 0, 2 ** 31, 2 ** 53]) {
            swallow(() => e[drain](arg));
          }
        }
        swallow(() => e.setLaneSidechain(10, 0, '1'));
        swallow(() => e.setBusStripJson(1, 42));
        swallow(() => e.setTrackStripJson(10, 42));
        swallow(() => e.setTrackStripEqBandJson(10, '0', 42));
        swallow(() => e.setTrackStripInsertBypassed(10, 0, 1, 1));
        swallow(() => e.setMasterStripJson(42));
        swallow(() => e.setMasterStripEqBandJson('0', 42));
        swallow(() => e.setMasterStripInsertBypassed(0, 1, 1));
        swallow(() => e.setTrackStripInsertParamByName(10, 0, 42, '1'));
        swallow(() => e.setMasterStripInsertParamByName(0, 42, '1'));
        swallow(() => e.setBusStripInsertParamByName(1, 0, 42, '1'));
        swallow(() => e.setBusStripInsertBypassed(1, 0, 1, 1));
        swallow(() => e.resolveTrackInsertAutomationId(10, '0', 42));
        swallow(() => e.resolveMasterInsertAutomationId('0', 42));
        swallow(() => e.resolveBusInsertAutomationId(1, '0', 42));
        swallow(() => e.resolveInstrumentAutomationId('1', 42));
        swallow(() => e.setTrackStripPan(10, '0.5'));
        swallow(() => e.setTrackStripPanLaw(10, '3'));
        swallow(() => e.setTrackStripPanMode(10, '2'));
        swallow(() => e.setTrackStripDualPan(10, '0.5', '0.5'));
        swallow(() => e.setTrackStripChannelDelaySamples(10, '64'));
        swallow(() => e.createClipPageProvider('1', '1024', '256'));
        swallow(() => e.supplyClipPage('1', '0', 'x'));
        swallow(() => e.clearClipPage('1', '0'));
        swallow(() => e.destroyClipPageProvider('1'));
        swallow(() => e.setClipPagePrefetchFrames('4096'));
        swallow(() => e.armCapture(1));
        swallow(() => e.setCapturePunch('0', '128', 1));
        swallow(() => e.setRecordOffsetSamples('64'));
        swallow(() => e.setInputMonitor(1, '0.5'));
        swallow(() => e.setCaptureBuffer('2', '1024'));
        // Transport, parameter and MIDI arguments. The MIDI rows use two
        // out-of-byte-range values in a row on purpose: the second reader's
        // throw landing on the first one's pending exception is the abort.
        e.prepare(${SR}, ${BLOCK});
        swallow(() => e.prepare('48000', ${BLOCK}));
        swallow(() => e.play('now'));
        swallow(() => e.stop('now'));
        swallow(() => e.seekSample('x', 'y'));
        swallow(() => e.seekPpq(0, 'x'));
        swallow(() => e.seekMarker('0', 'x'));
        swallow(() => e.countInEndSample('0', '1'));
        swallow(() => e.setParameter(1, 0.5, 'now'));
        swallow(() => e.setParameterSmoothed(1, 0.5, 'now'));
        swallow(() => e.setSoloMute(0, true, false, 'now'));
        swallow(() => e.setTrackMonitorMode(0, 1, 'now'));
        swallow(() => e.bindMidiCc(300, 300, 1, 0, 1));
        swallow(() => e.bindMidiCc('0', 'x', 1, 0, 1));
        swallow(() => e.pushMidiNoteOn(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiNoteOff(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiCc(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputNoteOn(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputNoteOff(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputCc(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiPanic('now'));
        swallow(() => e.pushMidiSysex(0, new Uint8Array([0xf0, 0xf7]), 'now'));
        swallow(() => e.renderOffline([new Float32Array(${BLOCK})], '128', 'yes'));
        e.destroy();
        const p = new addon.Project();
        swallow(() => p.setTempoSegments([{}, {}]));
        swallow(() => p.setTimeSignatures([{}, {}]));
        swallow(() => p.setWarpMap({ anchors: [{}, {}] }));
        const { trackId, clipId } = p.addMidiClip(0, 4);
        swallow(() => p.setMidiEvents(clipId, [{}, {}]));
        swallow(() => p.setMidiEvents(clipId, [[], []]));
        swallow(() => p.addAutomationLane(trackId, { targetParamId: 1, points: [{}, {}] }));
        swallow(() => p.editAutomationLane(trackId, 1, { targetParamId: 1, points: [{}, {}] }));
        // Positional project arguments, every one of them a string.
        swallow(() => p.addTrack('audio'));
        swallow(() => p.addMidiClip('0', '4'));
        swallow(() => p.setSampleRate('48000'));
        swallow(() => p.setOverlapPolicy('0'));
        swallow(() => p.setMarker('0', 'x', 'v'));
        swallow(() => p.markerByIndex('0'));
        swallow(() => p.trackByIndex('0'));
        swallow(() => p.clipByIndex('0'));
        swallow(() => p.sourceByIndex('0'));
        swallow(() => p.tempoSegmentByIndex('0'));
        swallow(() => p.timeSignatureByIndex('0'));
        swallow(() => p.splitClip('1', 'x'));
        swallow(() => p.trimClip('1', 'x', 'y'));
        swallow(() => p.moveClip('1', 'x', 'y'));
        swallow(() => p.duplicateClip('1', 'x'));
        swallow(() => p.removeClip('1'));
        swallow(() => p.removeTrack('1'));
        swallow(() => p.renameTrack('1', 'renamed'));
        swallow(() => p.setTrackRoute('1', 'a', 'b'));
        swallow(() => p.setTrackKind('1', 'midi'));
        swallow(() => p.setTrackGain('1', 'loud'));
        swallow(() => p.setTrackPan('1', 'left'));
        swallow(() => p.setTrackMute('1', true));
        swallow(() => p.setTrackSolo('1', true));
        swallow(() => p.setTrackMidiDestination('1', 'x'));
        swallow(() => p.setClipGain('1', 'loud'));
        swallow(() => p.setClipFade('1', {}, {}));
        swallow(() => p.setClipLoop('1', 'x', 'y', 'z'));
        swallow(() => p.setClipSource('1', 'x'));
        swallow(() => p.setClipTakes('1', [], 'x'));
        swallow(() => p.setClipCompSegments('1', []));
        swallow(() => p.setClipWarpRef('1', 'x'));
        swallow(() => p.setClipWarpMode('1', 'tempo-sync'));
        swallow(() => p.removeWarpMap('1'));
        swallow(() => p.setSourceAudio('1', new Float32Array(4), '1', '48000'));
        swallow(() => p.setAudioSourceMetadata('1', 'h', 'r'));
        swallow(() => p.setMidiEvents('1', []));
        swallow(() => p.setProgram('1', 'piano', 'x'));
        swallow(() => p.setProgramOnChannel('1', 300, 300, 'piano', 'x'));
        swallow(() => p.bakeMidiFx('1', '{}'));
        swallow(() => p.bakeMidiFxWithSourceIndex('1', '{}'));
        swallow(() => p.previewMidiFxCount('1', '{}'));
        swallow(() => p.validateMidiNotes('1'));
        swallow(() => p.addAutomationLane('bad', 'bad2'));
        swallow(() => p.editAutomationLane('bad', 'bad2', 'not-an-object'));
        swallow(() => p.removeAutomationLane('bad', 'bad2'));
        swallow(() => p.getAssistSidecar('0'));
        swallow(() => p.setMaxUndoDepth('8'));
        swallow(() => p.setMaxHistoryBytes('1024'));
        swallow(() => p.snapToGrid('1', 'x', 'y'));
        swallow(() => p.autoTempo(new Float32Array(4096), '48000', '0', false));
        swallow(() => p.analyzeTempo(new Float32Array(4096), '48000'));
        p.destroy();
        const bank = new addon.SampleBank();
        swallow(() => bank.addSample('not-audio', {}));
        swallow(() => bank.addSample(new Float32Array(8), { rootKey: 300, loopStart: 'x' }));
        swallow(() => bank.addSample(new Float32Array(8), { loopMode: {} }));
        swallow(() => bank.addZone('0', {}));
        swallow(() => bank.addZone(0, { keyLo: 300, keyHi: 300, velLo: 300, velHi: 300 }));
        swallow(() => bank.addZone(0, 'not-an-object'));
        bank.destroy();
      }
      process.exit(0);
    `;
    const result = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
    expect(
      { status: result.status, signal: result.signal },
      `child stderr:\n${result.stderr}`,
    ).toEqual({ status: 0, signal: null });
  });
});
