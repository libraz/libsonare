import { afterEach, describe, expect, it } from 'vitest';
import { Mixer, PlaybackRenderer, RealtimeEngine, RealtimeVoiceChanger } from '../dist/index.js';
import { SonarePlaybackWorkletProcessor } from '../dist/worklet.js';
import {
  mixingScenePresetJson,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const SCOPE_RATES = [44100, 48000, 96000];
const BLOCK = 128;

const scope = globalThis as { sampleRate?: unknown };
const hadScopeRate = Object.hasOwn(globalThis, 'sampleRate');
const originalScopeRate = scope.sampleRate;

function setScopeRate(rate: number | undefined): void {
  if (rate === undefined) {
    delete scope.sampleRate;
  } else {
    scope.sampleRate = rate;
  }
}

/** Builds a processor, returns what `read` observes, and always destroys it. */
function observe<T extends { destroy(): void }, R>(build: () => T, read: (p: T) => R): R {
  const processor = build();
  try {
    return read(processor);
  } finally {
    processor.destroy();
  }
}

const SPEAKERS = { target: { kind: 'speakers', layout: 'stereo' } } as const;
const BLOCKS = 16;

/** A deterministic broadband input, block by block. */
function inputBlock(block: number): Float32Array {
  const out = new Float32Array(BLOCK);
  let seed = 12345 + block * 7919;
  for (let i = 0; i < BLOCK; i++) {
    seed = (seed * 1103515245 + 12345) >>> 0;
    out[i] = ((seed >>> 8) / 0x800000 - 1) * 0.25;
  }
  return out;
}

/** Concatenated output of `render` over `blocks` input blocks. */
function renderBlocks(
  render: (input: Float32Array) => Float32Array,
  blocks = BLOCKS,
): Float32Array {
  const out = new Float32Array(BLOCK * blocks);
  for (let block = 0; block < blocks; block++) {
    out.set(render(inputBlock(block)), block * BLOCK);
  }
  return out;
}

/** Largest absolute sample difference, against a tolerance scaled to the signal. */
function expectCloseSignals(actual: Float32Array, expected: Float32Array): void {
  expect(actual.length).toBe(expected.length);
  const scale = expected.reduce((peak, v) => Math.max(peak, Math.abs(v)), 0);
  expect(scale).toBeGreaterThan(0);
  const worst = actual.reduce((peak, v, i) => Math.max(peak, Math.abs(v - expected[i])), 0);
  expect(worst).toBeLessThanOrEqual(scale * 1e-6);
}

function maxDifference(a: Float32Array, b: Float32Array): number {
  return a.reduce((peak, v, i) => Math.max(peak, Math.abs(v - b[i])), 0);
}

/** Read after setupWorklet() has initialized the module. */
const mixerScene = (): string => mixingScenePresetJson('vocalReverbSend');

/** What the mixer processor renders, read through its own process(). */
function mixerProcessorOutput(processor: SonareWorkletProcessor): Float32Array {
  return renderBlocks((input) => {
    const left = new Float32Array(BLOCK);
    const right = new Float32Array(BLOCK);
    processor.process([[input, input]], [[left, right]]);
    return left;
  });
}

/** The same scene rendered by a Mixer built explicitly at `rate`. */
function mixerReference(rate: number): Float32Array {
  const mixer = Mixer.fromSceneJson(mixerScene(), rate, BLOCK);
  try {
    mixer.compile();
    const buffer = mixer.createRealtimeBuffer();
    return renderBlocks((input) => {
      buffer.leftInputs[0].set(input);
      buffer.rightInputs[0].set(input);
      for (let strip = 1; strip < buffer.leftInputs.length; strip++) {
        buffer.leftInputs[strip].fill(0);
        buffer.rightInputs[strip].fill(0);
      }
      buffer.process(BLOCK);
      return buffer.outLeft.slice(0, BLOCK);
    });
  } finally {
    mixer.delete();
  }
}

/** What the voice-changer processor renders, read through its own process(). */
function voiceChangerProcessorOutput(
  processor: SonareRealtimeVoiceChangerWorkletProcessor,
  blocks: number,
): Float32Array {
  return renderBlocks((input) => {
    const output = new Float32Array(BLOCK);
    processor.process([[input]], [[output]]);
    return output;
  }, blocks);
}

/** The same chain rendered by a RealtimeVoiceChanger prepared explicitly at `rate`. */
function voiceChangerReference(
  rate: number,
  blocks: number,
): { output: Float32Array; latency: number } {
  const changer = new RealtimeVoiceChanger('neutral-monitor');
  try {
    changer.prepare(rate, BLOCK, 1);
    const latency = changer.latencySamples();
    return { output: renderBlocks((input) => changer.processMono(input), blocks), latency };
  } finally {
    changer.delete();
  }
}

/** Blocks that cover the changer's latency at every scope rate plus BLOCKS of settled output. */
function voiceChangerBlocks(): number {
  const latency = Math.max(...SCOPE_RATES.map((rate) => voiceChangerReference(rate, 1).latency));
  return Math.ceil(latency / BLOCK) + BLOCKS;
}

function rms(signal: Float32Array): number {
  return Math.sqrt(signal.reduce((sum, v) => sum + v * v, 0) / Math.max(1, signal.length));
}

const cases = [
  {
    name: 'SonareWorkletProcessor',
    make: (sampleRate?: number) =>
      new SonareWorkletProcessor({
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize: BLOCK,
      }),
    rate: (p: SonareWorkletProcessor) => p.sampleRate,
  },
  {
    name: 'SonareRealtimeEngineWorkletProcessor',
    make: (sampleRate?: number) =>
      new SonareRealtimeEngineWorkletProcessor({ sampleRate, blockSize: BLOCK, channelCount: 2 }),
    rate: (p: SonareRealtimeEngineWorkletProcessor) => p.sampleRate,
  },
  {
    name: 'SonareRealtimeVoiceChangerWorkletProcessor',
    make: (sampleRate?: number) =>
      new SonareRealtimeVoiceChangerWorkletProcessor({
        sampleRate,
        blockSize: BLOCK,
        channelCount: 1,
      }),
    rate: (p: SonareRealtimeVoiceChangerWorkletProcessor) =>
      (p as unknown as { sampleRate: number }).sampleRate,
  },
] as const;

describe('worklet processors take their rate from the AudioWorkletGlobalScope', () => {
  setupWorklet();

  afterEach(() => {
    if (hadScopeRate) {
      scope.sampleRate = originalScopeRate;
    } else {
      delete scope.sampleRate;
    }
  });

  for (const { name, make, rate } of cases) {
    describe(name, () => {
      // Fails on a 48000 guess: 44100 and 96000 scopes would read back 48000.
      it('prepares at the scope rate when no sampleRate option is given', () => {
        for (const scopeRate of SCOPE_RATES) {
          setScopeRate(scopeRate);
          expect(observe(() => make(), rate as never)).toBe(scopeRate);
        }
      });

      it('accepts an explicit rate equal to the scope rate', () => {
        for (const scopeRate of SCOPE_RATES) {
          setScopeRate(scopeRate);
          expect(observe(() => make(scopeRate), rate as never)).toBe(scopeRate);
        }
      });

      // Fails on current code: an explicit 48000 in a 44100 scope was accepted.
      it('refuses an explicit rate that differs from the scope rate with a RangeError', () => {
        for (const scopeRate of SCOPE_RATES) {
          setScopeRate(scopeRate);
          const other = SCOPE_RATES.find((candidate) => candidate !== scopeRate) as number;
          expect(() => make(other)).toThrow(RangeError);
          expect(() => make(other)).toThrow(/AudioWorkletGlobalScope rate/);
        }
      });

      // Fails on current code: the missing rate silently became 48000.
      it('refuses to guess a rate outside a worklet scope', () => {
        setScopeRate(undefined);
        expect(() => make()).toThrow(/no sampleRate option/);
      });

      it('takes the explicit rate as given outside a worklet scope', () => {
        setScopeRate(undefined);
        expect(observe(() => make(44100), rate as never)).toBe(44100);
      });
    });
  }

  // The rate field alone is what resolveScopeSampleRate returned; these read the DSP itself.
  describe('the DSP inside each processor runs at the scope rate', () => {
    it('SonareWorkletProcessor renders what a Mixer built at that rate renders', () => {
      const references = SCOPE_RATES.map(mixerReference);
      // The control: the scene renders differently at each rate, so agreement is not vacuous.
      expect(maxDifference(references[0], references[1])).toBeGreaterThan(1e-4);
      SCOPE_RATES.forEach((scopeRate, index) => {
        setScopeRate(scopeRate);
        const rendered = observe(
          () => new SonareWorkletProcessor({ sceneJson: mixerScene(), blockSize: BLOCK }),
          mixerProcessorOutput,
        );
        expectCloseSignals(rendered, references[index]);
      });
    });

    it('SonareRealtimeEngineWorkletProcessor maps beats to samples at that rate', () => {
      for (const scopeRate of SCOPE_RATES) {
        setScopeRate(scopeRate);
        const reference = new RealtimeEngine(scopeRate, BLOCK);
        try {
          const expected = reference.sampleAtPpq(3);
          const actual = observe(
            () => new SonareRealtimeEngineWorkletProcessor({ blockSize: BLOCK, channelCount: 2 }),
            (p) => (p as unknown as { engine: RealtimeEngine }).engine.sampleAtPpq(3),
          );
          expect(actual).toBe(expected);
        } finally {
          reference.destroy();
        }
      }
      // The control: the reference itself moves with the rate.
      const at = (rate: number) => {
        const engine = new RealtimeEngine(rate, BLOCK);
        try {
          return engine.sampleAtPpq(3);
        } finally {
          engine.destroy();
        }
      };
      expect(at(44100)).not.toBe(at(48000));
    });

    it('SonareRealtimeVoiceChangerWorkletProcessor renders what a changer prepared at that rate renders', () => {
      const blocks = voiceChangerBlocks();
      const rendered = SCOPE_RATES.map((rate) => voiceChangerReference(rate, blocks));
      const references = rendered.map((reference) => reference.output);
      // The control: past its latency each reference carries signal, and the rates differ.
      for (const { output, latency } of rendered) {
        expect(rms(output.subarray(latency))).toBeGreaterThan(1e-3);
      }
      expect(maxDifference(references[0], references[1])).toBeGreaterThan(1e-4);
      SCOPE_RATES.forEach((scopeRate, index) => {
        setScopeRate(scopeRate);
        const output = observe(
          () =>
            new SonareRealtimeVoiceChangerWorkletProcessor({ blockSize: BLOCK, channelCount: 1 }),
          (processor) => voiceChangerProcessorOutput(processor, blocks),
        );
        expectCloseSignals(output, references[index]);
      });
    });
  });

  describe('SonarePlaybackWorkletProcessor', () => {
    const make = (sampleRate?: number, port?: { postMessage: (message: unknown) => void }) =>
      new SonarePlaybackWorkletProcessor({ config: SPEAKERS, sampleRate }, port);

    /** A reference renderer prepared explicitly at `rate`. */
    function referenceLatency(rate: number): number {
      const renderer = new PlaybackRenderer({ config: SPEAKERS, sampleRate: rate });
      try {
        return renderer.latencySamples();
      } finally {
        renderer.delete();
      }
    }

    /** The processor's own diagnostics latency, where its prepared rate shows. */
    function latencyOf(sampleRate: number | undefined): number {
      const messages: Array<{ type: string; diagnostics?: { latency: { samples: number } } }> = [];
      const processor = make(sampleRate, {
        postMessage: (message: unknown) => messages.push(message as never),
      });
      try {
        processor.receiveMessage({ type: 'diagnostics' });
        return messages.at(-1)?.diagnostics?.latency.samples as number;
      } finally {
        processor.destroy();
      }
    }

    it('prepares at the scope rate when no sampleRate option is given', () => {
      for (const scopeRate of SCOPE_RATES) {
        setScopeRate(scopeRate);
        expect(latencyOf(undefined)).toBe(referenceLatency(scopeRate));
      }
    });

    it('accepts an explicit rate equal to the scope rate', () => {
      for (const scopeRate of SCOPE_RATES) {
        setScopeRate(scopeRate);
        expect(latencyOf(scopeRate)).toBe(referenceLatency(scopeRate));
      }
    });

    // Fails on current code: the inline fallback honoured any explicit rate.
    it('refuses an explicit rate that differs from the scope rate with a RangeError', () => {
      for (const scopeRate of SCOPE_RATES) {
        setScopeRate(scopeRate);
        const other = SCOPE_RATES.find((candidate) => candidate !== scopeRate) as number;
        expect(() => make(other)).toThrow(RangeError);
      }
    });

    // Fails on current code: the missing rate silently became 48000.
    it('refuses to guess a rate outside a worklet scope', () => {
      setScopeRate(undefined);
      expect(() => make()).toThrow(/no sampleRate option/);
    });
  });
});
