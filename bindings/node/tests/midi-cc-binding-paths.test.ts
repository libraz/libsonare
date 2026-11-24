import { runInNewContext } from 'node:vm';
import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError } from '../src/errors.js';
import {
  midiCc,
  midiCcLearn,
  midiCcToBreakpoint,
  midiParamToCc,
  midiRouteEvents,
  RealtimeEngine,
} from '../src/index.js';

const good = { ccNumber: 7, paramId: 1 };
const event = midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 64 });
const toCc = (binding: object) =>
  midiParamToCc({ bindings: [good, binding as never], paramId: 1, unitValue: 0.5, group: 0 });

describe('a MIDI CC binding inside a list is named by its index', () => {
  it.each([
    ['ccNumber', { ccNumber: 200, paramId: 1 }, RangeError],
    ['ccNumber', { ccNumber: '7', paramId: 1 }, TypeError],
    ['paramId', { ccNumber: 7, paramId: -1 }, RangeError],
    ['channel', { ccNumber: 7, paramId: 1, channel: 16 }, RangeError],
    ['channel', { ccNumber: 7, paramId: 1, channel: '3' }, TypeError],
    ['kind', { ccNumber: 7, paramId: 1, kind: 'bogus' }, RangeError],
    ['consume', { ccNumber: 7, paramId: 1, consume: 'yes' }, TypeError],
    ['ccLsbNumber', { ccNumber: 7, paramId: 1, ccLsbNumber: 300 }, RangeError],
    ['minValue', { ccNumber: 7, paramId: 1, minValue: Number.NaN }, RangeError],
  ] as const)('midiParamToCc names bindings[1].%s', (field, binding, errorClass) => {
    expect(() => toCc(binding)).toThrow(errorClass);
    expect(() => toCc(binding)).toThrow(new RegExp(`^midiParamToCc: bindings\\[1\\]\\.${field} `));
  });

  it('midiCcToBreakpoint names the binding by its index', () => {
    expect(() =>
      midiCcToBreakpoint({ bindings: [{ ccNumber: 200, paramId: 1 } as never], event }),
    ).toThrow(/^midiCcToBreakpoint: bindings\[0\]\.ccNumber must be an integer in \[0, 127\]$/);
  });

  it('validates and ignores consume in CC conversion inputs', () => {
    const base = midiCcToBreakpoint({ bindings: [good], event });
    const consumed = midiCcToBreakpoint({ bindings: [{ ...good, consume: true }], event });
    expect(base).not.toBeNull();
    expect(consumed).toEqual(base);
    expect(() =>
      midiCcToBreakpoint({ bindings: [{ ...good, consume: 1 } as never], event }),
    ).toThrow(/^midiCcToBreakpoint: bindings\[0\]\.consume must be a boolean$/);

    const midiParamRequest = { bindings: [good], paramId: 1, unitValue: 0.5, group: 0 };
    const cc = midiParamToCc(midiParamRequest);
    expect(cc).not.toBeNull();
    expect(
      midiParamToCc({
        ...midiParamRequest,
        bindings: [{ ...good, consume: true }],
      }),
    ).toEqual(cc);
    expect(() =>
      midiParamToCc({
        bindings: [{ ...good, consume: 'yes' } as never],
        paramId: 1,
        unitValue: 0.5,
        group: 0,
      }),
    ).toThrow(/^midiParamToCc: bindings\[0\]\.consume must be a boolean$/);
  });

  it('a single binding keeps its bare field names', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() => engine.bindMidiCc({ ccNumber: 200, paramId: 1 })).toThrow(
        /^RealtimeEngine\.bindMidiCc: ccNumber must be an integer in \[0, 127\]$/,
      );
      expect(() => engine.bindMidiCc({ ccNumber: 7, paramId: 1, channel: 16 })).toThrow(
        /^RealtimeEngine\.bindMidiCc: channel must be /,
      );
    } finally {
      engine.destroy();
    }
  });

  it.each([
    ['ccLsbNumber', { ccLsbNumber: 1 }, 'control-change 14'],
    ['selectorMsb', { selectorMsb: 1 }, 'RPN or NRPN'],
    ['selectorLsb', { selectorLsb: 1 }, 'RPN or NRPN'],
  ] as const)(
    'the conversions refuse a nonzero inactive %s naming it and the kind',
    (field, extra, kind) => {
      const binding = { ...good, ...extra };
      expect(() => toCc(binding)).toThrow(
        new RangeError(
          `midiParamToCc: bindings[1].${field} must be 0 unless bindings[1].kind is ${kind}`,
        ),
      );
      expect(() => midiCcToBreakpoint({ bindings: [binding], event: event })).toThrow(
        new RangeError(
          `midiCcToBreakpoint: bindings[0].${field} must be 0 unless bindings[0].kind is ${kind}`,
        ),
      );
    },
  );

  it.each([
    ['ccLsbNumber', { ccNumber: 7, paramId: 11, ccLsbNumber: 1 }, 'control-change 14'],
    ['selectorMsb', { ccNumber: 8, paramId: 12, selectorMsb: 1 }, 'RPN or NRPN'],
    ['selectorLsb', { ccNumber: 9, paramId: 13, selectorLsb: 1 }, 'RPN or NRPN'],
  ] as const)(
    'bindMidiCc refuses a nonzero inactive %s naming it and the kind',
    (field, binding, kind) => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        expect(() => engine.bindMidiCc(binding)).toThrow(
          new RangeError(`RealtimeEngine.bindMidiCc: ${field} must be 0 unless kind is ${kind}`),
        );
        expect(engine.midiCcBindings()).toHaveLength(0);
      } finally {
        engine.destroy();
      }
    },
  );
});

