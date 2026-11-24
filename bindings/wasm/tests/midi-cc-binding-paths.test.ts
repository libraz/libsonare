import { beforeAll, describe, expect, it } from 'vitest';
import { init, midiCc, midiCcToBreakpoint, midiParamToCc, RealtimeEngine } from '../dist/index.js';

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
});
