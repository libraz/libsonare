import { describe, expect, it } from 'vitest';
import {
  Audio,
  deemphasis,
  ErrorCode,
  ebur128LoudnessRange,
  fourierTempogram,
  lufs,
  masteringChain,
  meteringSilenceRatio,
  momentaryLufs,
  nnlsChroma,
  onsetEnvelope,
  preemphasis,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  shortTermLufs,
  tempogram,
  tempogramRatio,
} from '../src/index.js';
import type { MasteringChainConfig } from '../src/types.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('progress callback', () => {
  it('masteringChain invokes onProgress for an empty config', () => {
    const progress: Array<[number, string]> = [];
    masteringChain(new Float32Array(22050).fill(0.1), 22050, {}, (value, stage) => {
      progress.push([value, stage]);
    });
    expect(progress).toEqual([[1, 'complete']]);
  });

  it('masteringChain invokes onProgress for each enabled stage', () => {
    const samples = new Float32Array(22050).fill(0.1);
    const stages: string[] = [];
    const progresses: number[] = [];
    masteringChain(
      samples,
      22050,
      {
        eq: { tilt: { tiltDb: 1.0 } },
        dynamics: { compressor: { thresholdDb: -24 } },
      },
      (progress, stage) => {
        progresses.push(progress);
        stages.push(stage);
      },
    );
    expect(stages).toContain('eq.tilt');
    expect(stages).toContain('dynamics.compressor');
    expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
  });

  it('masteringChain ignores an onProgress false result', () => {
    const progress: number[] = [];
    const result = masteringChain(
      new Float32Array(22050).fill(0.1),
      22050,
      {
        eq: { tilt: { tiltDb: 1.0 } },
        dynamics: { compressor: { thresholdDb: -24 } },
      },
      (value) => {
        progress.push(value);
        return false;
      },
    );

    expect(progress.some((value) => value > 0.5)).toBe(true);
    expect(result.samples).toBeInstanceOf(Float32Array);
  });

  it('masteringChain cancels through the request cancel callback', () => {
    expect(() =>
      masteringChain({
        samples: new Float32Array(22050).fill(0.1),
        sampleRate: 22050,
        config: { eq: { tilt: { tiltDb: 1.0 } } },
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });
});

describe('feature request-object compatibility', () => {
  it('preserves positional results for emphasis, chroma, and loudness features', () => {
    const filterInput = new Float32Array([1, 1, 1, 1]);
    const emphasized = preemphasis(filterInput, 0.5, 0);
    expect(Array.from(preemphasis({ samples: filterInput, coef: 0.5, zi: 0 }))).toEqual(
      Array.from(emphasized),
    );
    expect(Array.from(deemphasis({ samples: emphasized, coef: 0.5, zi: 0 }))).toEqual(
      Array.from(deemphasis(emphasized, 0.5, 0)),
    );

    const chromaInput = generateSine(440, SR, 2.0);
    const chroma = nnlsChroma(chromaInput, SR);
    const objectChroma = nnlsChroma({ samples: chromaInput, sampleRate: SR });
    expect(objectChroma.nChroma).toBe(chroma.nChroma);
    expect(objectChroma.nFrames).toBe(chroma.nFrames);
    expect(Array.from(objectChroma.data)).toEqual(Array.from(chroma.data));
    const unblended = nnlsChroma(chromaInput, SR, {
      enableStftBlend: false,
      stftBlendWeight: 0,
      stftBlendNFft: 2048,
    });
    expect(unblended.data.length).toBe(12 * unblended.nFrames);

    const loudnessInput = generateSine(440, 48000, 3.0);
    const positional = lufs(loudnessInput, 48000);
    const object = lufs({ samples: loudnessInput, sampleRate: 48000 });
    expect(object.integratedLufs).toBeCloseTo(positional.integratedLufs, 10);
    expect(Array.from(momentaryLufs({ samples: loudnessInput, sampleRate: 48000 }))).toEqual(
      Array.from(momentaryLufs(loudnessInput, 48000)),
    );
    expect(Array.from(shortTermLufs({ samples: loudnessInput, sampleRate: 48000 }))).toEqual(
      Array.from(shortTermLufs(loudnessInput, 48000)),
    );
    expect(ebur128LoudnessRange({ samples: loudnessInput, sampleRate: 48000 })).toBeCloseTo(
      ebur128LoudnessRange(loudnessInput, 48000),
      10,
    );
  });

  it('exposes silence ratio metering', () => {
    const samples = new Float32Array(2048);
    samples.fill(1, 1024);
    expect(meteringSilenceRatio(samples, SR, -45, 1024, 1024)).toBeCloseTo(0.5, 6);
  });
});

describe('color saturation stages engage only when meaningful', () => {
  const stagesFor = (config: MasteringChainConfig): string[] =>
    masteringChain(new Float32Array(22050).fill(0.1), 22050, config).stages;

  it('does not engage the exciter when amount is zero', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0 } } })).not.toContain(
      'saturation.exciter',
    );
  });

  it('does not engage tape when drive and saturation are zero', () => {
    expect(stagesFor({ saturation: { tape: { driveDb: 0, saturation: 0 } } })).not.toContain(
      'saturation.tape',
    );
  });

  it('engages the exciter when amount is positive', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0.2 } } })).toContain('saturation.exciter');
  });

  it('honors an explicit enabled:false even with meaningful params', () => {
    expect(stagesFor({ saturation: { tape: { driveDb: 3, enabled: false } } })).not.toContain(
      'saturation.tape',
    );
  });

  it('honors an explicit enabled:true even with zero amount', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0, enabled: true } } })).toContain(
      'saturation.exciter',
    );
  });
});

