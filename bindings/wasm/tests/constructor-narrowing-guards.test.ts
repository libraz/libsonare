/**
 * What a JS number is allowed to become on its way into an embind CONSTRUCTOR.
 *
 * embind registers a parameter by its declared C++ type, so a JS number bound
 * for a wasm `i32` is converted by ToInt32, which WRAPS: `2**32 + 5` arrives as
 * `5`, `2**32` and `NaN` and `Infinity` all arrive as `0`, and `1.5` arrives as
 * `1`. A wrapped argument is inside the legal domain by construction, so no
 * downstream range check can separate it from a value the caller meant -- an
 * engine sized for 5-frame blocks, a command queue taking the `0` that means
 * "use the default", a Hann window nobody asked for. The narrow integer
 * positions of these constructors are therefore registered as `const val&` and
 * narrowed by the shared checked reader.
 *
 * The value shapes are chosen for what they wrap ONTO rather than for being
 * extreme: `2**32 + 5` discriminates because it lands on a different LEGAL
 * value, which `Number.MAX_SAFE_INTEGER` does not. `-1` covers the other end,
 * where the reader passes a representable int and the field's own domain guard
 * is what must refuse it; the two refusals read differently and this file keeps
 * them apart.
 *
 * Every refusal is paired with the legal value beside it, and each entry point
 * carries a control that reads back which value was selected -- a constructor
 * that ignored its argument would refuse nothing and accept nothing in exactly
 * the same way.
 *
 * These drive the embind classes off the module rather than the TS facades.
 * `RealtimeEngine`'s facade always passes five arguments, so the four-argument
 * overload is unreachable through it, and `StreamAnalyzer`'s facade resolves
 * `window` and `outputFormat` client-side, so a test through it would measure
 * the facade rather than the binding.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, type SonareError } from '../src/index';
import { getSonareModule } from '../src/module_state';

beforeAll(async () => {
  await init();
});

function refusalFor(fn: () => unknown): SonareError {
  let caught: unknown;
  try {
    fn();
  } catch (e) {
    caught = e;
  }
  expect(caught, 'expected a SonareError, got no throw').toBeDefined();
  expect(isSonareError(caught)).toBe(true);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
  return caught as SonareError;
}

/** The two refusals the shared checked reader can produce, verbatim. */
const READER_RANGE = 'must be a finite number within the 32-bit integer range';
const READER_FRACTIONAL = 'must be an integer';

/**
 * The value shapes, with the legal value ToInt32 folded each one onto at a
 * narrow parameter. `wrappedOnto` is what the guard exists to stop being
 * silently selected; it is not read by the assertions.
 */
const READER_REFUSED = [
  { label: '2**32 + 5', value: 2 ** 32 + 5, wrappedOnto: 5, reason: READER_RANGE },
  { label: '2**32', value: 2 ** 32, wrappedOnto: 0, reason: READER_RANGE },
  { label: '2**53 + 1', value: 2 ** 53 + 1, wrappedOnto: 0, reason: READER_RANGE },
  { label: 'NaN', value: Number.NaN, wrappedOnto: 0, reason: READER_RANGE },
  { label: 'Infinity', value: Number.POSITIVE_INFINITY, wrappedOnto: 0, reason: READER_RANGE },
  { label: '1.5', value: 1.5, wrappedOnto: 1, reason: READER_FRACTIONAL },
] as const;

type NativeHandle = { delete: () => void };

const ENGINE_BASE = {
  sampleRate: 48000,
  maxBlockSize: 128,
  commandCapacity: 1024,
  telemetryCapacity: 1024,
  maxChannels: 2,
};

type EngineOverrides = Partial<Record<keyof typeof ENGINE_BASE, number>>;

function newEngine4(overrides: EngineOverrides = {}) {
  const c = { ...ENGINE_BASE, ...overrides };
  return new (getSonareModule().RealtimeEngine)(
    c.sampleRate,
    c.maxBlockSize,
    c.commandCapacity,
    c.telemetryCapacity,
  );
}

function newEngine5(overrides: EngineOverrides = {}) {
  const c = { ...ENGINE_BASE, ...overrides };
  return new (getSonareModule().RealtimeEngine)(
    c.sampleRate,
    c.maxBlockSize,
    c.commandCapacity,
    c.telemetryCapacity,
    c.maxChannels,
  );
}

