import { afterEach, describe, expect, it } from 'vitest';
import { PlaybackRenderer } from '../dist/index.js';
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