describe('StreamingMasteringChain', () => {
  it('processes mono blocks and reports stage names', () => {
    const chain = new StreamingMasteringChain({
      'eq.tilt.tiltDb': 0.5,
      'dynamics.compressor.thresholdDb': -20,
    });
    chain.prepare(48000, 512, 1);
    expect(chain.stageNames()).toEqual(expect.arrayContaining(['eq.tilt', 'dynamics.compressor']));
    const out = chain.processMono(new Float32Array(512).fill(0.1));
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    chain.reset();
  });

  it('flushes delayed mono output exactly once', () => {
    const chain = new StreamingMasteringChain({ 'maximizer.truePeakLimiter.enabled': true });
    chain.prepare(48000, 64, 1);
    expect(chain.latencySamples()).toBeGreaterThan(0);
    chain.processMono(new Float32Array(64).fill(0.5));
    const tail: number[] = [];
    for (;;) {
      const block = chain.flushMono();
      if (block.length === 0) {
        break;
      }
      tail.push(...block);
    }
    expect(tail.length).toBeGreaterThanOrEqual(chain.latencySamples());
    expect(chain.flushMono()).toHaveLength(0);
  });

  it('rejects denoise and loudness stages', () => {
    expect(() => new StreamingMasteringChain({ 'repair.denoise.enabled': true })).toThrow();
    expect(() => new StreamingMasteringChain({ 'loudness.targetLufs': -14 })).toThrow();
  });

  it('accepts a loudness stage when a static gain is supplied', () => {
    const chain = new StreamingMasteringChain({
      'loudness.targetLufs': -14,
      loudnessStaticGainDb: 3.0,
      loudnessStaticGainPeakDb: -1.0,
    });
    chain.prepare(48000, 512, 1);
    const out = chain.processMono(new Float32Array(512).fill(0.1));
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    chain.reset();
  });
});