const ANALYZER_BASE = {
  sampleRate: 22050,
  nFft: 256,
  hopLength: 128,
  nMels: 16,
  fmin: 0,
  fmax: 0,
  tuningRefHz: 440,
  emitEveryNFrames: 1,
  magnitudeDownsample: 1,
  maxPendingFrames: 4096,
  maxProgressionEntries: 4096,
  keyUpdateIntervalSec: 5,
  bpmUpdateIntervalSec: 10,
  window: 0,
  outputFormat: 0,
};

type AnalyzerOverrides = Partial<Record<keyof typeof ANALYZER_BASE, number>>;

function newAnalyzer(overrides: AnalyzerOverrides = {}) {
  const c = { ...ANALYZER_BASE, ...overrides };
  return new (getSonareModule().StreamAnalyzer)(
    c.sampleRate,
    c.nFft,
    c.hopLength,
    c.nMels,
    c.fmin,
    c.fmax,
    c.tuningRefHz,
    false,
    true,
    false,
    false,
    false,
    c.emitEveryNFrames,
    c.magnitudeDownsample,
    c.maxPendingFrames,
    c.maxProgressionEntries,
    c.keyUpdateIntervalSec,
    c.bpmUpdateIntervalSec,
    c.window,
    c.outputFormat,
  );
}

/**
 * One narrowed constructor position. `legal` is a value the field accepts that
 * differs from the base, and `domainRefusal` is the wording the field's own
 * guard uses for -1 -- a representable int the reader passes on.
 */
interface NarrowedSlot {
  entry: string;
  field: string;
  legal: number;
  domainRefusal: string;
  construct: (value: number) => NativeHandle;
}

