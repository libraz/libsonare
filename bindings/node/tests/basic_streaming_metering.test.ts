import { describe, expect, it } from 'vitest';
import {
  Audio,
  fourierTempogram,
  lufs,
  masteringChain,
  momentaryLufs,
  nnlsChroma,
  onsetEnvelope,
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
});