describe('StreamingEqualizer', () => {
  it('processes stereo blocks and exposes a spectrum snapshot', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
    eq.setGainScale(0.5);
    eq.setOutputGainDb(3);
    eq.setOutputPan(0);
    const left = new Float32Array(512).fill(0.1);
    const right = new Float32Array(512).fill(0.1);
    const out = eq.processStereo(left, right);
    expect(out.left.length).toBe(512);
    expect(out.right.length).toBe(512);
    expect(Number.isFinite(out.left[0])).toBe(true);
    const snapshot = eq.spectrum();
    expect(snapshot.seq).toBeGreaterThan(0);
    expect(snapshot.bandGainDb.length).toBe(24);
    expect(snapshot.bandGainDb[0]).toBeGreaterThan(2.5);
    expect(snapshot.bandGainDb[0]).toBeLessThan(3.5);
  });

  it('switches phase mode and reports linear-phase latency', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'Peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true });
    eq.setPhaseMode('linear');
    expect(eq.latencySamples()).toBeGreaterThan(0);
    eq.setPhaseMode('linear-phase');
    expect(eq.latencySamples()).toBeGreaterThan(0);
    eq.setPhaseMode('zero');
    expect(eq.latencySamples()).toBe(0);
    expect(() => eq.setPhaseMode('bogus' as unknown as 'zero')).toThrow();
  });

  it('accepts an external sidechain key for dynamic bands', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, {
      type: 'Peak',
      frequencyHz: 1000,
      gainDb: 0,
      q: 2,
      enabled: true,
      dynamic: true,
      externalSidechain: true,
      thresholdDb: -32,
      ratio: 4,
      rangeDb: -12,
      attackMs: 0,
      releaseMs: 20,
    });
    const audio = new Float32Array(512).fill(0.02);
    const key = generateSine(1000, 48000, 512 / 48000);
    eq.setSidechainMono(key);
    const out = eq.processMono(audio);
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    eq.clearSidechain();
  });

  // The band reader used to be a file-local copy of the shared option helpers,
  // which is why nothing counted setBand as options-accepting. Migrating it back
  // to the shared family must not drop a key on the way, so pin the ones with an
  // observable effect, and pin the four string-valued keys through the fact that
  // an unknown spelling throws — a reader that silently fell back to the default
  // could not throw at all.
  it('applies every band field supplied to setBand', () => {
    const sampleRate = 48000;
    // `bandGainDb` is indexed BY BAND, not by frequency: entry i is the gain
    // band i currently applies (`band.gain_db * gain_scale`), or the dynamic
    // EQ's live movement when that band is dynamic. It is not a response curve,
    // so every assertion below reads band 0 and treats the rest as unconfigured.
    const staticBand = {
      type: 'HighShelf',
      coeffMode: 'Rbj',
      frequencyHz: 6000,
      gainDb: 9,
      q: 0.9,
      enabled: true,
      slopeDbOct: 24,
      placement: 'Stereo',
      phase: 'Inherit',
      soloed: false,
      bypassed: false,
      proportionalQ: true,
      proportionalQStrength: 0.5,
      dynamic: false,
      thresholdDb: -30,
      autoThreshold: false,
      ratio: 3,
      rangeDb: -8,
      attackMs: 5,
      releaseMs: 60,
      detectorDelayMs: 2,
      externalSidechain: false,
      sidechainFreqHz: 900,
      sidechainQ: 1.2,
    } as const;

    // The snapshot is republished from inside process(), so a band has to be
    // driven with a block before its applied gain is readable.
    const appliedGainDb = (band: Record<string, unknown>, gainScale?: number): number => {
      const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        eq.setBand(0, band as never);
        if (gainScale !== undefined) {
          eq.setGainScale(gainScale);
        }
        eq.processMono(generateSine(1000, sampleRate, 512 / sampleRate));
        return eq.spectrum().bandGainDb[0];
      } finally {
        eq.destroy();
      }
    };

    // gainDb reaches the band with its own value and its own sign.
    expect(appliedGainDb(staticBand)).toBeCloseTo(9, 3);
    expect(appliedGainDb({ ...staticBand, gainDb: -9 })).toBeCloseTo(-9, 3);

    // enabled / bypassed each take the band out on their own.
    expect(appliedGainDb({ ...staticBand, enabled: false })).toBe(0);
    expect(appliedGainDb({ ...staticBand, bypassed: true })).toBe(0);

    // `dynamic` switches what the band reports from its static gain to the
    // movement the detector has applied, so it must no longer read as 9.
    expect(appliedGainDb({ ...staticBand, dynamic: true })).not.toBeCloseTo(9, 1);

    // The reported gain is the band's gain scaled, so the two multiply.
    expect(appliedGainDb(staticBand, 0.5)).toBeCloseTo(4.5, 3);

    // Only the band that was set is configured.
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    eq.setBand(0, { ...staticBand });
    eq.processMono(generateSine(1000, sampleRate, 512 / sampleRate));
    expect(eq.spectrum().bandGainDb[1]).toBe(0);
    eq.destroy();

    // frequencyHz and type shape the audio rather than the reported gain: a
    // +9 dB shelf at 6 kHz lifts an 8 kHz tone and leaves a 200 Hz tone alone,
    // and a low shelf does the opposite.
    const rms = (data: ArrayLike<number>): number => {
      let sum = 0;
      for (let i = 0; i < data.length; i++) {
        sum += data[i] * data[i];
      }
      return Math.sqrt(sum / data.length);
    };
    const shelfGainDb = (band: Record<string, unknown>, toneHz: number): number => {
      const shelf = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        shelf.setBand(0, band as never);
        const tone = generateSine(toneHz, sampleRate, 512 / sampleRate);
        let out: Float32Array = tone;
        // Let the filter state settle before measuring.
        for (let i = 0; i < 4; i++) {
          out = shelf.processMono(tone);
        }
        return 20 * Math.log10(rms(out) / rms(tone));
      } finally {
        shelf.destroy();
      }
    };

    // Stated as the difference between the two ends rather than as an absolute
    // level at each, so the assertion tracks where the shelf sits instead of the
    // exact skirt of whatever coeffMode / q / slope the band was given.
    const lowShelf = { ...staticBand, type: 'LowShelf' };
    expect(shelfGainDb(staticBand, 8000)).toBeGreaterThan(shelfGainDb(staticBand, 200) + 6);
    expect(shelfGainDb(lowShelf, 200)).toBeGreaterThan(shelfGainDb(lowShelf, 8000) + 6);

    // The four string-valued keys reach their parsers rather than silently
    // falling back: an unknown spelling is rejected, which a reader that fell
    // back to the default could not do.
    const strings = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    expect(() => strings.setBand(0, { ...staticBand, type: 'NotAType' as never })).toThrow();
    expect(() => strings.setBand(0, { ...staticBand, coeffMode: 'NotAMode' as never })).toThrow();
    expect(() =>
      strings.setBand(0, { ...staticBand, placement: 'NotAPlacement' as never }),
    ).toThrow();
    expect(() => strings.setBand(0, { ...staticBand, phase: 'NotAPhase' as never })).toThrow();
    strings.destroy();
  });

  it('uses the prepared sample rate for match when options omit sampleRate', () => {
    const sampleRate = 44100;
    const source = generateSine(1000, sampleRate, 0.25);
    const reference = generateSine(2000, sampleRate, 0.25);
    const omitted = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    const explicit = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });

    omitted.match(source, reference, { maxBands: 6 });
    explicit.match(source, reference, { sampleRate, maxBands: 6 });

    const omittedGain = Array.from(omitted.spectrum().bandGainDb);
    const explicitGain = Array.from(explicit.spectrum().bandGainDb);
    expect(omittedGain.length).toBe(explicitGain.length);
    for (let i = 0; i < omittedGain.length; i += 1) {
      expect(omittedGain[i]).toBeCloseTo(explicitGain[i], 6);
    }
  });
});