const NARROWED_SLOTS: NarrowedSlot[] = [
  {
    entry: 'RealtimeEngine/4',
    field: 'maxBlockSize',
    legal: 256,
    domainRefusal: 'max_block_size must be positive',
    construct: (maxBlockSize) => newEngine4({ maxBlockSize }),
  },
  {
    entry: 'RealtimeEngine/4',
    field: 'commandCapacity',
    legal: 512,
    domainRefusal: 'command_capacity and telemetry_capacity must be 0',
    construct: (commandCapacity) => newEngine4({ commandCapacity }),
  },
  {
    entry: 'RealtimeEngine/4',
    field: 'telemetryCapacity',
    legal: 512,
    domainRefusal: 'command_capacity and telemetry_capacity must be 0',
    construct: (telemetryCapacity) => newEngine4({ telemetryCapacity }),
  },
  {
    entry: 'RealtimeEngine/5',
    field: 'maxBlockSize',
    legal: 256,
    domainRefusal: 'max_block_size must be positive',
    construct: (maxBlockSize) => newEngine5({ maxBlockSize }),
  },
  {
    entry: 'RealtimeEngine/5',
    field: 'commandCapacity',
    legal: 512,
    domainRefusal: 'command_capacity and telemetry_capacity must be 0',
    construct: (commandCapacity) => newEngine5({ commandCapacity }),
  },
  {
    entry: 'RealtimeEngine/5',
    field: 'telemetryCapacity',
    legal: 512,
    domainRefusal: 'command_capacity and telemetry_capacity must be 0',
    construct: (telemetryCapacity) => newEngine5({ telemetryCapacity }),
  },
  {
    entry: 'RealtimeEngine/5',
    field: 'maxChannels',
    legal: 4,
    domainRefusal: 'max_channels must be within 1..',
    construct: (maxChannels) => newEngine5({ maxChannels }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'sampleRate',
    legal: 44100,
    domainRefusal: 'sample_rate must be positive',
    construct: (sampleRate) => newAnalyzer({ sampleRate }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'nFft',
    legal: 512,
    domainRefusal: 'n_fft must be positive',
    construct: (nFft) => newAnalyzer({ nFft }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'hopLength',
    legal: 64,
    domainRefusal: 'hop_length must be positive',
    construct: (hopLength) => newAnalyzer({ hopLength }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'nMels',
    legal: 32,
    domainRefusal: 'n_mels must be positive',
    construct: (nMels) => newAnalyzer({ nMels }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'emitEveryNFrames',
    legal: 2,
    domainRefusal: 'emit_every_n_frames must be positive',
    construct: (emitEveryNFrames) => newAnalyzer({ emitEveryNFrames }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'magnitudeDownsample',
    legal: 2,
    domainRefusal: 'magnitude_downsample must be positive',
    construct: (magnitudeDownsample) => newAnalyzer({ magnitudeDownsample }),
  },
  {
    entry: 'StreamAnalyzer',
    field: 'window',
    legal: 1,
    domainRefusal: 'window is out of range',
    construct: (window) => newAnalyzer({ window }),
  },
  {
    // Float32 is the only accepted format, so its legal value is the base one;
    // the pairing here is between 0 and everything a wrap used to turn into 0.
    entry: 'StreamAnalyzer',
    field: 'outputFormat',
    legal: 0,
    domainRefusal: 'outputFormat is deprecated',
    construct: (outputFormat) => newAnalyzer({ outputFormat }),
  },
];

describe('the i32 boundary conversion these constructors guard against', () => {
  it('folds a JS number onto a different legal i32, on this runtime', () => {
    // The premise the refusals below are written against, measured rather than
    // asserted: a hand-built module whose only export takes an i32 and returns
    // it. Nothing in libsonare is involved, which is the point -- the wrap is a
    // property of the JS/wasm boundary, so it applied to every narrow parameter
    // these constructors used to declare.
    const identityI32 = new Uint8Array([
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01,
      0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x06, 0x01, 0x02, 0x69, 0x64, 0x00, 0x00, 0x0a, 0x06,
      0x01, 0x04, 0x00, 0x20, 0x00, 0x0b,
    ]);
    const instance = new WebAssembly.Instance(new WebAssembly.Module(identityI32), {});
    const id = instance.exports.id as (value: number) => number;

    // The control: a value inside the range is itself, so the folds below are
    // the conversion rather than a function that answers 0 to everything.
    expect(id(128)).toBe(128);
    expect(id(-1)).toBe(-1);
    for (const shape of READER_REFUSED) {
      expect(id(shape.value), `${shape.label} at an i32 parameter`).toBe(shape.wrappedOnto);
    }
  });
});

describe('a narrowed constructor position refuses what a wrap made legal', () => {
  for (const slot of NARROWED_SLOTS) {
    it(`${slot.entry} names ${slot.field} in every reader refusal`, () => {
      for (const shape of READER_REFUSED) {
        const error = refusalFor(() => slot.construct(shape.value));
        expect(error.message, `${slot.field} = ${shape.label}`).toContain(slot.field);
        expect(error.message, `${slot.field} = ${shape.label}`).toContain(shape.reason);
      }
    });

    it(`${slot.entry} lets the ${slot.field} domain guard answer -1`, () => {
      // -1 is representable, so the reader passes it on. The field's own guard
      // is what must refuse it, and seeing the reader's wording here would mean
      // the narrowing check had grown into a domain check it does not own.
      const error = refusalFor(() => slot.construct(-1));
      expect(error.message).toContain(slot.domainRefusal);
      expect(error.message).not.toContain(READER_RANGE);
      expect(error.message).not.toContain(READER_FRACTIONAL);
    });

    it(`${slot.entry} still constructs for a legal ${slot.field}`, () => {
      // The non-vacuity half: without it a constructor that had started
      // refusing every value would satisfy both assertions above.
      const handle = slot.construct(slot.legal);
      try {
        expect(handle).toBeDefined();
      } finally {
        handle.delete();
      }
    });
  }
});

describe('each entry point reads back which value it was constructed with', () => {
  /** kMaxBlockExceeded, engine/telemetry.h. */
  const MAX_BLOCK_EXCEEDED = 6;

  it('RealtimeEngine/4 sizes the render block from its second argument', () => {
    // A block wider than the engine was built for is silenced and reported
    // through telemetry, so the two engines answer the same 256-frame call
    // differently and the difference is attributable to that argument alone.
    const exceeded = (maxBlockSize: number): number[] => {
      const engine = newEngine4({ maxBlockSize });
      try {
        engine.process([new Float32Array(256)]);
        return engine
          .drainTelemetry(16)
          .filter((record) => record.error === MAX_BLOCK_EXCEEDED)
          .map((record) => record.value);
      } finally {
        engine.delete();
      }
    };
    expect(exceeded(64)).not.toHaveLength(0);
    expect(exceeded(256)).toHaveLength(0);
  });

  it('RealtimeEngine/5 prepares the channel count from its fifth argument', () => {
    const renderWidth = (maxChannels: number): number => {
      const engine = newEngine5({ maxChannels });
      try {
        const planes = Array.from({ length: 6 }, () => new Float32Array(128));
        return engine.renderOffline(planes, 128, false).length;
      } finally {
        engine.delete();
      }
    };
    expect(renderWidth(6)).toBe(6);
    // Two channels cannot answer a six-plane render, which is what makes the
    // acceptance above a measurement of the argument rather than of its absence.
    refusalFor(() => renderWidth(2));
  });

  it('StreamAnalyzer keeps the sample rate, mel count and hop it was given', () => {
    const inspect = (overrides: AnalyzerOverrides) => {
      const analyzer = newAnalyzer(overrides);
      try {
        analyzer.process(new Float32Array(2048));
        return {
          sampleRate: analyzer.sampleRate(),
          frames: analyzer.frameCount(),
          nMels: analyzer.readFramesSoa(1).nMels,
        };
      } finally {
        analyzer.delete();
      }
    };

    const base = inspect({});
    expect(base.sampleRate).toBe(22050);
    expect(inspect({ sampleRate: 44100 }).sampleRate).toBe(44100);
    expect(base.nMels).toBe(16);
    expect(inspect({ nMels: 32 }).nMels).toBe(32);
    // Halving the hop doubles the frames the same input produces.
    expect(base.frames).toBeGreaterThan(0);
    expect(inspect({ hopLength: 64 }).frames).toBeGreaterThan(base.frames);
  });
});

describe('the untouched neighbours of the narrowed positions are unchanged', () => {
  // maxPendingFrames is a double with a guard of its own and may never start
  // reporting the checked reader's refusals. fmin is a float, read through the
  // checked FLOAT reader, which asks a narrower question than the integer one:
  // it refuses only what no float can carry. Between them they show the refusals
  // above come from the converted integer positions rather than from something
  // upstream that had begun rejecting every constructor argument.
  const NEIGHBOUR_VALUES: ReadonlyArray<{ label: string; value: number }> = [
    ...READER_REFUSED.map(({ label, value }) => ({ label, value })),
    { label: '-1', value: -1 },
  ];

  /**
   * What each shape does at `fmin`, a `float` position. The magnitudes every
   * narrowed integer position refuses are ACCEPTED into the float and fail
   * later, unnamed, in the mel filterbank; a fractional value is simply a legal
   * frequency. A float carries neither a NaN nor an infinity into a frequency,
   * so those two the reader answers by name; everything else a float can hold
   * it passes on, and the domain guard behind it decides.
   */
  const FMIN_OUTCOME: ReadonlyArray<readonly [string, string]> = [
    // A float holds these, so the refusal comes from deep in the build and
    // names no field -- the contrast that shows what the narrowed positions buy.
    ['2**32 + 5', 'Invalid parameter'],
    ['2**32', 'Invalid parameter'],
    ['2**53 + 1', 'Invalid parameter'],
    ['NaN', 'fmin must be a finite number'],
    ['Infinity', 'fmin must be a finite number'],
    ['1.5', 'constructs'],
    ['-1', 'StreamConfig: fmin/fmax must be finite and non-negative'],
  ];

  it('fmin answers each shape as a float position, not as a narrowed one', () => {
    for (const shape of NEIGHBOUR_VALUES) {
      const expected = FMIN_OUTCOME.find(([label]) => label === shape.label)?.[1];
      expect(expected, `no expectation recorded for fmin = ${shape.label}`).toBeDefined();
      if (expected === 'constructs') {
        const analyzer = newAnalyzer({ fmin: shape.value });
        try {
          expect(analyzer.sampleRate()).toBe(ANALYZER_BASE.sampleRate);
        } finally {
          analyzer.delete();
        }
        continue;
      }
      const error = refusalFor(() => newAnalyzer({ fmin: shape.value }));
      expect(error.message, `fmin = ${shape.label}`).toContain(expected);
      expect(error.message, `fmin = ${shape.label}`).not.toContain(READER_RANGE);
      expect(error.message, `fmin = ${shape.label}`).not.toContain(READER_FRACTIONAL);
    }
  });

  it('takes 1.5 at fmin and refuses it one position over', () => {
    // The sharpest form of the same claim: one value, one constructor, two
    // adjacent positions, and only the narrowed one refuses.
    const analyzer = newAnalyzer({ fmin: 1.5 });
    analyzer.delete();
    expect(refusalFor(() => newAnalyzer({ nMels: 1.5 })).message).toContain(
      `nMels ${READER_FRACTIONAL}`,
    );
  });

  it('maxPendingFrames keeps its own refusal wording', () => {
    for (const shape of NEIGHBOUR_VALUES) {
      const error = refusalFor(() => newAnalyzer({ maxPendingFrames: shape.value }));
      expect(error.message, `maxPendingFrames = ${shape.label}`).toContain(
        'maxPendingFrames must be a non-negative integer',
      );
    }
  });

  it('both neighbours still accept a legitimate value', () => {
    const analyzer = newAnalyzer({ fmin: 40, maxPendingFrames: 64 });
    try {
      expect(analyzer.sampleRate()).toBe(ANALYZER_BASE.sampleRate);
    } finally {
      analyzer.delete();
    }
  });
});
