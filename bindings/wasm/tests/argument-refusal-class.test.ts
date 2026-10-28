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
  masteringInsertTiming,
  masteringProcess,
  masteringRepairDereverbConfigForRoom,
  masteringRepairNoiseBandBins,
  melToAudio,
  melToHz,
  melToStft,
  mfccToAudio,
  mfccToMel,
  midiToHz,
  mixingScenePresetJson,
  mixStereo,
  RealtimeEngine,
  renderPlayback,
  StreamAnalyzer,
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
  const engine = new RealtimeEngine(48000, 256);
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
        engine.setMarkers([{ id: 1, ppq: 'x' as unknown as number, name: 'a', kind: 0 }]),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe('ppq must be a number');
    });
  });

  it('refuses an out-of-domain field with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() => engine.setMarkers([{ id: 1, ppq: -1, name: 'a', kind: 0 }]));
      expect(error).toBeInstanceOf(RangeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe(
        'setMarkers: marker ppq must be finite and non-negative',
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
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'));
    try {
      // The library reports INVALID_PARAMETER for an unknown bus: refused by cause.
      const refusal = thrown(() => mixer.busMeter('no-such-bus'));
      expect(refusal).toBeInstanceOf(RangeError);
      expect(isSonareError(refusal)).toBe(false);
      expect((refusal as Error).message).toBe('failed to read bus meter: Invalid parameter');
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
      () => dbToAmplitude(new Float32Array(2), wrong),
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

  it('refuses a missing frameRate on the note entry points with a RangeError', () => {
    const request = {
      samples,
      sampleRate: 22050,
      f0Hz: new Float32Array(4),
      voiced: new Uint8Array(4),
    } as unknown as Parameters<typeof extractNotes>[0];
    expect(thrown(() => extractNotes(request))).toBeInstanceOf(RangeError);
  });

  it('refuses a missing playback config with a TypeError, not an embind error', () => {
    const error = thrown(() =>
      renderPlayback({ samples: new Float32Array(480), channels: 1, sampleRate: 48000 } as never),
    );
    expect(error).toBeInstanceOf(TypeError);
  });

  it('refuses an unknown chord chroma method with a RangeError', () => {
    expect(
      thrown(() => detectChords(samples, 22050, { chromaMethod: 'bogus' } as never)),
    ).toBeInstanceOf(RangeError);
  });

  it('refuses an unknown EQ phase mode and band placement with a RangeError', () => {
    const eq = new StreamingEqualizer();
    try {
      expect(thrown(() => eq.setPhaseMode('bogus' as never))).toBeInstanceOf(RangeError);
      expect(
        thrown(() => eq.magnitudeResponse(new Float32Array(1), 'bogus' as never)),
      ).toBeInstanceOf(RangeError);
    } finally {
      eq.delete();
    }
  });

  it('refuses a bad realtime voice-changer channel count and stereo length with a RangeError', () => {
    expect(
      thrown(() => voiceChangeRealtime({ samples, sampleRate: 48000, channels: 3 as never })),
    ).toBeInstanceOf(RangeError);
    expect(
      thrown(() =>
        voiceChangeRealtime({ samples: new Float32Array(2205), sampleRate: 48000, channels: 2 }),
      ),
    ).toBeInstanceOf(RangeError);
  });

  it('refuses the removed pan-mode alias with a RangeError', () => {
    const left = [new Float32Array(2205).fill(0.1)];
    const right = [new Float32Array(2205).fill(0.1)];
    for (const panMode of ['pan', 'bogus']) {
      const error = thrown(() => mixStereo(left, right, 22050, { pan: 0.5, panMode } as never));
      expect(error).toBeInstanceOf(RangeError);
    }
  });

  it('refuses an unread key of a solo mastering processor with a RangeError', () => {
    const error = thrown(() =>
      masteringProcess('utility.gain', new Float32Array(2205), 48000, { gainDb: 3 } as never),
    );
    expect(error).toBeInstanceOf(RangeError);
    expect(isSonareError(error)).toBe(false);
  });

  it('refuses an out-of-range mixStereo pan with a RangeError', () => {
    const channel = [new Float32Array(2205).fill(0.1)];
    const error = thrown(() => mixStereo(channel, channel, 48000, { pan: [2] }));
    expect(error).toBeInstanceOf(RangeError);
    expect(isSonareError(error)).toBe(false);
  });

  it('refuses a bad truePeakOversample with the same class on addStrip and configureMeter', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'));
    try {
      const strip = thrown(() => mixer.addStrip('extra', { truePeakOversample: 3 }));
      const meter = thrown(() => mixer.configureMeter(true, 3));
      expect(strip).toBeInstanceOf(RangeError);
      expect(meter).toBeInstanceOf(RangeError);
    } finally {
      mixer.destroy();
    }
  });

  it('refuses an unknown masteringInsertTiming parameter key with a RangeError', () => {
    const error = thrown(() => masteringInsertTiming('utility.gain', { bogus: 1 } as never, 48000));
    expect(error).toBeInstanceOf(RangeError);
    expect(isSonareError(error)).toBe(false);
  });

  it('reports StreamAnalyzer computeMagnitude as a coded NotSupported SonareError', () => {
    const error = thrown(() => new StreamAnalyzer({ sampleRate: 22050, computeMagnitude: true }));
    expect(isSonareError(error)).toBe(true);
    expect((error as { code: number }).code).toBe(ErrorCode.NotSupported);
    expect((error as Error).message).toMatch(/computeMagnitude is not supported/);
  });

  it('refuses a misspelt request key of the inverse transforms by name', () => {
    const power = new Float32Array(8);
    const refused = (call: () => unknown, key: string) => {
      const error = thrown(call);
      expect(error).toBeInstanceOf(TypeError);
      expect((error as Error).message).toContain(`'${key}'`);
    };
    refused(() => melToStft({ power, nMels: 2, nFrames: 4, nfft: 4096 } as never), 'nfft');
    refused(() => melToAudio({ power, nMels: 2, nFrames: 4, nfft: 4096 } as never), 'nfft');
    refused(
      () => mfccToMel({ coefficients: power, nMfcc: 2, nFrames: 4, nmels: 8 } as never),
      'nmels',
    );
    refused(
      () => mfccToAudio({ coefficients: power, nMfcc: 2, nFrames: 4, nfft: 4096 } as never),
      'nfft',
    );
    refused(() => masteringRepairNoiseBandBins({ nfft: 4096 } as never), 'nfft');
    refused(
      () => masteringRepairDereverbConfigForRoom({ estimate: {}, bogus: 1 } as never),
      'bogus',
    );
    refused(
      () => masteringRepairDereverbConfigForRoom({} as never, { bogus: 1 } as never),
      'bogus',
    );
  });
});