describe('streaming handles signal use-after-destroy', () => {
  // The classes document "Idempotent; any other method called afterwards
  // throws". A reader that answers with a plausible measurement instead is
  // indistinguishable from a live handle at the call site.
  it('throws from every StreamingMasteringChain reader after destroy()', () => {
    const chain = new StreamingMasteringChain({ 'maximizer.truePeakLimiter.enabled': true });
    chain.prepare(48000, 64, 1);
    expect(chain.latencySamples()).toBeGreaterThan(0);
    chain.destroy();
    expect(() => chain.latencySamples()).toThrow(/not initialized/);
    expect(() => chain.stageNames()).toThrow(/not initialized/);
    expect(() => chain.processMono(new Float32Array(64))).toThrow(/not initialized/);
    expect(() => chain.reset()).toThrow(/not initialized/);
    // A second destroy() stays a no-op.
    expect(() => chain.destroy()).not.toThrow();
  });

  it('throws from every StreamingEqualizer reader after destroy()', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'Peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true });
    expect(eq.lastAutoGainDb()).toBe(0);
    eq.destroy();
    expect(() => eq.latencySamples()).toThrow(/not initialized/);
    expect(() => eq.lastAutoGainDb()).toThrow(/not initialized/);
    expect(() => eq.clearSidechain()).toThrow(/not initialized/);
    expect(() => eq.processMono(new Float32Array(512))).toThrow(/not initialized/);
    expect(() => eq.destroy()).not.toThrow();
  });
});

