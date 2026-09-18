/**
 * Runtime coverage for the live Mixer controls exposed in WASM:
 * type contracts of meter/goniometer readers, string-union mapping for
 * pan law / meter tap / automation curve, and solo / solo-safe behavior.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson, mixStereo } from '../dist/index.js';
import { assertStripIndex } from './_helpers';

const SR = 48000;
const BLOCK = 512;

function blockEnergy(r: { left: Float32Array; right: Float32Array }): number {
  let sum = 0;
  for (let i = 0; i < r.left.length; i++) {
    sum += r.left[i] * r.left[i] + r.right[i] * r.right[i];
  }
  return sum;
}

function silentBlocks(count: number): Float32Array[] {
  const blocks: Float32Array[] = [];
  for (let i = 0; i < count; i++) {
    blocks.push(new Float32Array(BLOCK));
  }
  return blocks;
}

function serialTailScene(): string {
  const delay = (ms: number) => ({
    slot: 'post',
    processor: 'effects.delay.stereo',
    params: JSON.stringify({
      delayTimeLMs: ms,
      delayTimeRMs: ms,
      feedback: 0,
      dryWet: 1,
    }),
  });
  return JSON.stringify({
    version: 1,
    strips: [
      {
        id: 'source',
        inserts: [delay(10)],
        sends: [{ id: 'to-aux', destinationBusId: 'aux', sendDb: 0, timing: 'post' }],
      },
    ],
    buses: [
      { id: 'aux', role: 'aux', inserts: [delay(20)] },
      { id: 'master', role: 'master', inserts: [delay(30)] },
    ],
    connections: [{ source: 'aux', destination: 'master' }],
  });
}

describe('Mixer runtime controls (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  describe('type contracts of readers and setters', () => {
    it('forwards asymmetric dual-pan and polarity fields through scene JSON', () => {
      const mixer = Mixer.fromSceneJson(
        JSON.stringify({
          version: 1,
          strips: [
            {
              id: 'asymmetric',
              panMode: 2,
              dualPanLeft: -0.25,
              dualPanRight: 0.75,
              polarityInvertLeft: true,
              polarityInvertRight: false,
            },
          ],
          buses: [{ id: 'master', role: 'master' }],
          connections: [],
        }),
        SR,
        BLOCK,
      );
      try {
        const scene = JSON.parse(mixer.toSceneJson()) as {
          strips: Array<{
            id: string;
            panMode: number;
            dualPanLeft: number;
            dualPanRight: number;
            polarityInvertLeft: boolean;
            polarityInvertRight: boolean;
          }>;
        };
        expect(scene.strips).toHaveLength(1);
        expect(scene.strips[0]).toMatchObject({
          id: 'asymmetric',
          panMode: 2,
          dualPanLeft: -0.25,
          dualPanRight: 0.75,
          polarityInvertLeft: true,
          polarityInvertRight: false,
        });
      } finally {
        mixer.delete();
      }
    });

    it('reports the longest serial send tail', () => {
      const mixer = Mixer.fromSceneJson(serialTailScene(), SR, BLOCK);
      try {
        expect(mixer.tailSamples()).toBeGreaterThan(1440);
      } finally {
        mixer.delete();
      }
    });

    it('exposes setters, meter readers, and goniometer with the documented shapes', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        // stripById resolves a scene id to a numeric index in [0, stripCount()).
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        expect(typeof vocal).toBe('number');
        expect(Number.isInteger(vocal)).toBe(true);
        expect(vocal).toBeGreaterThanOrEqual(0);
        expect(vocal).toBeLessThan(mixer.stripCount());

        // Every new setter must accept its documented arguments without throwing.
        expect(() => mixer.setSoloed(vocal, false)).not.toThrow();
        expect(() => mixer.setSoloSafe(vocal, true)).not.toThrow();
        expect(() => mixer.setPolarityInvert(vocal, false, true)).not.toThrow();
        expect(() => mixer.setPanLaw(vocal, 'const4.5dB')).not.toThrow();
        expect(() => mixer.setChannelDelaySamples(vocal, 0)).not.toThrow();
        expect(() => mixer.setVcaOffsetDb(vocal, -1.5)).not.toThrow();
        expect(() => mixer.setDualPan(vocal, -0.3, 0.4)).not.toThrow();
        expect(() =>
          mixer.setSurroundPan(vocal, { azimuth: -45, divergence: 0.25, lfe: 0.5 }),
        ).not.toThrow();

        const sendIndex = mixer.addSend(vocal, 'rt-send', 'vocal-verb', -20, 'postFader');
        expect(typeof sendIndex).toBe('number');
        expect(Number.isInteger(sendIndex)).toBe(true);
        expect(sendIndex).toBeGreaterThanOrEqual(0);
        expect(() => mixer.setSendDb(vocal, sendIndex, -12)).not.toThrow();

        // Automation schedulers accept the union curve type.
        expect(() => mixer.scheduleFaderAutomation(vocal, 0, -6, 'linear')).not.toThrow();
        expect(() => mixer.schedulePanAutomation(vocal, 0, 0.1, 'exponential')).not.toThrow();
        expect(() => mixer.scheduleWidthAutomation(vocal, 0, 1.2, 'linear')).not.toThrow();
        expect(() =>
          mixer.scheduleSendAutomation(vocal, sendIndex, 0, -18, 'exponential'),
        ).not.toThrow();

        expect(() => mixer.setFaderDb(vocal, Number.NaN)).toThrow();
        expect(() => mixer.setPan(vocal, Number.POSITIVE_INFINITY)).toThrow();
        expect(() => mixer.scheduleFaderAutomation(vocal, 0, Number.NaN, 'linear')).toThrow();
        expect(() => mixer.setFaderDb(vocal, -3)).not.toThrow();
        expect(() => mixer.setPan(vocal, 0.25)).not.toThrow();

        mixer.compile();

        // Drive one block so the meters and goniometer have data.
        const vocalL = new Float32Array(BLOCK);
        const vocalR = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          const v = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
          vocalL[i] = v;
          vocalR[i] = v;
        }
        const returnL = new Float32Array(BLOCK);
        const returnR = new Float32Array(BLOCK);
        mixer.processStereo([vocalL, returnL], [vocalR, returnR]);

        for (const tap of ['preFader', 'postFader'] as const) {
          const meter = mixer.meterTap(vocal, tap);
          expect(typeof meter).toBe('object');
          for (const field of ['peakDbL', 'peakDbR', 'rmsDbL', 'rmsDbR', 'correlation'] as const) {
            expect(Number.isFinite(meter[field])).toBe(true);
          }

          const stripMeter = mixer.stripMeter(vocal, tap);
          expect(Number.isFinite(stripMeter.peakDbL)).toBe(true);
          expect(Number.isFinite(stripMeter.rmsDbL)).toBe(true);
        }

        const goniometer = mixer.readGoniometerLatest(vocal, 8);
        expect(Array.isArray(goniometer)).toBe(true);
        expect(goniometer.length).toBeGreaterThan(0);
        expect(goniometer.length).toBeLessThanOrEqual(8);
        for (const point of goniometer) {
          expect(Number.isFinite(point.left)).toBe(true);
          expect(Number.isFinite(point.right)).toBe(true);
        }

        // maxPoints is a request, not an allocation size. Sizing the working
        // vector from it directly meant a metering UI deriving the count from a
        // window size could take the module down -- and in WASM that is not a
        // catchable error, it is an out-of-memory abort of the whole instance,
        // so there would be nothing left to assert against. The buffer is
        // bounded by the strip's goniometer ring instead. Matches Node
        // (mixing-runtime.test.ts) and Python (test_mixing.py).
        //
        // These run against the built module: they fail against a `dist/` older
        // than the fix in src/wasm/bindings/mixing/mixing_automation.cpp --
        // most likely by aborting rather than by a clean assertion failure.
        const huge = mixer.readGoniometerLatest(vocal, Number.MAX_SAFE_INTEGER);
        expect(Array.isArray(huge)).toBe(true);
        expect(huge.length).toBeGreaterThan(0);
        expect(huge.length).toBeLessThanOrEqual(4096);
        expect(huge.every((point) => Number.isFinite(point.left))).toBe(true);

        // Anything that is not a finite non-negative integer is a catchable
        // error, never a silent clamp to zero and never an abort.
        for (const invalid of [
          -1,
          -0.5,
          1.5,
          Number.NaN,
          Number.POSITIVE_INFINITY,
          Number.MAX_VALUE,
        ]) {
          expect(() => mixer.readGoniometerLatest(vocal, invalid)).toThrow();
        }

        // Zero stays a legal empty read.
        expect(mixer.readGoniometerLatest(vocal, 0)).toEqual([]);
      } finally {
        mixer.delete();
      }
    });

    it('accepts every pan-law, meter-tap, and send-timing string union value', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        for (const law of ['const3dB', 'const4.5dB', 'const6dB', 'linear0dB'] as const) {
          expect(() => mixer.setPanLaw(vocal, law)).not.toThrow();
        }
        for (const timing of ['preFader', 'postFader'] as const) {
          expect(typeof mixer.addSend(vocal, `s-${timing}`, 'vocal-verb', -18, timing)).toBe(
            'number',
          );
        }
        mixer.compile();
        const vocalL = new Float32Array(BLOCK);
        vocalL[0] = 1;
        const out = mixer.processStereo(
          [vocalL, new Float32Array(BLOCK)],
          [vocalL, new Float32Array(BLOCK)],
        );
        // 'const6dB' / 'postFader' resolved end-to-end and produced audio.
        expect(blockEnergy(out)).toBeGreaterThan(0);
        expect(Number.isFinite(mixer.meterTap(vocal, 'preFader').peakDbL)).toBe(true);
      } finally {
        mixer.delete();
      }
    });

    it('removeSend drops a previously-added send and shifts higher indices down', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        // Add two sends to the same bus; remove the first so the second shifts
        // down into index 0.
        const first = mixer.addSend(vocal, 'rt-send-a', 'vocal-verb', -20, 'postFader');
        const second = mixer.addSend(vocal, 'rt-send-b', 'vocal-verb', -24, 'postFader');
        expect(second).toBeGreaterThan(first);

        // setSendDb on the highest index is valid before removal.
        expect(() => mixer.setSendDb(vocal, second, -18)).not.toThrow();

        // Remove the first send: the second shifts down by one, so the old
        // highest index is now out of range and addressing it throws.
        expect(() => mixer.removeSend(vocal, first)).not.toThrow();
        expect(() => mixer.setSendDb(vocal, second, -18)).toThrow();
        // The shifted-down send is still addressable at the lower index.
        expect(() => mixer.setSendDb(vocal, first, -18)).not.toThrow();

        mixer.compile();
        const vocalL = new Float32Array(BLOCK);
        vocalL[0] = 1;
        const out = mixer.processStereo(
          [vocalL, new Float32Array(BLOCK)],
          [vocalL, new Float32Array(BLOCK)],
        );
        expect(blockEnergy(out)).toBeGreaterThan(0);
      } finally {
        mixer.delete();
      }
    });
  });

  describe('non-finite discard telemetry', () => {
    // A +200 dB peaking band whose recursive (biquad) coefficients scale with
    // the boost. A single-strip scene keeps stripIndex fixed at 0.
    function highGainEqScene(): string {
      return JSON.stringify({
        version: 1,
        strips: [
          {
            id: 'src',
            inserts: [
              {
                slot: 'pre',
                processor: 'eq.parametric',
                params: JSON.stringify({
                  'band0.frequencyHz': 1000,
                  'band0.gainDb': 200,
                  'band0.q': 1,
                  'band0.enabled': true,
                }),
              },
            ],
          },
        ],
        buses: [{ id: 'master', role: 'master' }],
      });
    }

    it("reports the EQ discard immediately and the meters' one block later", () => {
      const mixer = Mixer.fromSceneJson(highGainEqScene(), SR, BLOCK);
      try {
        mixer.compile();

        const clean = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          clean[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR);
        }

        // Clean run: the boosted band actively filters the strip, so the zero
        // discard count read below is not vacuous -- the non-zero output
        // energy confirms the EQ's recursive state was actually driven.
        const cleanOut = mixer.processStereo([clean], [clean]);
        expect(blockEnergy(cleanOut)).toBeGreaterThan(0);
        expect(mixer.stripNonFiniteDiscardCount(0)).toBe(0);

        // Poisoned block: 1e35 is finite and float32-representable, so the
        // mixer's own non-finite INPUT guard (sonare_mixer_process_stereo)
        // lets it through -- but multiplied by the band's boosted coefficient
        // it overflows float32 range inside the biquad recursion, corrupting
        // the filter's own state without the input itself ever being NaN or
        // infinite. The overflowed (now infinite) samples continue on to the
        // strip's own pre/post meters downstream in this same block. The EQ
        // checks its own state at the end of its process call, so its share
        // of the count rises on this block.
        const poisoned = clean.slice();
        poisoned[100] = 1e35;
        mixer.processStereo([poisoned], [poisoned]);
        const afterPoisonedBlock = mixer.stripNonFiniteDiscardCount(0);
        expect(afterPoisonedBlock).toBeGreaterThan(0);

        // A meter checks its own loudness state at the TOP of a block, before
        // consuming that block's samples -- so the corruption it absorbed
        // above is only detected (and counted) once the NEXT block runs, even
        // though that block carries no new poison of its own.
        mixer.processStereo([clean], [clean]);
        const afterFollowingBlock = mixer.stripNonFiniteDiscardCount(0);
        expect(afterFollowingBlock).toBeGreaterThan(afterPoisonedBlock);

        // A further clean block adds nothing more: both the EQ and the
        // meters have now fully recovered.
        mixer.processStereo([clean], [clean]);
        expect(mixer.stripNonFiniteDiscardCount(0)).toBe(afterFollowingBlock);
      } finally {
        mixer.delete();
      }
    });

    it('rejects an out-of-range strip index', () => {
      const mixer = Mixer.fromSceneJson(highGainEqScene(), SR, BLOCK);
      try {
        expect(() => mixer.stripNonFiniteDiscardCount(mixer.stripCount())).toThrow();
      } finally {
        mixer.delete();
      }
    });

    it('reports a bus discard for its own meter one block after an upstream overflow reaches it', () => {
      // 'src' has no explicit connection but auto-routes to 'master' (see the
      // asymmetric-strip fixture above); 'master' itself owns no insert, so
      // only its post-insert meter is in play.
      const mixer = Mixer.fromSceneJson(highGainEqScene(), SR, BLOCK);
      try {
        mixer.compile();

        const clean = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          clean[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR);
        }

        const cleanOut = mixer.processStereo([clean], [clean]);
        expect(blockEnergy(cleanOut)).toBeGreaterThan(0);
        expect(mixer.busNonFiniteDiscardCount('master')).toBe(0);

        // Same poison as the strip case: the strip's boosted EQ band
        // overflows to infinity and that signal reaches the master bus this
        // same block. The bus owns no insert of its own, so nothing on it
        // discards yet -- only its meter absorbed the corruption, and a
        // meter's own check runs at the top of its NEXT process call.
        const poisoned = clean.slice();
        poisoned[100] = 1e35;
        mixer.processStereo([poisoned], [poisoned]);
        expect(mixer.busNonFiniteDiscardCount('master')).toBe(0);

        mixer.processStereo([clean], [clean]);
        const afterFollowingBlock = mixer.busNonFiniteDiscardCount('master');
        expect(afterFollowingBlock).toBeGreaterThan(0);

        // One more clean block adds nothing further: the meter has recovered.
        mixer.processStereo([clean], [clean]);
        expect(mixer.busNonFiniteDiscardCount('master')).toBe(afterFollowingBlock);
      } finally {
        mixer.delete();
      }
    });

    it('throws for a bus declared but not yet compiled, rather than reading zero', () => {
      const mixer = Mixer.fromSceneJson(highGainEqScene(), SR, BLOCK);
      try {
        mixer.compile();
        // 'late' is now in the topology, but the graph has not recompiled
        // since, so it has no DSP record yet. Reading zero here would read
        // as a clean bus; it must throw instead.
        mixer.addBus('late', 'aux');
        expect(() => mixer.busNonFiniteDiscardCount('late')).toThrow();
      } finally {
        mixer.delete();
      }
    });

    it('rejects an unknown bus id', () => {
      const mixer = Mixer.fromSceneJson(highGainEqScene(), SR, BLOCK);
      try {
        expect(() => mixer.busNonFiniteDiscardCount('does-not-exist')).toThrow();
      } finally {
        mixer.delete();
      }
    });
  });

  describe('imperative strip topology', () => {
    it('adds a strip to a live mixer', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
      try {
        const before = mixer.stripCount();
        mixer.addStrip('late-arrival');
        expect(mixer.stripCount()).toBe(before + 1);
        expect(mixer.stripById('late-arrival')).toBe(before);
        expect(() => mixer.compile()).not.toThrow();
        // The C call answers a failure with a NULL strip rather than an error
        // code, so a facade that skipped the null check would report both of
        // these as a successful add.
        expect(() => mixer.addStrip('late-arrival')).toThrow(/duplicate strip id/);
        expect(() => mixer.addStrip('over-sampled', { truePeakOversample: 32 })).toThrow(
          /truePeakOversample must be in \[0, 16\]/,
        );
        // A wrong-typed metering field is refused by name rather than substituted.
        expect(() => mixer.addStrip('wrong-type', { lufs: 'yes' as unknown as boolean })).toThrow(
          /lufs/,
        );
        expect(mixer.stripCount()).toBe(before + 1);
      } finally {
        mixer.delete();
      }
    });

    it('refuses a metering argument that is not a plain object', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
      try {
        const before = mixer.stripCount();
        // A wrong-typed BAG would otherwise read as an empty one and put the
        // caller on the full default metering, the field readers' failure mode
        // one level up.
        // @ts-expect-error a non-object metering bag is rejected at runtime
        expect(() => mixer.addStrip('number-bag', 5)).toThrow(/metering must be a plain object/);
        // @ts-expect-error an array is not a plain object, though `typeof []` is 'object'
        expect(() => mixer.addStrip('array-bag', [])).toThrow(/metering must be a plain object/);
        expect(mixer.stripCount()).toBe(before);

        // Absent and explicit undefined are the omitted default, which the scene
        // document spells by carrying no metering object at all.
        mixer.addStrip('omitted');
        mixer.addStrip('explicit-undefined', undefined);
        expect(mixer.stripCount()).toBe(before + 2);
        const strips = (
          JSON.parse(mixer.toSceneJson()) as {
            strips: Array<{ id: string; metering?: Record<string, unknown> }>;
          }
        ).strips;
        expect(strips.find((entry) => entry.id === 'omitted')?.metering).toBeUndefined();
        expect(strips.find((entry) => entry.id === 'explicit-undefined')?.metering).toBeUndefined();
      } finally {
        mixer.delete();
      }
    });

    it('round-trips an added strip metering configuration through the scene', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
      try {
        mixer.addStrip('metered', { lufs: false, truePeakOversample: 8 });
        // The document omits the object at the full default, so a strip added
        // with no metering argument carries none and only the opted-out one shows.
        mixer.addStrip('plain');
        const strips = (
          JSON.parse(mixer.toSceneJson()) as {
            strips: Array<{ id: string; metering?: Record<string, unknown> }>;
          }
        ).strips;
        expect(strips.find((entry) => entry.id === 'metered')?.metering).toEqual({
          enabled: true,
          lufs: false,
          truePeak: true,
          truePeakOversample: 8,
        });
        expect(strips.find((entry) => entry.id === 'plain')?.metering).toBeUndefined();
      } finally {
        mixer.delete();
      }
    });
  });

  describe('solo and solo-safe', () => {
    // The drum strips all route through a shared bus whose inserts (parallel
    // compressor + tape) carry internal state. To compare energy between two
    // input strips without cross-contamination, each measurement uses a fresh
    // mixer with kick soloed and the bus return marked solo-safe.
    function measureWithSoloedKick(feedStripId: string): number {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'), SR, BLOCK);
      try {
        const kick = mixer.stripById('kick');
        const busReturn = mixer.stripById('drum-bus-return');
        const target = mixer.stripById(feedStripId);
        assertStripIndex(kick, 'kick');
        assertStripIndex(busReturn, 'drum-bus-return');
        assertStripIndex(target, feedStripId);
        expect(kick).toBeGreaterThanOrEqual(0);
        expect(busReturn).toBeGreaterThanOrEqual(0);
        expect(target).toBeGreaterThanOrEqual(0);

        // Keep the shared bus return in the path while kick is soloed.
        mixer.setSoloSafe(busReturn, true);
        mixer.setSoloed(kick, true);
        mixer.compile();

        const stripCount = mixer.stripCount();
        const left = silentBlocks(stripCount);
        const right = silentBlocks(stripCount);
        for (let i = 0; i < BLOCK; i++) {
          const v = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR);
          left[target][i] = v;
          right[target][i] = v;
        }

        // Process a few blocks to let the bus inserts settle, sum the tail energy.
        let energy = 0;
        for (let block = 0; block < 8; block++) {
          const out = mixer.processStereo(left, right);
          if (block >= 4) {
            energy += blockEnergy(out);
          }
        }
        return energy;
      } finally {
        mixer.delete();
      }
    }

    it('silences non-soloed strips in the master while solo-safe strips stay audible', () => {
      // Feeding the soloed strip (kick) reaches the master.
      const soloedEnergy = measureWithSoloedKick('kick');
      // Feeding a non-soloed, non-solo-safe source (snare) is implied-muted.
      const mutedEnergy = measureWithSoloedKick('snare');

      expect(soloedEnergy).toBeGreaterThan(1e-6);
      expect(mutedEnergy).toBeLessThan(soloedEnergy * 1e-3);
    });
  });

  describe('one-shot mixStereo pan options', () => {
    // A left-only source tells the two modes apart: Balance leaves it on the
    // left, stereoPan collapses the pair to a centred mono sum that reaches the
    // right. So the right channel alone reports which mode the strip ran in.
    const leftOnly = () => new Float32Array(BLOCK).fill(1);
    const silence = () => new Float32Array(BLOCK);

    it('applies pan and panMode independently of each other', () => {
      // panMode used to be read only when pan was supplied too, so a caller who
      // changed nothing but the mode was mixed in the default one with nothing
      // to show for it. Either option alone has to reach the strip.
      const modeOnly = mixStereo([leftOnly()], [silence()], SR, { panMode: 'stereoPan' });
      expect(modeOnly.right[BLOCK - 1]).toBeGreaterThan(0.1);

      const balance = mixStereo([leftOnly()], [silence()], SR);
      expect(balance.right[BLOCK - 1]).toBe(0);

      // A position-only call is unchanged by the mode now being read outside
      // it: an absent mode keeps what the strip already carries, and a fresh
      // strip already carries Balance.
      const panOnly = mixStereo([leftOnly()], [silence()], SR, { pan: 0.3 });
      const panWithExplicitMode = mixStereo([leftOnly()], [silence()], SR, {
        pan: 0.3,
        panMode: 'balance',
      });
      expect(Array.from(panOnly.left)).toEqual(Array.from(panWithExplicitMode.left));
      expect(Array.from(panOnly.right)).toEqual(Array.from(panWithExplicitMode.right));
    });

    it('applies a mode-only request per strip', () => {
      const perStrip = mixStereo([leftOnly(), leftOnly()], [silence(), silence()], SR, {
        panMode: ['stereoPan', 'stereoPan'],
      });
      expect(perStrip.right[BLOCK - 1]).toBeGreaterThan(0.1);
    });

    it('still rejects an unknown mode supplied without a position', () => {
      // Reading the mode outside the pan guard must not turn a bad value into a
      // silently ignored one.
      expect(() =>
        mixStereo([leftOnly()], [silence()], SR, { panMode: 'unknown' as never }),
      ).toThrow();
      expect(() => mixStereo([leftOnly()], [silence()], SR, { panMode: 99 })).toThrow();
    });
  });
});
