/**
 * Which classes the AudioWorklet entry carries.
 *
 * A processor living in the worklet realm cannot import the root entry without pulling the
 * worker clients into its module graph, so every class designed to run on the audio thread has
 * to be reachable from `./worklet`. The table classifies every class the root exports; a new
 * root class fails here until it is placed, and a placement the entry does not honour fails too.
 */

import { describe, expect, it } from 'vitest';
import * as index from '../dist/index.js';
import * as worklet from '../dist/worklet.js';
import { setupWorklet } from './_worklet_helpers';

/** Every class the root entry exports: `worklet`, or the reason it stays on the main thread. */
const ROOT_CLASS_REALM: Record<string, 'worklet' | string> = {
  Mixer: 'worklet',
  RealtimeVoiceChanger: 'worklet',
  StreamAnalyzer: 'worklet',
  StreamingEqualizer: 'worklet',
  StreamingMasteringChain: 'worklet',
  StreamingRetune: 'worklet',
  Audio: 'decoded-file analysis handle',
  ClipPageProvider: 'fed from the main thread; the worklet reads pages through the clip page ring',
  ClipPageStreamer: 'streams pages from storage on the main thread',
  HrtfSet: 'load-time data handed to a renderer',
  OfflineWorkerClient: 'starts a Worker',
  PlaybackLoudnessMeter: 'measures program loudness ahead of a render',
  PlaybackRenderer: 'copies planes per call; the worklet path is SonarePlaybackWorkletProcessor',
  PolyphonicAnalysis: 'offline analysis result',
  Project: 'document editing',
  ProjectTimeline: 'document editing',
  RealtimeEngine: 'main-thread engine; the worklet side is SonareEngine',
  SampleBank: 'load-time data',
  VocalEditDraft: 'offline edit session',
  VocalEditSession: 'offline edit session',
  VocalEditWorkerClient: 'starts a Worker',
  VocalEditWorkerDraft: 'proxies a Worker',
  VocalEditWorkerSession: 'proxies a Worker',
  VocalRenderJob: 'offline render',
  VocalRenderSnapshot: 'offline render',
};

/** The worklet entry's runtime exports, exactly. */
const WORKLET_VALUE_EXPORTS = [
  'Mixer',
  'RealtimeVoiceChanger',
  'SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS',
  'SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S',
  'SONARE_ENGINE_RING_HEADER_INTS',
  'SONARE_ENGINE_TELEMETRY_RECORD_BYTES',
  'SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION',
  'SONARE_EXTERNAL_MIDI_RING_HEADER_INTS',
  'SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S',
  'SONARE_METER_RING_HEADER_INTS',
  'SONARE_METER_RING_PROTOCOL_VERSION',
  'SONARE_METER_RING_RECORD_FLOATS',
  'SONARE_SCOPE_RING_HEADER_INTS',
  'SONARE_SCOPE_RING_PROTOCOL_VERSION',
  'SONARE_SPECTRUM_RING_HEADER_INTS',
  'SONARE_SPECTRUM_RING_PROTOCOL_VERSION',
  'SonareEngine',
  'SonareEngineTelemetryError',
  'SonareEngineTelemetryType',
  'SonarePlaybackNode',
  'SonarePlaybackWorkletProcessor',
  'SonareRealtimeEngineNode',
  'SonareRealtimeEngineWorkletProcessor',
  'SonareRealtimeVoiceChangerWorkletProcessor',
  'SonareStreamAnalyzerNode',
  'SonareStreamAnalyzerWorkletProcessor',
  'SonareWorkletProcessor',
  'StreamAnalyzer',
  'StreamingEqualizer',
  'StreamingMasteringChain',
  'StreamingRetune',
  'attachOpfsClipStream',
  'clipPageRequestRingBufferByteLength',
  'createClipPageRequestRingBuffer',
  'createEngineTelemetryRingBuffer',
  'createExternalMidiRingBuffer',
  'createMeterRingBuffer',
  'createScopeRingBuffer',
  'createSpectrumRingBuffer',
  'engineTelemetryRingBufferByteLength',
  'externalMidiRingBufferByteLength',
  'init',
  'isInitialized',
  'meterRingBufferByteLength',
  'opfsClipPageWorkerSource',
  'readClipPageRequestRingBuffer',
  'readEngineTelemetryRingBuffer',
  'readExternalMidiRingBuffer',
  'readMeterRingBuffer',
  'readScopeRingBuffer',
  'readSpectrumRingBuffer',
  'registerSonarePlaybackWorkletProcessor',
  'registerSonareRealtimeEngineWorkletProcessor',
  'registerSonareRealtimeVoiceChangerWorkletProcessor',
  'registerSonareStreamAnalyzerWorkletProcessor',
  'registerSonareWorkletProcessor',
  'scopeRingBufferByteLength',
  'spectrumRingBufferByteLength',
];