describe('onset, tempogram, NNLS chroma, and LUFS', () => {
  const allFinite = (arr: Float32Array): boolean => {
    for (let i = 0; i < arr.length; i++) {
      if (!Number.isFinite(arr[i])) {
        return false;
      }
    }
    return true;
  };

  it('onsetEnvelope returns a finite Float32Array', () => {
    const env = onsetEnvelope(generateSine(440, SR, 2.0), SR);
    expect(env).toBeInstanceOf(Float32Array);
    expect(env.length).toBeGreaterThan(0);
    expect(allFinite(env)).toBe(true);
  });

  it('fourierTempogram returns an [nBins x nFrames] matrix', () => {
    const env = onsetEnvelope(generateSine(440, SR, 2.0), SR);
    const winLength = 384;
    const result = fourierTempogram(env, SR, 512, winLength);
    expect(result.nFrames).toBe(env.length);
    const expectedBins = Math.floor(winLength / 2) + 1;
    expect(result.nBins).toBe(expectedBins);
    expect(result.data).toBeInstanceOf(Float32Array);
    expect(result.data.length).toBe(result.nBins * result.nFrames);
    expect(allFinite(result.data)).toBe(true);
  });

  it('tempogramRatio returns one finite value per default factor', () => {
    const env = onsetEnvelope(generateSine(440, SR, 2.0), SR);
    const tg = tempogram(env, SR);
    const ratio = tempogramRatio(tg.data, tg.winLength, SR);
    expect(ratio).toBeInstanceOf(Float32Array);
    expect(ratio.length).toBe(5); // {0.5, 1, 2, 3, 4}
    expect(allFinite(ratio)).toBe(true);
  });

  it('tempogram cosine mode is exposed', () => {
    const onset = new Float32Array([0.2, 1.0, 0.4, 0.0, 0.8, 0.1, 0.5, 0.3]);
    const result = tempogram(onset, SR, 1, 4, 'cosine');
    expect(result.nFrames).toBe(onset.length);
    expect(result.winLength).toBe(4);
    expect(result.data.length).toBe(4 * onset.length);
    expect(allFinite(result.data)).toBe(true);
    for (const value of result.data) {
      expect(value).toBeGreaterThanOrEqual(-1.000001);
      expect(value).toBeLessThanOrEqual(1.000001);
    }
  });

  it('tempogram and Fourier tempogram forward center and norm', () => {
    const onset = new Float32Array([0.2, 1.0, 0.4, 0.0, 0.8, 0.1, 0.5, 0.3]);
    expect(
      tempogram({
        onsetEnvelope: onset,
        sampleRate: SR,
        hopLength: 1,
        winLength: 4,
        center: false,
        norm: false,
      }),
    ).toEqual(tempogram(onset, SR, 1, 4, 'autocorrelation', false, false));
    expect(
      fourierTempogram({
        onsetEnvelope: onset,
        sampleRate: SR,
        hopLength: 1,
        winLength: 4,
        center: false,
        norm: false,
      }),
    ).toEqual(fourierTempogram(onset, SR, 1, 4, false, false));
  });

  it('nnlsChroma returns a 12 x nFrames matrix', () => {
    const result = nnlsChroma(generateSine(440, SR, 2.0), SR);
    expect(result.nChroma).toBe(12);
    expect(result.nFrames).toBeGreaterThan(0);
    expect(result.data).toBeInstanceOf(Float32Array);
    expect(result.data.length).toBe(12 * result.nFrames);
    expect(allFinite(result.data)).toBe(true);
  });

  it('lufs returns finite measures; louder reads higher', () => {
    const loudSamples = generateSine(440, 48000, 3.0);
    const quietSamples = loudSamples.map((s) => s * 0.1);

    const loud = lufs(loudSamples, 48000);
    const quiet = lufs(quietSamples, 48000);

    for (const r of [loud, quiet]) {
      expect(Number.isFinite(r.integratedLufs)).toBe(true);
      expect(Number.isFinite(r.momentaryLufs)).toBe(true);
      expect(Number.isFinite(r.shortTermLufs)).toBe(true);
      expect(Number.isFinite(r.loudnessRange)).toBe(true);
      expect(r.loudnessRange).toBeGreaterThanOrEqual(0);
    }
    expect(loud.integratedLufs).toBeGreaterThan(quiet.integratedLufs);
  });

  it('momentaryLufs and shortTermLufs return finite time series', () => {
    const samples = generateSine(440, 48000, 3.0);

    const momentary = momentaryLufs(samples, 48000);
    expect(momentary).toBeInstanceOf(Float32Array);
    expect(momentary.length).toBeGreaterThan(0);
    expect(allFinite(momentary)).toBe(true);

    const shortTerm = shortTermLufs(samples, 48000);
    expect(shortTerm).toBeInstanceOf(Float32Array);
    expect(shortTerm.length).toBeGreaterThan(0);
    expect(allFinite(shortTerm)).toBe(true);
  });

  it('Audio methods mirror standalone onset/chroma/LUFS functions', () => {
    const audio = Audio.fromBuffer(generateSine(440, 48000, 3.0), 48000);
    try {
      const env = audio.onsetEnvelope();
      expect(env).toBeInstanceOf(Float32Array);
      expect(env.length).toBeGreaterThan(0);

      const chromaResult = audio.nnlsChroma();
      expect(chromaResult.nChroma).toBe(12);
      expect(chromaResult.data.length).toBe(12 * chromaResult.nFrames);

      const loud = audio.lufs();
      expect(Number.isFinite(loud.integratedLufs)).toBe(true);
      expect(Number.isFinite(loud.loudnessRange)).toBe(true);

      expect(audio.momentaryLufs().length).toBeGreaterThan(0);
      expect(audio.shortTermLufs().length).toBeGreaterThan(0);
    } finally {
      audio.destroy();
    }
  });
});

