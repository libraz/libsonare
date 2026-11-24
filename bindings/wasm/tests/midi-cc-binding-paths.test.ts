import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  midiCc,
  midiCcLearn,
  midiCcToBreakpoint,
  midiParamToCc,
  RealtimeEngine,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const good = { ccNumber: 7, paramId: 1 };
const makeEvent = () => midiCc({ ppq: 0, group: 0, channel: 0, controller: 7, value: 64 });
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
    ['ccLsbNumber', { ccNumber: 7, paramId: 1, ccLsbNumber: 300 }, RangeError],
    ['minValue', { ccNumber: 7, paramId: 1, minValue: Number.NaN }, RangeError],
  ] as const)('midiParamToCc names bindings[1].%s', (field, binding, errorClass) => {
    expect(() => toCc(binding)).toThrow(errorClass);
    expect(() => toCc(binding)).toThrow(new RegExp(`^midiParamToCc: bindings\\[1\\]\\.${field} `));
  });

  it('midiCcToBreakpoint names the binding by its index', () => {
    expect(() =>
      midiCcToBreakpoint({
        bindings: [{ ccNumber: 200, paramId: 1 } as never],
        event: makeEvent(),
      }),
    ).toThrow(/^midiCcToBreakpoint: bindings\[0\]\.ccNumber must be an integer in \[0, 127\]$/);
  });

  it('reports a valid SysEx event as unsupported by CC conversion', () => {
    expect(() =>
      midiCcToBreakpoint({
        bindings: [good],
        event: { ppq: 0, word0: 0x30000000, word1: 0, sysex: new Uint8Array([0xf0, 0xf7]) },
      }),
    ).toThrow(
      expect.objectContaining({
        code: ErrorCode.NotSupported,
        message: 'midiCcToBreakpoint: MIDI CC conversion does not support SysEx events',
      }),
    );
  });

  it('requires SysEx payloads to be Uint8Array', () => {
    expect(() =>
      midiCcToBreakpoint({
        bindings: [good],
        event: { ppq: 0, word0: 0x30000000, sysex: [0xf0, 0xf7] as never },
      }),
    ).toThrow(new TypeError('midiCcToBreakpoint: event.sysex must be a Uint8Array'));
  });

  it('validates and ignores consume in CC conversion inputs', () => {
    const event = makeEvent();
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
    [
      'ccLsbNumber',
      { ccNumber: 7, paramId: 11, ccLsbNumber: 1 },
      'ccLsbNumber must be 0 unless kind is control-change 14',
    ],
    [
      'selectorMsb',
      { ccNumber: 8, paramId: 12, selectorMsb: 1 },
      'selectorMsb must be 0 unless kind is RPN or NRPN',
    ],
    [
      'selectorLsb',
      { ccNumber: 9, paramId: 13, selectorLsb: 1 },
      'selectorLsb must be 0 unless kind is RPN or NRPN',
    ],
  ] as const)(
    '%s uses caller-spelled relation subjects on the engine path',
    (_field, binding, message) => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        expect(() => engine.bindMidiCc(binding as never)).toThrow(
          new RangeError(`RealtimeEngine.bindMidiCc: ${message}`),
        );
        expect(engine.midiCcBindings()).toHaveLength(0);
      } finally {
        engine.destroy();
      }
    },
  );

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
      expect(() => midiCcToBreakpoint({ bindings: [binding], event: makeEvent() })).toThrow(
        new RangeError(
          `midiCcToBreakpoint: bindings[0].${field} must be 0 unless bindings[0].kind is ${kind}`,
        ),
      );
    },
  );
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