/** Class-valued exports of @p entry: capitalised functions that are not error classes. */
function classExports(entry: Record<string, unknown>): string[] {
  return Object.entries(entry)
    .filter(([name, value]) => typeof value === 'function' && /^[A-Z]/.test(name))
    .filter(([, value]) => !((value as { prototype: unknown }).prototype instanceof Error))
    .map(([name]) => name)
    .sort();
}

const workletClasses = Object.entries(ROOT_CLASS_REALM)
  .filter(([, realm]) => realm === 'worklet')
  .map(([name]) => name);
const mainThreadClasses = Object.keys(ROOT_CLASS_REALM).filter(
  (name) => !workletClasses.includes(name),
);

describe('the classes the worklet entry carries', () => {
  it('classify every class the root entry exports', () => {
    expect(classExports(index)).toEqual(Object.keys(ROOT_CLASS_REALM).sort());
  });

  it.each(workletClasses)('carries %s', (name) => {
    expect(typeof (worklet as Record<string, unknown>)[name]).toBe('function');
  });

  it.each(mainThreadClasses)('leaves %s on the main thread', (name) => {
    expect((worklet as Record<string, unknown>)[name]).toBeUndefined();
  });

  it('exports exactly the listed runtime names', () => {
    expect(Object.keys(worklet).sort()).toEqual([...WORKLET_VALUE_EXPORTS].sort());
  });
});

describe('the audio-thread classes constructed from the worklet entry', () => {
  setupWorklet();
  const SR = 48000;
  const BLOCK = 128;
  const tone = (length = BLOCK): Float32Array => {
    const samples = new Float32Array(length);
    for (let i = 0; i < length; i += 1) {
      samples[i] = Math.sin((2 * Math.PI * 440 * i) / SR) * 0.25;
    }
    return samples;
  };

  it('StreamingEqualizer processes a block', () => {
    const eq = new worklet.StreamingEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    try {
      expect(eq.processMono(tone())).toHaveLength(BLOCK);
    } finally {
      eq.destroy();
    }
  });

  it('StreamingRetune processes a block', () => {
    const retune = new worklet.StreamingRetune();
    try {
      retune.prepare(SR, BLOCK);
      expect(retune.processMono(tone())).toHaveLength(BLOCK);
    } finally {
      retune.destroy();
    }
  });

  it('RealtimeVoiceChanger processes a block', () => {
    const vc = new worklet.RealtimeVoiceChanger({
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: BLOCK,
    });
    try {
      expect(vc.processMono(tone())).toHaveLength(BLOCK);
    } finally {
      vc.destroy();
    }
  });

  it('StreamAnalyzer produces frames', () => {
    const analyzer = new worklet.StreamAnalyzer({ sampleRate: SR });
    try {
      analyzer.process(tone(SR / 4));
      expect(analyzer.stats().totalFrames).toBeGreaterThan(0);
    } finally {
      analyzer.destroy();
    }
  });

  it('Mixer mixes a block into caller-owned outputs', () => {
    const mixer = new worklet.Mixer({ sampleRate: SR, maxBlockSize: BLOCK });
    try {
      const outLeft = new Float32Array(BLOCK);
      const outRight = new Float32Array(BLOCK);
      mixer.processStereoInto([], [], outLeft, outRight);
      expect(outLeft.every(Number.isFinite)).toBe(true);
    } finally {
      mixer.destroy();
    }
  });
});
