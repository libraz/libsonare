/**
 * Pins how a MIDI clip's `destinationId` marshals across the facade.
 *
 * The field falls back to `trackId` only when it is *absent*. An explicit 0 is
 * a real destination and must be forwarded unchanged — a facade that treats 0
 * as "unset" reroutes the clip to its track id, which is audible as silence at
 * the requested destination and as an unexpected voice at the track's own. The
 * three cases below fix both directions of that mapping, and the Node and
 * Python suites pin the same behaviour so the surfaces cannot drift apart.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { capabilities, ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('RealtimeEngine MIDI clip destination routing', () => {
  const rms = (data: Float32Array): number => {
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length);
  };

  const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

  // One held note, on at frame 0 and off well past the rendered block.
  const heldNote = [
    { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
    { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
  ];

  const renderOneBlock = (engine: RealtimeEngine): number => {
    engine.play();
    const out = engine.process([new Float32Array(128), new Float32Array(128)]);
    return Math.max(rms(out[0]), rms(out[1]));
  };

  it('reports NotSupported when MIDI CC bindings are not compiled in', (context) => {
    if (capabilities().features.arrangement) {
      context.skip();
    }
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      let caught: unknown;
      try {
        engine.clearMidiCcBindings();
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(Error);
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({
        code: ErrorCode.NotSupported,
        codeName: 'NotSupported',
        reason: 'NotCompiledIn',
        message: 'RealtimeEngine.clearMidiCcBindings: arrangement support is not compiled in',
      });
    } finally {
      engine.destroy();
    }
  });

  it('refuses an out-of-domain wordCount without replacing the existing schedule', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      const noteOn = midi1Word(0x9, 0, 60, 100);
      engine.setBuiltinInstrument(6, { gain: 0.5 });
      engine.setMidiClips([
        {
          id: 1,
          trackId: 6,
          destinationId: 6,
          lengthSamples: 8192,
          events: [{ renderFrame: 0, word0: noteOn, wordCount: 1 }],
        },
      ]);
      expect(() =>
        engine.setMidiClips([
          {
            id: 2,
            trackId: 6,
            destinationId: 6,
            lengthSamples: 8192,
            events: [{ renderFrame: 0, word0: midi1Word(0x8, 0, 60, 0), wordCount: 5 }],
          },
        ]),
      ).toThrow(
        new RangeError(
          'RealtimeEngine.setMidiClips: clips[0].events[0].wordCount must be an integer in [0, 4]',
        ),
      );
      expect(() =>
        engine.setMidiClips([
          {
            id: 2,
            trackId: 6,
            destinationId: 6,
            lengthSamples: 128,
            events: [
              {
                renderFrame: 0,
                word0: midi1Word(0x9, 0, 62, 100),
                wordCount: 5,
                group: 'bad' as unknown as number,
              },
            ],
          },
        ]),
      ).toThrow(
        new TypeError('RealtimeEngine.setMidiClips: clips[0].events[0].group must be an integer'),
      );
      expect(renderOneBlock(engine)).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });

  it('names each narrowed scheduled-event field by its clip path', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    const fields: Array<[string, number]> = [
      ['renderFrame', 1.5],
      ['word0', 4294967296],
      ['word1', 4294967296],
      ['word2', 4294967296],
      ['word3', 4294967296],
      ['wordCount', 5],
      ['group', 256],
      ['sysexHandle', 4294967296],
    ];
    try {
      for (const [field, value] of fields) {
        const event = {
          renderFrame: 0,
          word0: midi1Word(0x9, 0, 60, 100),
          wordCount: 1,
          [field]: value,
        } as never;
        let caught: unknown;
        try {
          engine.setMidiClips([{ id: 1, trackId: 5, events: [event] }]);
        } catch (error) {
          caught = error;
        }
        expect(caught).toBeInstanceOf(RangeError);
        expect((caught as Error).message).toContain(`clips[0].events[0].${field}`);
      }

      let groupError: unknown;
      try {
        engine.setMidiClips([
          {
            id: 2,
            trackId: 5,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1, group: 16 },
            ],
          },
        ]);
      } catch (error) {
        groupError = error;
      }
      expect(groupError).toBeInstanceOf(RangeError);
      expect((groupError as Error).message).toContain('clips[0].events[0].group');

      const malformedEvent = () =>
        engine.setMidiClips([{ id: 3, trackId: 5, events: [null as never] }]);
      expect(malformedEvent).toThrow(TypeError);
      expect(malformedEvent).toThrow(/clips\[0\]\.events\[0\] must be an object/);

      const invalidCountAndGroup = () =>
        engine.setMidiClips([
          {
            id: 4,
            trackId: 5,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 5, group: -1 },
            ],
          },
        ]);
      expect(invalidCountAndGroup).toThrow(
        /clips\[0\]\.events\[0\]\.wordCount must be an integer in \[0, 4\]/,
      );
    } finally {
      engine.destroy();
    }
  });

  it('reports the C cause when the MIDI instrument rack is full', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      for (let destinationId = 0; destinationId < 16; destinationId++) {
        engine.setBuiltinInstrument(destinationId, {});
      }
      let caught: unknown;
      try {
        engine.setBuiltinInstrument(16, {});
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({
        code: ErrorCode.InvalidState,
        message: expect.stringContaining('the instrument rack is full'),
      });
    } finally {
      engine.destroy();
    }
  });

  it('reports the C cause when MIDI instrument preparation fails', (context) => {
    if (process.env.SONARE_TEST_MIDI_PREPARE_FAILURE !== '1') {
      context.skip();
    }
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      let caught: unknown;
      try {
        engine.setBuiltinInstrument(0, {});
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(RangeError);
      expect((caught as Error).message).toContain(
        "instrument must be preparable at the engine's sample rate and block size",
      );
    } finally {
      engine.destroy();
    }
  });

  it('infers the UMP size when wordCount is zero', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setBuiltinInstrument(6, { gain: 0.5 });
      engine.setMidiClips([
        {
          id: 1,
          trackId: 6,
          destinationId: 6,
          lengthSamples: 8192,
          events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 0 }],
        },
      ]);
      expect(renderOneBlock(engine)).toBeGreaterThan(0);
    } finally {
      engine.destroy();
    }
  });

  it('forwards an explicit destinationId of 0 rather than rewriting it to the track id', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    // Only destination 0 carries an instrument, and the track id differs from
    // it, so a rewrite to trackId would leave the note unheard.
    engine.setBuiltinInstrument(0, { gain: 0.5 });
    engine.setMidiClips([
      { id: 1, trackId: 6, destinationId: 0, lengthSamples: 8192, events: heldNote },
    ]);
    expect(renderOneBlock(engine)).toBeGreaterThan(0);
    engine.destroy();
  });

  it('does not sound the track-id instrument when destination 0 is explicit', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    // The mirror image: the instrument sits at the track id and destination 0
    // is empty, so the same clip must render silence. A rewrite of 0 to trackId
    // is audible here, which is what the previous case alone cannot catch.
    engine.setBuiltinInstrument(6, { gain: 0.5 });
    engine.setMidiClips([
      { id: 1, trackId: 6, destinationId: 0, lengthSamples: 8192, events: heldNote },
    ]);
    expect(renderOneBlock(engine)).toBe(0);
    engine.destroy();
  });

  it('falls back to the track id when destinationId is omitted', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setBuiltinInstrument(6, { gain: 0.5 });
    engine.setMidiClips([{ id: 1, trackId: 6, lengthSamples: 8192, events: heldNote }]);
    expect(renderOneBlock(engine)).toBeGreaterThan(0);
    engine.destroy();
  });
});