describe('StreamAnalyzer quantize-config override', () => {
  it('uses feature flags and empty arrays for every disabled-feature combination and read type', () => {
    const input = new Float32Array(32);
    input[8] = 0.5;
    for (let mask = 0; mask < 16; mask += 1) {
      const config = {
        sampleRate: 8000,
        nFft: 32,
        hopLength: 32,
        nMels: 8,
        computeMel: (mask & 1) !== 0,
        computeChroma: (mask & 2) !== 0,
        computeOnset: (mask & 4) !== 0,
        computeSpectral: (mask & 8) !== 0,
      };
      for (let readType = 0; readType < 3; readType += 1) {
        const analyzer = new StreamAnalyzer(config);
        analyzer.process(input);
        const frames =
          readType === 0
            ? analyzer.readFrames(1)
            : readType === 1
              ? analyzer.readFramesU8(1)
              : analyzer.readFramesI16(1);
        expect(frames.nFrames).toBe(1);
        expect(frames.featureFlags).toBe(mask);
        expect(frames.nMels).toBe(mask & 1 ? 8 : 0);
        expect(frames.nChroma).toBe(mask & 2 ? 12 : 0);
        expect(frames.mel.length).toBe(mask & 1 ? 8 : 0);
        expect(frames.chroma.length).toBe(mask & 2 ? 12 : 0);
        expect(frames.onsetStrength.length).toBe(mask & 4 ? 1 : 0);
        expect(frames.rmsEnergy.length).toBe(1);
        expect(frames.spectralCentroid.length).toBe(mask & 8 ? 1 : 0);
        expect(frames.spectralFlatness.length).toBe(mask & 8 ? 1 : 0);
      }
    }
  });

  it('rejects legacy outputFormat selectors', () => {
    expect(() => new StreamAnalyzer({ outputFormat: 1 })).toThrow();
    expect(() => new StreamAnalyzer({ outputFormat: 2 })).toThrow();
    for (const value of [0.5, Number.NaN, Number.POSITIVE_INFINITY, '0']) {
      expect(() => new StreamAnalyzer({ outputFormat: value as unknown as number })).toThrow();
    }
  });

  it('widens the saturating quantization range', () => {
    const config = { sampleRate: SR, nFft: 1024, hopLength: 256, nMels: 32, window: 1 };

    // A tiny centroidMax saturates the (positive) spectral centroid to the u8
    // maximum; a huge centroidMax collapses it toward zero. Reading identical
    // audio with the two configs must differ, proving the override reaches the
    // native quantizer.
    const tight = new StreamAnalyzer(config);
    const wide = new StreamAnalyzer(config);
    tight.process(generateSine(440, SR, 0.5));
    const narrow = tight.readFramesU8(4, { centroidMax: 1.0 });

    wide.process(generateSine(440, SR, 0.5));
    const broad = wide.readFramesU8(4, { centroidMax: 1e9 });

    expect(narrow.nFrames).toBe(broad.nFrames);
    expect(narrow.nFrames).toBeGreaterThan(0);
    expect(narrow.spectralCentroid[0]).toBe(255); // saturated by the narrow range
    expect(Array.from(narrow.spectralCentroid)).not.toEqual(Array.from(broad.spectralCentroid));

    // Omitting the config keeps the default ranges (no throw, same shape).
    const fallback = wide.readFramesU8(4);
    expect(fallback.nMels).toBe(32);
  });

  it('rejects malformed config geometry like the C ABI', () => {
    // The shared StreamAnalyzer constructor enforces the same relationship and
    // positive-value contract as the flat C ABI, so direct Node construction
    // throws instead of silently producing garbage spectra.
    expect(() => new StreamAnalyzer({ sampleRate: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nFft: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nMels: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nFft: 1024, hopLength: 2048 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, fmin: 8000, fmax: 4000 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, maxProgressionEntries: 0 })).toThrow();
  });

  it('accepts only valid window ordinals at the native boundary', () => {
    for (const window of [0, 1, 2, 3]) {
      const analyzer = new StreamAnalyzer({
        sampleRate: 8000,
        nFft: 32,
        hopLength: 32,
        nMels: 8,
        window,
      });
      expect(analyzer.sampleRate()).toBe(8000);
      analyzer.destroy();
    }

    const explicitUndefined = new StreamAnalyzer({ window: undefined });
    explicitUndefined.destroy();

    const invalidWindows: unknown[] = [
      -1,
      4,
      99,
      0.5,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      '0',
      true,
      null,
      {},
      [],
      new Number(1),
    ];
    for (const window of invalidWindows) {
      expect(() => new StreamAnalyzer({ window: window as number })).toThrow(RangeError);
    }
  });

  it('bounds unread frames and reports drop-newest telemetry', () => {
    const analyzer = new StreamAnalyzer({
      sampleRate: 8000,
      nFft: 32,
      hopLength: 32,
      nMels: 8,
      maxPendingFrames: 3,
      maxProgressionEntries: 3,
    });
    analyzer.process(new Float32Array(32 * 64));
    const stats = analyzer.stats();
    expect(stats.pendingFrames).toBe(3);
    expect(stats.droppedOutputFrames).toBeGreaterThan(0);
    expect(stats.pendingFrames + stats.droppedOutputFrames).toBe(stats.totalFrames);
    expect(stats.droppedChordProgressionEntries).toBe(0);
    expect(stats.droppedBarProgressionEntries).toBe(0);
  });
});

