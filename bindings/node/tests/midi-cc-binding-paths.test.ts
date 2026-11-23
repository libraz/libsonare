import { describe, expect, it } from 'vitest';
import { midiCc, midiCcToBreakpoint, midiParamToCc, RealtimeEngine } from '../src/index.js';

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