it('midiCcToBreakpoint forwards SysEx to the C refusal', () => {
  const error = (() => {
    try {
      midiCcToBreakpoint({
        bindings: [good],
        event: {
          ppq: 0,
          word0: 0x30000000,
          word1: 0,
          sysex: Uint8Array.of(0x11, 0xf0, 0x00, 0x7d, 0xf7, 0x22).subarray(1, 5),
        },
      });
    } catch (caught) {
      return caught;
    }
    return undefined;
  })();
  expect(isSonareError(error)).toBe(true);
  expect(error).toMatchObject({ code: ErrorCode.NotSupported, codeName: 'NotSupported' });
  expect((error as Error).message).toBe(
    'midiCcToBreakpoint: MIDI CC conversion does not support SysEx events',
  );

  const wrongShape = () =>
    midiCcToBreakpoint({
      bindings: [{ ...good, ccLsbNumber: 128 }],
      event: { ppq: 0, word0: 0x30000000, sysex: [0xf0, 0x00, 0xf7] as never },
    });
  expect(wrongShape).toThrow(TypeError);
  expect(wrongShape).toThrow(new TypeError('midiCcToBreakpoint: event.sysex must be a Uint8Array'));
});

it('learn ignores valid SysEx and names a payload on a channel event', () => {
  const bytes = Uint8Array.from([0xf0, 0x7e, 0x7f, 0xf7]);
  const start = midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 32 });
  const end = midiCc({ ppq: 1, group: 0, channel: 0, controller: 7, value: 96 });
  const sysex = { ppq: 0, word0: 0x30000000, word1: 0, sysex: bytes };
  const binding = midiCcLearn({ events: [start, end], paramId: 1 });
  expect(binding).not.toBeNull();
  expect(midiCcLearn({ events: [sysex, start, end], paramId: 1 })).toEqual(binding);
  const invalid = () => midiCcLearn({ events: [{ ...start, sysex: bytes }, end], paramId: 1 });
  expect(invalid).toThrow(RangeError);
  expect(invalid).toThrow(/events\[0\]\.sysex/);
});

it('copies the SysEx byte range including an embedded zero', () => {
  const source = Uint8Array.of(0x11, 0xf0, 0x00, 0x7d, 0xf7, 0x22);
  const sysex = source.subarray(1, 5);
  const result = midiRouteEvents({ events: [{ ppq: 0, word0: 0x30000000, sysex }] });
  source.fill(0);
  expect(result.events[0]?.sysex).toEqual(Uint8Array.of(0xf0, 0x00, 0x7d, 0xf7));
});

it('checks binding shapes across the entire request before binding domains', () => {
  const bad = { ccNumber: 200, paramId: 1, channel: 'not a channel' } as never;
  expect(() => midiCcToBreakpoint({ bindings: [bad], event })).toThrow(
    new TypeError('midiCcToBreakpoint: bindings[0].channel must be a number'),
  );
  expect(() =>
    midiParamToCc({ bindings: [good, bad], paramId: 1, unitValue: 0.5, group: 0 }),
  ).toThrow(new TypeError('midiParamToCc: bindings[1].channel must be a number'));
  expect(() =>
    midiParamToCc({
      bindings: [{ ...good, ccLsbNumber: 128 }],
      paramId: 1,
      unitValue: 'not a value',
      group: 0,
    } as never),
  ).toThrow(new TypeError('midiParamToCc: unitValue must be a number'));

  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    expect(() => engine.bindMidiCc(bad)).toThrow(
      new TypeError('RealtimeEngine.bindMidiCc: channel must be a number'),
    );
  } finally {
    engine.destroy();
  }
});

it('checks every routed event shape before any event domain', () => {
  const malformedLater = () =>
    midiRouteEvents({
      events: [
        { ppq: 0, word0: 4294967296 },
        { ppq: 'not ppq', word0: 0 },
      ] as never,
    });
  expect(malformedLater).toThrow(new TypeError('midiRouteEvents: events[1].ppq must be a number'));
  expect(() =>
    midiRouteEvents({
      events: [{ ppq: 0, word0: 4294967296 }],
      filterGroup: 'wrong' as never,
    }),
  ).toThrow(new TypeError('midiRouteEvents: filterGroup must be a number'));
});

it('accepts a cross-realm Uint8Array through the native event reader', () => {
  const sysex = runInNewContext('new Uint8Array([0xf0, 0x00, 0xf7])') as Uint8Array;
  expect(
    midiCcLearn({ events: [{ ppq: 0, word0: 0x30000000, word1: 0, sysex }], paramId: 1 }),
  ).toBeNull();
  const result = midiRouteEvents({ events: [{ ppq: 0, word0: 0x30000000, sysex }] });
  expect(result.events[0]?.sysex).toEqual(Uint8Array.of(0xf0, 0x00, 0xf7));
});

it('refuses wrong event and request shapes before CC domains', () => {
  const event = { ppq: 0, word0: 0x30000000, sysex: [0xf0, 0x00, 0xf7] as never };
  const learn = () =>
    midiCcLearn({
      events: [midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 64 })],
      paramId: 1,
      maxValue: 'wrong' as never,
      minMovement: 128,
    });
  const learnList = () =>
    midiCcLearn({ events: [{ ppq: 0, word0: 4294967296 }, event], paramId: 1 });
  for (const [run, message] of [
    [learn, 'midiCcLearn: maxValue must be a number'],
    [learnList, 'midiCcLearn: events[1].sysex must be a Uint8Array'],
  ] as const) {
    expect(run).toThrow(TypeError);
    expect(run).toThrow(new TypeError(message));
  }
});