describe('StreamAnalyzer external offsets', () => {
  it('preserves buffered timestamps and rejects discontinuities until reset', () => {
    const config = { sampleRate: SR, nFft: 2048, hopLength: 512 };
    const audio = new Float32Array(4096);
    const reference = new StreamAnalyzer(config);
    reference.process(audio);
    const expected = reference.readFrames(64).timestamps;

    const analyzer = new StreamAnalyzer(config);
    for (let offset = 0; offset < audio.length; offset += 128) {
      analyzer.processWithOffset(audio.subarray(offset, offset + 128), offset);
    }
    expect(Array.from(analyzer.readFrames(64).timestamps)).toEqual(Array.from(expected));
    analyzer.reset();
    analyzer.processWithOffset(audio.subarray(0, 128), 1000);
    expect(() => analyzer.processWithOffset(audio.subarray(128, 256), 1129)).toThrow();
    analyzer.reset(5000);
    expect(() => analyzer.processWithOffset(audio.subarray(0, 128), 5000)).not.toThrow();
  });

  it('rejects invalid size arguments without integer truncation or wrapping', () => {
    const analyzer = new StreamAnalyzer({ sampleRate: 8000, nFft: 32, hopLength: 32, nMels: 8 });
    const invalidValues = [
      -1,
      1.5,
      Number.MAX_SAFE_INTEGER + 1,
      Number.POSITIVE_INFINITY,
      Number.NaN,
    ];

    for (const value of invalidValues) {
      expect(() => analyzer.processWithOffset(new Float32Array([0]), value)).toThrow(
        /safe integer/,
      );
      expect(() => analyzer.readFramesSoa(value)).toThrow(/safe integer/);
      expect(() => analyzer.readFrames(value)).toThrow(/safe integer/);
      expect(() => analyzer.readFramesU8(value)).toThrow(/safe integer/);
      expect(() => analyzer.readFramesI16(value)).toThrow(/safe integer/);
      expect(() => analyzer.reset(value)).toThrow(/safe integer/);
    }

    analyzer.reset(Number.MAX_SAFE_INTEGER);
    expect(() =>
      analyzer.processWithOffset(new Float32Array([0]), Number.MAX_SAFE_INTEGER),
    ).not.toThrow();
    expect(() => analyzer.readFramesSoa(Number.MAX_SAFE_INTEGER)).not.toThrow();
    expect(() => analyzer.readFramesU8(Number.MAX_SAFE_INTEGER)).not.toThrow();
    expect(() => analyzer.readFramesI16(Number.MAX_SAFE_INTEGER)).not.toThrow();
  });
});
