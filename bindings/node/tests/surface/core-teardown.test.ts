import { describe, expect, expectTypeOf, it } from 'vitest';
import * as root from '../../dist/index.js';

type Constructor = { prototype: object };

const SYNC = [
  'Audio',
  'ClipPageProvider',
  'FileClipPageProvider',
  'HrtfSet',
  'Mixer',
  'PlaybackLoudnessMeter',
  'PlaybackRenderer',
  'PolyphonicAnalysis',
  'Project',
  'ProjectTimeline',
  'RealtimeEngine',
  'RealtimeVoiceChanger',
  'SampleBank',
  'StreamAnalyzer',
  'StreamingEqualizer',
  'StreamingMasteringChain',
  'StreamingRetune',
  'VocalEditDraft',
  'VocalEditSession',
  'VocalRenderJob',
  'VocalRenderSnapshot',
] as const;

const RETIRED = ['delete', 'dispose', 'close', 'terminate'] as const;

describe('teardown is destroy() plus Symbol.dispose on every class', () => {
  it.each(SYNC)('%s', (name) => {
    const ctor = (root as unknown as Record<string, Constructor | undefined>)[name];
    expect(typeof ctor, name).toBe('function');
    const proto = (ctor as Constructor).prototype as Record<PropertyKey, unknown>;
    expect(typeof proto.destroy).toBe('function');
    expect(typeof proto[Symbol.dispose]).toBe('function');
    expect(RETIRED.filter((method) => method in proto)).toEqual([]);
  });

  it('no longer declares the retired teardown names', () => {
    const typeOnly = (
      mixer: root.Mixer,
      timeline: root.ProjectTimeline,
      session: root.VocalEditSession,
    ) => {
      // @ts-expect-error delete() is gone; destroy() releases the mixer
      mixer.delete();
      // @ts-expect-error dispose() is gone; destroy() releases the timeline
      timeline.dispose();
      // @ts-expect-error dispose() is gone; destroy() releases the session
      session.dispose();
    };
    expect(typeof typeOnly).toBe('function');
  });
});

describe('ABI probes for optional features', () => {
  it('declare number | undefined, undefined when the feature is compiled out', () => {
    expectTypeOf(root.vocalEditAbiVersion).returns.toEqualTypeOf<number | undefined>();
    expectTypeOf(root.projectAbiVersion).returns.toEqualTypeOf<number | undefined>();
  });

  it('answer a positive integer in the full build', () => {
    for (const value of [root.vocalEditAbiVersion(), root.projectAbiVersion()]) {
      expect(Number.isInteger(value)).toBe(true);
      expect(value).toBeGreaterThan(0);
    }
  });
});
