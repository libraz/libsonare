/**
 * The error class of a refused argument.
 *
 * The contract is by cause, not by which layer refused: a refused argument is a
 * native `TypeError` (wrong type) or `RangeError` (out of domain), whether the
 * binding or the library refused it, so every library `InvalidParameter` is a
 * `RangeError`. A failure that is not an argument (object state, unsupported,
 * allocation, decode, cancellation) keeps its code as a `SonareError`. The C++
 * readers or `sonareExceptionInfo` decide the class and the module wrapper
 * rebuilds it.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { ExtractNotesRequest } from '../dist/index.js';
import {
  dbToAmplitude,
  detectChords,
  ErrorCode,
  extractNotes,
  framesToSamples,
  framesToTime,
  hzToMel,
  hzToMidi,
  hzToNote,
  init,
  isSonareError,
  Mixer,
  masteringApplyProcessor,
  masteringInsertTiming,
  masteringRepairDereverbConfigForRoom,
  masteringRepairNoiseBandBins,
  melToAudio,
  melToHz,
  melToStft,
  mfccToAudio,
  mfccToMel,
  midiToHz,
  mixingScenePreset,
  mixStereo,
  RealtimeEngine,
  renderPlayback,
  StreamingEqualizer,
  samplesToFrames,
  timeToFrames,
  voiceChangeRealtime,
} from '../dist/index.js';

function thrown(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected the call to throw');
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

describe('argument refusal error class', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses a wrong-typed field with a TypeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers([{ id: 1, ppq: 'x' as unknown as number, name: 'a', kind: 'marker' }]),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe('RealtimeEngine.setMarkers: ppq must be a number');
    });
  });

  it('refuses an out-of-domain field with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers([{ id: 1, ppq: -1, name: 'a', kind: 'marker' }]),
      );
      expect(error).toBeInstanceOf(RangeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe(
        'RealtimeEngine.setMarkers: marker ppq must be finite and at least 0',
      );
    });
  });

  it('refuses an array-like that is not one with a TypeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers(null as unknown as Parameters<typeof engine.setMarkers>[0]),
      );
      expect(error).toBeInstanceOf(TypeError);
    });
  });

  it('refuses a length outside the safe-integer domain with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers({ length: -1 } as unknown as Parameters<typeof engine.setMarkers>[0]),
      );
      expect(error).toBeInstanceOf(RangeError);
    });
  });

  it('refuses an engine lookup of an unknown id with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() => engine.marker(999));
      expect(error).toBeInstanceOf(RangeError);
      expect(isSonareError(error)).toBe(false);
    });
  });

  it('keeps an object-state failure a coded SonareError, and an unknown bus a refusal', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('drumBusSubgroup')), {
      sampleRate: 48000,
    });
    try {
      // The library reports INVALID_PARAMETER for an unknown bus: refused by cause.
      const refusal = thrown(() => mixer.busMeter('no-such-bus'));
      expect(refusal).toBeInstanceOf(RangeError);
      expect(isSonareError(refusal)).toBe(false);
      expect((refusal as Error).message).toBe('Mixer.busMeter: Invalid parameter');
    } finally {
      mixer.destroy();
    }
    const state = thrown(() => mixer.busMeter('no-such-bus'));
    expect(isSonareError(state)).toBe(true);
    expect((state as { code: number }).code).toBe(ErrorCode.InvalidState);
  });
});

describe('argument refusal classes shared with the Node surface', () => {
  const samples = new Float32Array(2205);

  beforeAll(async () => {
    await init();
  });

  it('refuses a non-number unit-conversion argument with a TypeError and keeps NaN total', () => {
    const wrong = '440' as unknown as number;
    for (const call of [
      () => hzToMel(wrong),
      () => melToHz(wrong),
      () => hzToMidi(wrong),
      () => midiToHz(wrong),
      () => hzToNote(wrong),
      () => dbToAmplitude({ values: new Float32Array(2), ref: wrong }),
      () => framesToTime(wrong),
      () => framesToSamples(wrong),
      () => samplesToFrames(wrong),
      () => timeToFrames(wrong),
    ]) {
      expect(thrown(call)).toBeInstanceOf(TypeError);
    }
    expect(Number.isNaN(hzToMel(Number.NaN))).toBe(true);
  });

  it('refuses a non-positive hop in the frame/time conversions with a RangeError', () => {
    expect(thrown(() => framesToTime(1, 22050, 0))).toBeInstanceOf(RangeError);
    expect(thrown(() => timeToFrames(1, 22050, -1))).toBeInstanceOf(RangeError);
  });

  it('refuses a missing frameRate on the note entry points with a TypeError', () => {
    const request = {
      samples,
      sampleRate: 22050,
      f0Hz: new Float32Array(4),
      voiced: new Uint8Array(4),
    } as unknown as ExtractNotesRequest;
    const error = thrown(() => extractNotes(request));
    expect(error).toBeInstanceOf(TypeError);
    expect((error as Error).message).toBe('extractNotes: frameRate must be a number');
  });

  it('refuses a missing playback config with a TypeError, not an embind error', () => {
    const error = thrown(() =>
      renderPlayback({
        samples: new Float32Array(480),
        numChannels: 1,
        sampleRate: 48000,
      } as never),
    );
    expect(error).toBeInstanceOf(TypeError);
  });

  it('refuses an unknown chord chroma method with a RangeError', () => {
    expect(
      thrown(() => detectChords(samples, 22050, { chromaMethod: 'bogus' } as never)),
    ).toBeInstanceOf(RangeError);
  });

  it('refuses an unknown EQ phase mode and band placement with a RangeError', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000 });
    try {
      expect(thrown(() => eq.setPhaseMode('bogus' as never))).toBeInstanceOf(RangeError);
      expect(
        thrown(() => eq.magnitudeResponse(new Float32Array(1), 'bogus' as never)),
      ).toBeInstanceOf(RangeError);
    } finally {
      eq.destroy();
    }
  });

  it('refuses a bad realtime voice-changer channel count and stereo length with a RangeError', () => {
    expect(
      thrown(() =>
        voiceChangeRealtime({
          channels: [samples, samples, samples],
          sampleRate: 48000,
        }),
      ),
    ).toBeInstanceOf(RangeError);
    expect(
      thrown(() =>
        voiceChangeRealtime({
          channels: [new Float32Array(2205), new Float32Array(2204)],
          sampleRate: 48000,
        }),
      ),
    ).toBeInstanceOf(RangeError);
  });

  it('refuses the removed pan-mode alias with a RangeError', () => {
    const left = new Float32Array(2205).fill(0.1);
    const right = new Float32Array(2205).fill(0.1);
    for (const panMode of ['pan', 'bogus']) {
      const error = thrown(() =>
        mixStereo({
          strips: [{ left, right, pan: 0.5, panMode } as never],
          sampleRate: 22050,
        }),
      );
      expect(error).toBeInstanceOf(RangeError);
    }
  });

  it('refuses an unread key of a solo mastering processor with a TypeError', () => {
    const error = thrown(() =>
      masteringApplyProcessor({
        processor: 'utility.gain',
        samples: new Float32Array(2205),
        sampleRate: 48000,
        params: { gainDb: 3 } as never,
      }),
    );
    expect(error).toEqual(
      new TypeError('masteringApplyProcessor: unknown key gainDb (expected levelDb)'),
    );
    expect(isSonareError(error)).toBe(false);
  });

  it('refuses an out-of-range mixStereo pan with a RangeError', () => {
    const channel = new Float32Array(2205).fill(0.1);
    const error = thrown(() =>
      mixStereo({ strips: [{ left: channel, right: channel, pan: 2 }], sampleRate: 48000 }),
    );
    expect(error).toBeInstanceOf(RangeError);
    expect(isSonareError(error)).toBe(false);
  });

  it('refuses a bad truePeakOversample with the same class on addStrip and configureMeter', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('drumBusSubgroup')), {
      sampleRate: 48000,
    });
    try {
      const strip = thrown(() => mixer.addStrip('extra', { truePeakOversample: 3 }));
      const meter = thrown(() => mixer.configureMeter({ enabled: true, truePeakOversample: 3 }));
      expect(strip).toBeInstanceOf(RangeError);
      expect(meter).toBeInstanceOf(RangeError);
    } finally {
      mixer.destroy();
    }
  });

  it('refuses an unknown masteringInsertTiming parameter key with a TypeError', () => {
    const error = thrown(() =>
      masteringInsertTiming({
        insert: 'utility.gain',
        params: { bogus: 1 } as never,
        sampleRate: 48000,
      }),
    );
    expect(error).toEqual(
      new TypeError('masteringInsertTiming: unknown key bogus (expected levelDb)'),
    );
    expect(isSonareError(error)).toBe(false);
  });

  it('refuses a misspelt request key of the inverse transforms by name', () => {
    const power = { data: new Float32Array(8), rows: 2, cols: 4 };
    const refused = (call: () => unknown, key: string) => {
      const error = thrown(call);
      expect(error).toBeInstanceOf(TypeError);
      expect((error as Error).message).toContain(`unknown key ${key} (expected `);
    };
    refused(() => melToStft({ power, nfft: 4096 } as never), 'nfft');
    refused(() => melToAudio({ power, nfft: 4096 } as never), 'nfft');
    refused(() => mfccToMel({ coefficients: power, nmels: 8 } as never), 'nmels');
    refused(() => mfccToAudio({ coefficients: power, nfft: 4096 } as never), 'nfft');
    refused(() => masteringRepairNoiseBandBins({ nfft: 4096 } as never), 'nfft');
    refused(
      () => masteringRepairDereverbConfigForRoom({ estimate: {}, bogus: 1 } as never),
      'bogus',
    );
  });
});
