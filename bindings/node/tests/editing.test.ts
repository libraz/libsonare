import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';
import type { VoicePresetId } from '../src/index.js';
import {
  Audio,
  noteMove,
  noteStretch,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchPyin,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  validateRealtimeVoiceChangerPresetJson,
  voiceChange,
  voiceChangeRealtime,
  voiceCharacterPresetId,
} from '../src/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function peak(samples: Float32Array): number {
  let out = 0;
  for (const sample of samples) {
    out = Math.max(out, Math.abs(sample));
  }
  return out;
}

describe('editing effects', () => {
  const tone = generateSine(440, SR, 0.5);

  it('pitchCorrectToMidi reaches the requested pitch with default settings', () => {
    // 440 Hz is MIDI 69 (A4); correct toward MIDI 71 (B4).
    const result = pitchCorrectToMidi(tone, SR, 69, 71);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);

    const detected = pitchPyin(result, SR, 2048, 512, 100, 1000);
    const expectedHz = 440 * 2 ** (2 / 12);
    const centsError = 1200 * Math.log2(detected.medianF0 / expectedHz);
    expect(Math.abs(centsError)).toBeLessThan(5);
  });

  it('rejects out-of-range MIDI / F0 instead of returning garbage', () => {
    // The C ABI and Python reject these; Node previously returned best-effort
    // garbage. Validation now lives in the core (and currentMidi in the wrapper).
    expect(() => pitchCorrectToMidi(tone, SR, 200, 71)).toThrow(); // currentMidi out of range
    expect(() => pitchCorrectToMidi(tone, SR, 69, 200)).toThrow(); // targetMidi out of range
    expect(() => pitchCorrectToMidi(tone, SR, 69, -1)).toThrow();
    const hop = 512;
    const nFrames = Math.floor(tone.length / hop) + 1;
    const badF0 = new Float32Array(nFrames).fill(440);
    badF0[0] = -5; // negative F0
    expect(() => pitchCorrectToMidiTimevarying(tone, badF0, 71, SR, hop)).toThrow();
  });

  it('pitchCorrectToMidiTimevarying follows a caller-supplied F0 contour', () => {
    const hop = 512;
    const nFrames = Math.floor(tone.length / hop) + 1;
    const f0 = new Float32Array(nFrames).fill(440);
    const result = pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBe(tone.length);
    expect(result.every((x) => Number.isFinite(x))).toBe(true);

    // Optional voiced / voicedProb arrays are accepted.
    const voiced = new Int32Array(nFrames).fill(1);
    const voicedProb = new Float32Array(nFrames).fill(1);
    const result2 = pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop, voiced, voicedProb);
    expect(result2.length).toBe(tone.length);

    // pYIN emits NaN F0 for unvoiced frames by default.
    f0[0] = Number.NaN;
    voiced[0] = 0;
    const pyinResult = pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop, voiced, voicedProb);
    expect(pyinResult.every((x) => Number.isFinite(x))).toBe(true);
  });

  it('rejects mismatched pitch-track companion arrays before native reads', () => {
    const hop = 512;
    const nFrames = Math.floor(tone.length / hop) + 1;
    const f0 = new Float32Array(nFrames).fill(440);
    const shortVoiced = new Int32Array(nFrames - 1).fill(1);
    const shortProb = new Float32Array(nFrames - 1).fill(1);

    expect(() => pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop, shortVoiced)).toThrow(
      /same length/,
    );
    expect(() =>
      pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop, undefined, shortProb),
    ).toThrow(/same length/);
    expect(() => pitchCorrectTimevarying(tone, f0, SR, hop, { voiced: shortVoiced })).toThrow(
      /same length/,
    );
    expect(() => pitchCorrectTimevarying(tone, f0, SR, hop, { voicedProb: shortProb })).toThrow(
      /same length/,
    );
  });

  it('pitchCorrectTimevarying supports scale mode and retune-strength knobs', () => {
    const hop = 512;
    const nFrames = Math.floor(tone.length / hop) + 1;
    const f0 = new Float32Array(nFrames).fill(440);

    // Default (no options) behaves like the fixed-MIDI path and stays finite.
    const base = pitchCorrectTimevarying(tone, f0, SR, hop);
    expect(base.length).toBe(tone.length);
    expect(base.every((x) => Number.isFinite(x))).toBe(true);

    // Scale mode snaps to the configured key without throwing.
    const scaled = pitchCorrectTimevarying(tone, f0, SR, hop, { mode: 'scale', scaleRoot: 0 });
    expect(scaled.length).toBe(tone.length);
    expect(scaled.every((x) => Number.isFinite(x))).toBe(true);

    // A lower retuneAmount produces a different correction than a full snap.
    const full = pitchCorrectTimevarying(tone, f0, SR, hop, { targetMidi: 71, retuneAmount: 1 });
    const gentle = pitchCorrectTimevarying(tone, f0, SR, hop, {
      targetMidi: 71,
      retuneAmount: 0.25,
    });
    expect(full.some((x, i) => Math.abs(x - gentle[i]) > 1e-6)).toBe(true);

    // Out-of-range knobs are rejected.
    expect(() =>
      pitchCorrectTimevarying(tone, f0, SR, hop, { targetMidi: 71, retuneAmount: 2 }),
    ).toThrow();
    expect(() => pitchCorrectTimevarying(tone, f0, SR, hop, { targetMidi: 200 })).toThrow();
    expect(() => pitchCorrectTimevarying(tone, f0, SR, hop, { mode: 'Scale' as never })).toThrow(
      /pitch correction mode/,
    );
    expect(() =>
      pitchCorrectTimevarying(tone, f0, SR, hop, { maxCorrectionSemitones: -1 }),
    ).toThrow();
    expect(() =>
      pitchCorrectTimevarying(tone, f0, SR, hop, { retuneSpeedMs: Number.NaN }),
    ).toThrow();
    expect(() => pitchCorrectTimevarying(tone, f0, SR, hop, { scaleModeMask: 0 })).toThrow();
    expect(() => pitchCorrectTimevarying(tone, new Float32Array(), SR, hop)).toThrow();
    expect(() => pitchCorrectTimevarying(tone, f0, SR, 0)).toThrow();
  });

  it('noteStretch returns a non-empty Float32Array', () => {
    const result = noteStretch(tone, SR, {
      onsetSample: 0,
      offsetSample: tone.length,
      stretchRatio: 1.5,
    });
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('noteStretch and noteMove default offset to the input length', () => {
    const stretched = noteStretch(tone, SR);
    const moved = noteMove(tone, SR);
    expect(stretched).toBeInstanceOf(Float32Array);
    expect(stretched.length).toBeGreaterThan(0);
    expect(moved).toBeInstanceOf(Float32Array);
    expect(moved.length).toBe(tone.length);
  });

  it('noteMove preserves output length', () => {
    const result = noteMove(tone, SR, {
      onsetSample: 100,
      offsetSample: 1000,
      targetOnsetSample: 500,
    });
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBe(tone.length);
  });

  it('voiceChange returns a non-empty Float32Array', () => {
    const result = voiceChange(tone, SR, { pitchSemitones: 2, formantFactor: 1.1 });
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('RealtimeVoiceChanger processes blocks and exposes presets', () => {
    const changer = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 128,
      channels: 1,
      preset: 'bright-idol',
    });
    const input = tone.subarray(0, 128);
    const output = new Float32Array(input.length);
    changer.processMonoInto(input, output);
    expect(output.some((sample) => Number.isFinite(sample))).toBe(true);
    expect(changer.latencySamples()).toBeGreaterThan(0);
    changer.setConfig('deep-narrator');
    expect(changer.configJson()).toContain('retune');
    changer.destroy();

    const offline = voiceChangeRealtime(tone.subarray(0, 512), SR, 'soft-whisper');
    expect(offline).toBeInstanceOf(Float32Array);
    expect(offline.length).toBe(512);
    expect(voiceChangeRealtime(tone.subarray(0, 512), SR, 'soft-whisper')).toEqual(offline);

    const uncompensated = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 512,
      channels: 1,
      preset: 'soft-whisper',
    });
    const latency = uncompensated.latencySamples();
    expect(latency).toBeGreaterThan(0);
    const raw = uncompensated.processMono(tone.subarray(0, 512));
    uncompensated.destroy();
    expect(peak(raw.subarray(0, latency))).toBeLessThan(1e-4);
    expect(peak(offline.subarray(0, latency))).toBeGreaterThan(1e-3);

    // Interleaved stereo path (mirrors the WASM voiceChangeRealtime channels
    // option): a 512-frame stereo buffer is 1024 interleaved samples.
    const stereoIn = new Float32Array(1024);
    for (let i = 0; i < 512; i++) {
      stereoIn[i * 2] = tone[i] ?? 0;
      stereoIn[i * 2 + 1] = tone[i] ?? 0;
    }
    const stereoOut = voiceChangeRealtime(stereoIn, SR, 'soft-whisper', { channels: 2 });
    expect(stereoOut.length).toBe(1024);
    expect(() =>
      voiceChangeRealtime(new Float32Array(3), SR, 'soft-whisper', { channels: 2 }),
    ).toThrow(/multiple of 2/);
    expect(() =>
      voiceChangeRealtime(stereoIn, SR, 'soft-whisper', { channels: 3 as unknown as 2 }),
    ).toThrow(/channels must be 1 or 2/);
    expect(realtimeVoiceChangerPresetNames()).toContain('robot-mascot');
    const presetJson = realtimeVoiceChangerPresetJson('bright-idol');
    expect(presetJson).toContain('bright-idol');
    expect(validateRealtimeVoiceChangerPresetJson(presetJson).ok).toBe(true);
    expect(validateRealtimeVoiceChangerPresetJson('{}').ok).toBe(false);

    // Unknown preset name throws (mirrors WASM/Python) rather than passing an
    // undefined ordinal to the native call.
    expect(() => voiceCharacterPresetId('not-a-preset' as never)).toThrow(
      /Unknown voice character preset/,
    );
  });

  it('RealtimeVoiceChanger sanitizes non-finite input without poisoning IIR state', () => {
    const changer = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 128,
      channels: 1,
      preset: 'neutral-monitor',
    });
    try {
      // The live block path has no JS-side finite preflight (it is the RT hot
      // path), so a NaN/Inf sample reaches the core, which must flush it to a
      // finite value in place before it recirculates through the filters.
      const bad = tone.subarray(0, 128).slice();
      bad[64] = Number.NaN;
      bad[65] = Number.POSITIVE_INFINITY;
      const out = changer.processMono(bad);
      expect(out.every((sample) => Number.isFinite(sample))).toBe(true);
      // A subsequent clean block must also stay finite: a single upstream NaN
      // must not permanently poison the recirculating filter / retune state.
      const clean = changer.processMono(tone.subarray(128, 256));
      expect(clean.every((sample) => Number.isFinite(sample))).toBe(true);
    } finally {
      changer.destroy();
    }
  });

  it('RealtimeVoiceChanger destroy releases native state and is idempotent', () => {
    const changer = new RealtimeVoiceChanger({
      sampleRate: 48000,
      maxBlockSize: 128,
      channels: 1,
      preset: 'neutral-monitor',
    });
    changer.destroy();
    expect(() => changer.processMono(new Float32Array(16))).toThrow(/destroyed/);
    expect(() => changer.reset()).toThrow(/destroyed/);
    expect(() => changer.configJson()).toThrow(/destroyed/);
    expect(() => changer.latencySamples()).toThrow(/destroyed/);
    expect(() => changer.setConfig('neutral-monitor')).toThrow(/destroyed/);
    expect(() => changer.destroy()).not.toThrow();
    expect(() => changer[Symbol.dispose]()).not.toThrow();
  });

  it('exposes editing methods on the Audio class', () => {
    const audio = Audio.fromBuffer(tone, SR);
    expect(audio.pitchCorrectToMidi(69, 71)).toBeInstanceOf(Float32Array);
    expect(
      audio.noteStretch({ onsetSample: 0, offsetSample: audio.getLength(), stretchRatio: 1.5 }),
    ).toBeInstanceOf(Float32Array);
    expect(audio.voiceChange({ pitchSemitones: 2, formantFactor: 1.1 })).toBeInstanceOf(
      Float32Array,
    );
    audio.destroy();
  });

  it('mono RealtimeVoiceChanger with wetMix=0 returns aligned dry input (C-1 regression)', () => {
    // The strict input contract accepts the complete public flat POD. The
    // chain must short-circuit to the dry signal when only wetMix changes.
    const preset = realtimeVoiceChangerPresetConfig('neutral-monitor');
    preset.wetMix = 0;
    const changer = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 128,
      channels: 1,
      preset,
    });
    // Stay below the final limiter's -1 dBTP ceiling: this checks the dry
    // path itself, rather than intentional output protection.
    const input = tone.subarray(0, 128).map((sample) => sample * 0.1);
    const latency = changer.latencySamples();
    const output = new Float32Array(input.length);
    const captured = new Float32Array((Math.ceil(latency / input.length) + 1) * input.length);
    for (let block = 0; block * input.length < captured.length; block++) {
      changer.processMonoInto(block === 0 ? input : new Float32Array(input.length), output);
      captured.set(output, block * input.length);
    }
    for (let i = 0; i < input.length; ++i) {
      expect(captured[latency + i]).toBeCloseTo(input[i], 5);
    }
    changer.destroy();
  });

  it('WASM-style validate rejects empty objects and required-field omissions (C-2 regression)', () => {
    // The full validator must catch missing dsp / id / name and unknown keys.
    expect(validateRealtimeVoiceChangerPresetJson('{}').ok).toBe(false);
    expect(
      validateRealtimeVoiceChangerPresetJson('{"schemaVersion":1,"id":"x","name":"x"}').ok,
    ).toBe(false);
    expect(
      validateRealtimeVoiceChangerPresetJson(
        '{"schemaVersion":1,"id":"x","name":"x","category":"custom","dsp":{"retune":{"semitones":0,"mix":0,"grainSize":0},"formant":{"factor":1,"amount":0,"body":0,"brightness":0,"nasal":0},"eq":{"highpassHz":80,"bodyDb":0,"presenceDb":0,"airDb":0},"gate":{"thresholdDb":-55,"attackMs":2,"releaseMs":100,"rangeDb":18},"compressor":{"thresholdDb":-22,"ratio":2.5,"attackMs":6,"releaseMs":90,"makeupGainDb":1},"deesser":{"frequencyHz":7200,"thresholdDb":-28,"ratio":3,"rangeDb":8},"reverb":{"mix":0.04,"timeMs":320,"damping":0.55,"seed":1},"limiter":{"ceilingDb":-1,"releaseMs":50}}}',
      ).ok,
    ).toBe(true);
  });

  describe('RealtimeVoiceChanger error paths', () => {
    it('processMonoInto with oversized block throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        const oversized = new Float32Array(129); // maxBlockSize + 1
        const output = new Float32Array(129);
        expect(() => changer.processMonoInto(oversized, output)).toThrow(/block/);
      } finally {
        changer.destroy();
      }
    });

    it('setConfig with invalid JSON string throws an Error', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        // The native C++ JSON parser rejects malformed JSON and throws.
        expect(() => changer.setConfig('{not valid json}' as unknown as never)).toThrow();
      } finally {
        changer.destroy();
      }
    });

    it('processInterleaved with mismatched channel count throws RangeError', () => {
      // Prepared for 1 channel; passing channels=2 must throw RangeError("invalid channel count").
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        // 128 * 2 interleaved samples but only 1 channel was prepared — native
        // binding checks channels <= channels_ and rejects.
        const interleaved = new Float32Array(128 * 2);
        expect(() => changer.processInterleaved(interleaved, 2)).toThrow(/channel/i);
      } finally {
        changer.destroy();
      }
    });
  });

  describe('RealtimeVoiceChanger.processInterleavedInto', () => {
    const FRAMES = 128;
    const CHANNELS = 2;
    const INTERLEAVED = FRAMES * CHANNELS;

    function makeStereoInterleaved(): Float32Array {
      // Distinct sine for L vs R so the test fails if a channel is dropped /
      // mis-deinterleaved. L: 440 Hz, R: 660 Hz.
      const out = new Float32Array(INTERLEAVED);
      for (let i = 0; i < FRAMES; i++) {
        out[i * 2] = Math.sin((2 * Math.PI * 440 * i) / 48000);
        out[i * 2 + 1] = Math.sin((2 * Math.PI * 660 * i) / 48000) * 0.5;
      }
      return out;
    }

    it('writes into a pre-allocated stereo output without allocating', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'bright-idol',
      });
      try {
        // The voice changer reports a non-trivial latency (== retune grain),
        // so the first few blocks may be all-zero look-ahead. Feed enough
        // blocks to flush past latency_samples() before asserting that the
        // chain has produced non-zero output.
        const latencyFrames = changer.latencySamples();
        const blocksToFlush = Math.ceil(latencyFrames / FRAMES) + 1;
        const input = makeStereoInterleaved();
        const output = new Float32Array(INTERLEAVED);
        let anyNonZero = false;
        for (let block = 0; block < blocksToFlush; block++) {
          output.fill(0);
          changer.processInterleavedInto(input, CHANNELS, output);
          for (let i = 0; i < INTERLEAVED; i++) {
            expect(Number.isFinite(output[i])).toBe(true);
            if (output[i] !== 0) {
              anyNonZero = true;
            }
          }
        }
        expect(anyNonZero).toBe(true);
      } finally {
        changer.destroy();
      }
    });

    it('with wetMix=0 preserves an aligned interleaved dry buffer', () => {
      // Mirrors the mono C-1 regression with a complete public flat POD.
      const preset = realtimeVoiceChangerPresetConfig('neutral-monitor');
      preset.wetMix = 0;
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset,
      });
      try {
        const input = makeStereoInterleaved();
        // Keep both channels below the final limiter ceiling so equality tests
        // the aligned dry route rather than expected limiting gain.
        for (let i = 0; i < INTERLEAVED; i++) {
          input[i] *= 0.1;
        }
        const output = new Float32Array(INTERLEAVED);
        const latency = changer.latencySamples();
        const frames = INTERLEAVED / CHANNELS;
        const captured = new Float32Array((Math.ceil(latency / frames) + 1) * INTERLEAVED);
        for (let block = 0; block * INTERLEAVED < captured.length; block++) {
          changer.processInterleavedInto(
            block === 0 ? input : new Float32Array(INTERLEAVED),
            CHANNELS,
            output,
          );
          captured.set(output, block * INTERLEAVED);
        }
        for (let i = 0; i < INTERLEAVED; i++) {
          expect(captured[latency * CHANNELS + i]).toBeCloseTo(input[i], 5);
        }
      } finally {
        changer.destroy();
      }
    });

    it('mismatched input/output lengths throw RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        const input = new Float32Array(INTERLEAVED);
        const tooSmall = new Float32Array(INTERLEAVED - 2);
        expect(() => changer.processInterleavedInto(input, CHANNELS, tooSmall)).toThrow(/length/i);
      } finally {
        changer.destroy();
      }
    });

    it('invalid channel count (0 or > prepared channels) throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        const input = new Float32Array(INTERLEAVED);
        const output = new Float32Array(INTERLEAVED);
        // channels=0 is invalid; cast to bypass the 1|2 TS literal type.
        expect(() => changer.processInterleavedInto(input, 0 as unknown as 1 | 2, output)).toThrow(
          /channel/i,
        );
        // channels=3 exceeds prepared (2) and also doesn't divide 256 evenly.
        expect(() => changer.processInterleavedInto(input, 3 as unknown as 1 | 2, output)).toThrow(
          /channel/i,
        );
      } finally {
        changer.destroy();
      }
    });

    it('block exceeding maxBlockSize throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        // (FRAMES + 1) frames * 2 channels exceeds maxBlockSize per-frame budget.
        const oversized = new Float32Array((FRAMES + 1) * CHANNELS);
        const output = new Float32Array((FRAMES + 1) * CHANNELS);
        expect(() => changer.processInterleavedInto(oversized, CHANNELS, output)).toThrow(/block/);
      } finally {
        changer.destroy();
      }
    });
  });
});

// One named operation, one default, wherever it is exposed. The Python handle
// form defaulted to a character preset while every other entry point defaulted
// to the monitoring preset, so the same buffer rendered audibly differently
// depending on which spelling the caller reached for. The expected value comes
// from the shared corpus the Python suite reads, so a change on one surface
// alone fails on the others.
describe('voiceChangeRealtime default preset', () => {
  it('renders what the shared corpus names as the default', () => {
    const expected = (
      JSON.parse(
        readFileSync(
          new URL('../../../tests/conformance/binding_defaults.json', import.meta.url),
          'utf8',
        ),
      ) as { defaults: Record<string, VoicePresetId> }
    ).defaults['voice_change_realtime.preset'];
    expect(expected).toBe('neutral-monitor');

    const sampleRate = 22050;
    const samples = new Float32Array(2048);
    for (let i = 0; i < samples.length; i++) {
      samples[i] = 0.25 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    }
    // A TypeScript default parameter is not introspectable, so compare renders:
    // omitting the preset must produce exactly what naming the default produces.
    expect(voiceChangeRealtime(samples, sampleRate)).toEqual(
      voiceChangeRealtime(samples, sampleRate, expected),
    );
  });
});
