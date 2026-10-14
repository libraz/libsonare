import {
  commitOverwriteWrite,
  commitSonareEngineTelemetryRingWrite,
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareExternalMidiRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  createSonareSpectrumRingBuffer,
  describe,
  engineTelemetryRingFromSharedBuffer,
  expect,
  it,
  Mixer,
  meterRingFromSharedBuffer,
  mixingScenePresetJson,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareEngineCommandRingBuffer,
  pushSonareExternalMidiRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareExternalMidiRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  readSonareSpectrumRingBuffer,
  releaseOverwriteRead,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
  SONARE_EXTERNAL_MIDI_RING_HEADER_INTS,
  SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SCOPE_RING_PROTOCOL_VERSION,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_PROTOCOL_VERSION,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareWorkletProcessor,
  scopeRingFromSharedBuffer,
  setupWorklet,
  sonareClipPageRequestRingBufferByteLength,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareExternalMidiRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  sonareScopeRingBufferByteLength,
  sonareScopeRingRecordFloats,
  sonareSpectrumRingBufferByteLength,
  spectrumRingFromSharedBuffer,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
  writeSonareEngineTelemetryRingBuffer,
} from './_worklet_helpers';

type TelemetryRingWithPublicationWrapped = ReturnType<
  typeof createSonareEngineTelemetryRingBuffer
> & {
  publicationWrapped: Int32Array;
};

function createTelemetryRing(capacity: number): TelemetryRingWithPublicationWrapped {
  return createSonareEngineTelemetryRingBuffer(capacity) as TelemetryRingWithPublicationWrapped;
}

describe('Sonare worklet ring buffers', () => {
  setupWorklet();

  describe('clip-page request ring-buffer (SPSC)', () => {
    it('drains in FIFO order and reports bounded overflow without overwriting requests', () => {
      const ring = createSonareClipPageRequestRingBuffer(2);
      expect(pushSonareClipPageRequestRingBuffer(ring, 10, 3)).toBe(true);
      expect(pushSonareClipPageRequestRingBuffer(ring, 11, 4)).toBe(true);
      expect(pushSonareClipPageRequestRingBuffer(ring, 12, 5)).toBe(false);

      const read = readSonareClipPageRequestRingBuffer(ring);
      expect(read.requests).toEqual([
        { clipId: 10, pageIndex: 3 },
        { clipId: 11, pageIndex: 4 },
      ]);
      expect(read.dropped).toBe(1);
      expect(readSonareClipPageRequestRingBuffer(ring)).toEqual({ requests: [], dropped: 1 });

      expect(pushSonareClipPageRequestRingBuffer(ring, -1, 0)).toBe(false);
      expect(readSonareClipPageRequestRingBuffer(ring).dropped).toBe(2);
    });

    it('accepts a manually constructed buffer with the documented header layout', () => {
      const capacity = 2;
      const sab = new SharedArrayBuffer(sonareClipPageRequestRingBufferByteLength(capacity));
      const header = new Int32Array(sab, 0, SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS);
      const records = new Uint32Array(
        sab,
        SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
        capacity * SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
      );
      // Header layout: [writeIndex, readIndex, capacity, recordUint32s, dropped].
      header[0] = 1;
      header[2] = capacity;
      header[3] = SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S;
      records[0] = 42;
      records[1] = 7;
      expect(
        readSonareClipPageRequestRingBuffer({ sharedBuffer: sab, header, records, capacity }),
      ).toEqual({ requests: [{ clipId: 42, pageIndex: 7 }], dropped: 0 });
      expect(header[1]).toBe(1);
    });
  });

  describe('overwrite SAB layouts and protocol cut', () => {
    it('appends one zeroed claim word per slot and attaches each producer version', () => {
      const capacity = 3;
      const bands = 4;
      const maxPoints = 5;
      const meter = createSonareMeterRingBuffer(capacity);
      const spectrum = createSonareSpectrumRingBuffer(capacity, bands);
      const scope = createSonareScopeRingBuffer(capacity, bands, maxPoints);
      const telemetry = createTelemetryRing(capacity);

      expect(meter.header[3]).toBe(0);
      expect(spectrum.header[4]).toBe(0);
      expect(scope.header[5]).toBe(0);
      expect(telemetry.header[1]).toBe(0);
      expect(readSonareMeterRingBuffer(meter)).toEqual({ nextReadIndex: 0, meters: [] });
      expect(readSonareSpectrumRingBuffer(spectrum)).toEqual({ nextReadIndex: 0, spectra: [] });
      expect(readSonareScopeRingBuffer(scope)).toEqual({ nextReadIndex: 0, scopes: [] });
      expect(readSonareEngineTelemetryRingBuffer(telemetry)).toEqual({
        nextReadIndex: 0,
        telemetry: [],
      });

      const meterStateOffset =
        SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
      const spectrumStateOffset =
        SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * (3 + bands) * Float32Array.BYTES_PER_ELEMENT;
      const scopeStateOffset =
        SONARE_SCOPE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * sonareScopeRingRecordFloats(bands, maxPoints) * Float32Array.BYTES_PER_ELEMENT;
      const telemetryStateOffset =
        SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * SONARE_ENGINE_TELEMETRY_RECORD_BYTES;
      const telemetryPublicationWrappedOffset =
        telemetryStateOffset + capacity * Int32Array.BYTES_PER_ELEMENT;
      expect(meter.slotStates.byteOffset).toBe(meterStateOffset);
      expect(spectrum.slotStates.byteOffset).toBe(spectrumStateOffset);
      expect(scope.slotStates.byteOffset).toBe(scopeStateOffset);
      expect(telemetry.slotStates.byteOffset).toBe(telemetryStateOffset);
      expect(telemetry.publicationWrapped.byteOffset).toBe(telemetryPublicationWrappedOffset);
      expect(telemetry.publicationWrapped.byteLength).toBe(Int32Array.BYTES_PER_ELEMENT);
      expect(telemetry.publicationWrapped).toHaveLength(1);
      expect(meter.sharedBuffer.byteLength).toBe(sonareMeterRingBufferByteLength(capacity));
      expect(spectrum.sharedBuffer.byteLength).toBe(
        sonareSpectrumRingBufferByteLength(capacity, bands),
      );
      expect(scope.sharedBuffer.byteLength).toBe(
        sonareScopeRingBufferByteLength(capacity, bands, maxPoints),
      );
      expect(telemetry.sharedBuffer.byteLength).toBe(
        sonareEngineTelemetryRingBufferByteLength(capacity),
      );
      expect(telemetry.sharedBuffer.byteLength).toBe(
        telemetryPublicationWrappedOffset + Int32Array.BYTES_PER_ELEMENT,
      );
      expect(Array.from(meter.slotStates)).toEqual([0, 0, 0]);
      expect(Array.from(spectrum.slotStates)).toEqual([0, 0, 0]);
      expect(Array.from(scope.slotStates)).toEqual([0, 0, 0]);
      expect(Array.from(telemetry.slotStates)).toEqual([0, 0, 0]);
      expect(Array.from(telemetry.publicationWrapped)).toEqual([0]);

      meterRingFromSharedBuffer(meter.sharedBuffer, capacity);
      spectrumRingFromSharedBuffer(spectrum.sharedBuffer, capacity, bands);
      scopeRingFromSharedBuffer(scope.sharedBuffer, capacity, bands, maxPoints);
      engineTelemetryRingFromSharedBuffer(telemetry.sharedBuffer, capacity);
      expect(meter.header[3]).toBe(SONARE_METER_RING_PROTOCOL_VERSION);
      expect(spectrum.header[4]).toBe(SONARE_SPECTRUM_RING_PROTOCOL_VERSION);
      expect(scope.header[5]).toBe(SONARE_SCOPE_RING_PROTOCOL_VERSION);
      expect(telemetry.header[1]).toBe(SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION);
    });

    it('rejects stale versions, unpublished records, and old record tails without mutation', () => {
      const stale = [
        {
          name: 'meter',
          ring: createSonareMeterRingBuffer(2),
          versionSlot: 3,
        },
        {
          name: 'spectrum',
          ring: createSonareSpectrumRingBuffer(2, 4),
          versionSlot: 4,
        },
        {
          name: 'scope',
          ring: createSonareScopeRingBuffer(2, 4, 3),
          versionSlot: 5,
        },
        {
          name: 'telemetry',
          ring: createTelemetryRing(2),
          versionSlot: 1,
        },
      ] as const;
      for (const entry of stale) {
        entry.ring.header[entry.versionSlot] = 2;
        const before = Array.from(entry.ring.header);
        expect(() => {
          if (entry.name === 'meter') {
            readSonareMeterRingBuffer(entry.ring);
          } else if (entry.name === 'spectrum') {
            readSonareSpectrumRingBuffer(entry.ring);
          } else if (entry.name === 'scope') {
            readSonareScopeRingBuffer(entry.ring);
          } else {
            readSonareEngineTelemetryRingBuffer(entry.ring);
          }
        }).toThrow(/protocol version 2 is incompatible/);
        expect(Array.from(entry.ring.header)).toEqual(before);
      }

      const staleMeter = createSonareMeterRingBuffer(2);
      staleMeter.header[3] = 2;
      const staleMeterBefore = Array.from(staleMeter.header);
      expect(() => meterRingFromSharedBuffer(staleMeter.sharedBuffer, 2)).toThrow(
        /protocol version 2 is incompatible/,
      );
      expect(Array.from(staleMeter.header)).toEqual(staleMeterBefore);

      const staleSpectrum = createSonareSpectrumRingBuffer(2, 4);
      staleSpectrum.header[4] = 2;
      const staleSpectrumBefore = Array.from(staleSpectrum.header);
      expect(() => spectrumRingFromSharedBuffer(staleSpectrum.sharedBuffer, 2, 4)).toThrow(
        /protocol version 2 is incompatible/,
      );
      expect(Array.from(staleSpectrum.header)).toEqual(staleSpectrumBefore);

      const staleScope = createSonareScopeRingBuffer(2, 4, 3);
      staleScope.header[5] = 2;
      const staleScopeBefore = Array.from(staleScope.header);
      expect(() => scopeRingFromSharedBuffer(staleScope.sharedBuffer, 2, 4, 3)).toThrow(
        /protocol version 2 is incompatible/,
      );
      expect(Array.from(staleScope.header)).toEqual(staleScopeBefore);

      const staleTelemetry = createTelemetryRing(2);
      staleTelemetry.header[1] = 2;
      const staleTelemetryBefore = Array.from(staleTelemetry.header);
      expect(() => engineTelemetryRingFromSharedBuffer(staleTelemetry.sharedBuffer, 2)).toThrow(
        /protocol version 2 is incompatible/,
      );
      expect(Array.from(staleTelemetry.header)).toEqual(staleTelemetryBefore);

      const unpublished = createTelemetryRing(2);
      unpublished.header[0] = 1;
      const unpublishedBefore = Array.from(unpublished.header);
      expect(() => engineTelemetryRingFromSharedBuffer(unpublished.sharedBuffer, 2)).toThrow(
        /records without a compatible producer/,
      );
      expect(Array.from(unpublished.header)).toEqual(unpublishedBefore);
      expect(() =>
        writeSonareEngineTelemetryRingBuffer(unpublished, {
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
          renderFrame: 1,
          timelineSample: 2,
          audibleTimelineSample: 3,
          graphLatencySamplesQ8: 4,
          value: 5,
        }),
      ).toThrow(/records without a compatible producer/);
      expect(Array.from(unpublished.header)).toEqual(unpublishedBefore);
      expect(unpublished.publicationWrapped[0]).toBe(0);

      const oldHeaderBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
      const oldRecordBytes = SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
      const oldSharedBuffer = new SharedArrayBuffer(oldHeaderBytes + 2 * oldRecordBytes);
      const oldHeader = new Int32Array(oldSharedBuffer, 0, SONARE_METER_RING_HEADER_INTS);
      const oldRecords = new Float32Array(
        oldSharedBuffer,
        oldHeaderBytes,
        2 * SONARE_METER_RING_RECORD_FLOATS,
      );
      oldHeader[1] = 2;
      oldHeader[2] = SONARE_METER_RING_RECORD_FLOATS;
      oldHeader[3] = 2;
      expect(() =>
        readSonareMeterRingBuffer({
          sharedBuffer: oldSharedBuffer,
          header: oldHeader,
          records: oldRecords,
          slotStates: new Int32Array(oldSharedBuffer, 0, 2),
          capacity: 2,
        }),
      ).toThrow(/invalid layout/);
    });

    it('rejects telemetry latch mappings with the wrong SAB, offset, length, or value', () => {
      const capacity = 3;
      const ring = createTelemetryRing(capacity);
      const stateOffset =
        SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * SONARE_ENGINE_TELEMETRY_RECORD_BYTES;
      const publicationWrappedOffset = stateOffset + capacity * Int32Array.BYTES_PER_ELEMENT;
      const wrongSab = new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT));
      const wrongOffset = new Int32Array(ring.sharedBuffer, publicationWrappedOffset - 4, 1);
      const wrongLength = new Int32Array(ring.sharedBuffer, publicationWrappedOffset, 0);
      const invalidViews = [wrongSab, wrongOffset, wrongLength];

      for (const publicationWrapped of invalidViews) {
        const headerBefore = Array.from(ring.header);
        const malformedRing = {
          ...ring,
          publicationWrapped,
        } as Parameters<typeof readSonareEngineTelemetryRingBuffer>[0];
        expect(() => readSonareEngineTelemetryRingBuffer(malformedRing)).toThrow(
          /invalid publicationWrapped view/,
        );
        expect(Array.from(ring.header)).toEqual(headerBefore);
        expect(Array.from(ring.publicationWrapped)).toEqual([0]);
      }

      ring.publicationWrapped[0] = 2;
      const headerBeforeInvalidValue = Array.from(ring.header);
      expect(() => readSonareEngineTelemetryRingBuffer(ring)).toThrow(
        /invalid publicationWrapped value/,
      );
      expect(Array.from(ring.header)).toEqual(headerBeforeInvalidValue);
      expect(ring.publicationWrapped[0]).toBe(2);
    });
  });

  describe('free-running cursors across the 2^31 and 2^32 boundaries', () => {
    // Cursors one step short of each boundary, stored as the Int32 header holds them.
    const seeds = [2 ** 31 - 2, 2 ** 32 - 2];
    const capacities = [1, 3, 128, 256];

    function seedCursors(header: Int32Array, seed: number, slots: number[]): void {
      for (const slot of slots) {
        header[slot] = seed;
      }
    }

    for (const capacity of capacities) {
      for (const seed of seeds) {
        it(`keeps the command ring FIFO and bounded (capacity ${capacity}, cursor ${seed})`, () => {
          const ring = createSonareEngineCommandRingBuffer(capacity);
          seedCursors(ring.header, seed, [0, 1]);
          for (let round = 0; round < 3; round++) {
            for (let i = 0; i < capacity; i++) {
              const command = {
                type: SonareEngineCommandType.TransportStop,
                argInt: round * 1000 + i,
              };
              expect(pushSonareEngineCommandRingBuffer(ring, command)).toBe(true);
            }
            expect(
              pushSonareEngineCommandRingBuffer(ring, {
                type: SonareEngineCommandType.TransportStop,
              }),
            ).toBe(false);
            for (let i = 0; i < capacity; i++) {
              expect(popSonareEngineCommandRingBuffer(ring)?.argInt).toBe(round * 1000 + i);
            }
            expect(popSonareEngineCommandRingBuffer(ring)).toBeNull();
          }
        });

        it(`keeps the clip-page request ring FIFO and bounded (capacity ${capacity}, cursor ${seed})`, () => {
          const ring = createSonareClipPageRequestRingBuffer(capacity);
          seedCursors(ring.header, seed, [0, 1]);
          for (let round = 0; round < 3; round++) {
            for (let i = 0; i < capacity; i++) {
              expect(pushSonareClipPageRequestRingBuffer(ring, 0xffff_ffff, round * 1000 + i)).toBe(
                true,
              );
            }
            expect(pushSonareClipPageRequestRingBuffer(ring, 1, 1)).toBe(false);
            const read = readSonareClipPageRequestRingBuffer(ring);
            expect(read.requests.map((request) => request.pageIndex)).toEqual(
              Array.from({ length: capacity }, (_, i) => round * 1000 + i),
            );
            expect(read.requests.every((request) => request.clipId === 0xffff_ffff)).toBe(true);
            expect(readSonareClipPageRequestRingBuffer(ring).requests).toEqual([]);
          }
        });

        it(`keeps the external-MIDI ring FIFO and bounded (capacity ${capacity}, cursor ${seed})`, () => {
          const ring = createSonareExternalMidiRingBuffer(capacity);
          seedCursors(ring.header, seed, [0, 1]);
          for (let round = 0; round < 3; round++) {
            for (let i = 0; i < capacity; i++) {
              expect(
                pushSonareExternalMidiRingBuffer(ring, 1, round * 1000 + i, 0x00003c80, 3),
              ).toBe(true);
            }
            expect(pushSonareExternalMidiRingBuffer(ring, 1, 0, 0x00003c80, 3)).toBe(false);
            expect(
              readSonareExternalMidiRingBuffer(ring).events.map((event) => event.renderFrame),
            ).toEqual(Array.from({ length: capacity }, (_, i) => round * 1000 + i));
            expect(readSonareExternalMidiRingBuffer(ring).events).toEqual([]);
          }
        });

        it(`returns the newest telemetry in order (capacity ${capacity}, cursor ${seed})`, () => {
          const ring = createTelemetryRing(capacity);
          seedCursors(ring.header, seed, [0]);
          ring.header[1] = SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION;
          const written = Math.min(capacity, 3);
          for (let i = 0; i < 3; i++) {
            writeSonareEngineTelemetryRingBuffer(ring, {
              type: SonareEngineTelemetryType.ProcessBlock,
              error: SonareEngineTelemetryError.None,
              renderFrame: i,
              timelineSample: i,
              audibleTimelineSample: i,
              graphLatencySamplesQ8: 0,
              value: i,
            });
          }
          const read = readSonareEngineTelemetryRingBuffer(ring, seed);
          expect(read.telemetry.map((item) => item.renderFrame)).toEqual(
            [0, 1, 2].slice(3 - written),
          );
          expect(readSonareEngineTelemetryRingBuffer(ring, read.nextReadIndex).telemetry).toEqual(
            [],
          );
        });
      }
    }

    it('round-trips external-MIDI render frames past 32 bits', () => {
      const ring = createSonareExternalMidiRingBuffer(4);
      const frames = [2 ** 32 - 1, 2 ** 32, 2 ** 32 + 1, Number.MAX_SAFE_INTEGER];
      for (const frame of frames) {
        expect(pushSonareExternalMidiRingBuffer(ring, 2, frame, 0x00403c90, 3)).toBe(true);
      }
      expect(
        readSonareExternalMidiRingBuffer(ring).events.map((event) => event.renderFrame),
      ).toEqual(frames);
    });

    it('reads the newest meter records across a wrapped write cursor', () => {
      for (const seed of seeds) {
        const ring = createSonareMeterRingBuffer(3);
        ring.header[0] = seed;
        ring.header[3] = SONARE_METER_RING_PROTOCOL_VERSION;
        const sceneJson = mixingScenePresetJson('vocalReverbSend');
        const reference = Mixer.fromSceneJson(sceneJson, 48000, 128);
        const stripCount = reference.stripCount();
        reference.delete();
        const processor = new SonareWorkletProcessor(
          {
            sceneJson,
            sampleRate: 48000,
            blockSize: 128,
            meterIntervalFrames: 128,
            meterSharedBuffer: ring.sharedBuffer,
          },
          {},
        );
        try {
          for (let block = 0; block < 4; block++) {
            const inputs = Array.from({ length: stripCount }, () => [
              new Float32Array(128),
              new Float32Array(128),
            ]);
            expect(
              processor.process(inputs, [[new Float32Array(128), new Float32Array(128)]]),
            ).toBe(true);
          }
          const read = readSonareMeterRingBuffer(ring, seed);
          expect(read.meters.map((meter) => meter.frame)).toEqual([256, 384, 512]);
        } finally {
          processor.destroy();
        }
      }
    });

    it('reads the newest spectrum records across a wrapped write cursor', () => {
      for (const seed of seeds) {
        const ring = createSonareSpectrumRingBuffer(3, 4);
        ring.header[0] = seed;
        ring.header[4] = SONARE_SPECTRUM_RING_PROTOCOL_VERSION;
        const processor = new SonareWorkletProcessor({
          sceneJson: mixingScenePresetJson('vocalReverbSend'),
          sampleRate: 48000,
          blockSize: 128,
          spectrumIntervalFrames: 128,
          spectrumSharedBuffer: ring.sharedBuffer,
        });
        try {
          for (let block = 0; block < 4; block++) {
            const left = new Float32Array(128);
            left[0] = 1;
            expect(
              processor.process([[left, new Float32Array(128)]], [[new Float32Array(128)]]),
            ).toBe(true);
          }
          const read = readSonareSpectrumRingBuffer(ring, seed);
          expect(read.spectra.map((spectrum) => spectrum.frame)).toEqual([256, 384, 512]);
          expect(read.spectra.every((spectrum) => spectrum.bands.length === 4)).toBe(true);
        } finally {
          processor.destroy();
        }
      }
    });
  });

  describe('external-MIDI ring-buffer (SPSC)', () => {
    it('preserves lowered MIDI bytes and drops only new records when full', () => {
      const ring = createSonareExternalMidiRingBuffer(2);
      expect(ring.header).toHaveLength(SONARE_EXTERNAL_MIDI_RING_HEADER_INTS);
      expect(ring.header[3]).toBe(SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S);
      expect(ring.sharedBuffer.byteLength).toBe(sonareExternalMidiRingBufferByteLength(2));
      expect(pushSonareExternalMidiRingBuffer(ring, 7, 127, 0x00643c90, 3)).toBe(true);
      // Transport clock retains the uint32 destination sentinel.
      expect(pushSonareExternalMidiRingBuffer(ring, 0xffffffff, 0, 0x000000f8, 1)).toBe(true);
      expect(pushSonareExternalMidiRingBuffer(ring, 8, 0, 0x00403c90, 3)).toBe(false);

      expect(readSonareExternalMidiRingBuffer(ring)).toEqual({
        events: [
          { destinationId: 7, renderFrame: 127, byteWord: 0x00643c90, byteCount: 3 },
          { destinationId: 0xffffffff, renderFrame: 0, byteWord: 0x000000f8, byteCount: 1 },
        ],
        dropped: 1,
      });
      expect(readSonareExternalMidiRingBuffer(ring)).toEqual({ events: [], dropped: 1 });
    });
  });

  describe('meter ring-buffer round-trip (pure, no audio thread)', () => {
    const sampleRate = 48000;
    const blockSize = 128;

    function makeRingProcessor(ringSharedBuffer: SharedArrayBuffer): {
      processor: SonareWorkletProcessor;
      stripCount: number;
    } {
      const sceneJson = mixingScenePresetJson('vocalReverbSend');
      // A transport must be present for the processor to compute and publish
      // meters; with a meterSharedBuffer the records go to the ring (not postMessage).
      const processor = new SonareWorkletProcessor(
        {
          sceneJson,
          sampleRate,
          blockSize,
          meterIntervalFrames: blockSize,
          meterSharedBuffer: ringSharedBuffer,
        },
        {},
      );
      // One meter record is published per processed block.
      const reference = Mixer.fromSceneJson(sceneJson, sampleRate, blockSize);
      const stripCount = reference.stripCount();
      reference.delete();
      return { processor, stripCount };
    }

    function fabricateInputs(stripCount: number, impulseStrip = 0): Float32Array[][] {
      const inputs: Float32Array[][] = [];
      for (let strip = 0; strip < stripCount; strip++) {
        const left = new Float32Array(blockSize);
        const right = new Float32Array(blockSize);
        if (strip === impulseStrip) {
          left[0] = 1.0;
          right[0] = 1.0;
        }
        inputs.push([left, right]);
      }
      return inputs;
    }

    function driveBlocks(
      processor: SonareWorkletProcessor,
      stripCount: number,
      blocks: number,
    ): void {
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      for (let block = 0; block < blocks; block++) {
        const inputs = fabricateInputs(stripCount, block === 0 ? 0 : -1);
        expect(processor.process(inputs, [[outL, outR]])).toBe(true);
      }
    }

    it('publishes one finite record per block and advances the read cursor', () => {
      const ring = createSonareMeterRingBuffer(8);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        const blocks = 5;
        driveBlocks(processor, stripCount, blocks);

        const read = readSonareMeterRingBuffer(ring, 0);
        expect(read.meters).toHaveLength(blocks);
        expect(read.nextReadIndex).toBe(blocks);

        let previousFrame = -1;
        for (const meter of read.meters) {
          expect(meter.type).toBe('meter');
          expect(meter.targetId).toBe(0);
          expect(Number.isFinite(meter.frame)).toBe(true);
          // frame is the cumulative processed-frame count; strictly increasing.
          expect(meter.frame).toBeGreaterThan(previousFrame);
          previousFrame = meter.frame;
          // dB fields are finite or -Infinity for digital silence (the documented
          // toDb() convention); correlation is always finite in [-1, 1].
          for (const field of ['peakDbL', 'peakDbR', 'rmsDbL', 'rmsDbR'] as const) {
            expect(meter[field] === Number.NEGATIVE_INFINITY || Number.isFinite(meter[field])).toBe(
              true,
            );
          }
          expect(Number.isFinite(meter.correlation)).toBe(true);
          expect(Number.isNaN(meter.momentaryLufs)).toBe(true);
        }
        // First record corresponds to the first published block of frames.
        expect(read.meters[0].frame).toBe(blockSize);
        expect(read.meters[blocks - 1].frame).toBe(blockSize * blocks);
      } finally {
        processor.destroy();
      }
    });

    it('returns only new records on incremental reads', () => {
      const ring = createSonareMeterRingBuffer(8);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        driveBlocks(processor, stripCount, 2);
        const first = readSonareMeterRingBuffer(ring, 0);
        expect(first.meters).toHaveLength(2);
        expect(first.nextReadIndex).toBe(2);

        // Re-reading from the returned cursor yields nothing new.
        const empty = readSonareMeterRingBuffer(ring, first.nextReadIndex);
        expect(empty.meters).toHaveLength(0);
        expect(empty.nextReadIndex).toBe(2);

        // After more blocks, only the new records are returned.
        driveBlocks(processor, stripCount, 3);
        const incremental = readSonareMeterRingBuffer(ring, first.nextReadIndex);
        expect(incremental.meters).toHaveLength(3);
        expect(incremental.nextReadIndex).toBe(5);
        expect(incremental.meters[0].frame).toBe(blockSize * 3);
      } finally {
        processor.destroy();
      }
    });

    it('wraps around and returns at most capacity most-recent records', () => {
      const capacity = 4;
      const ring = createSonareMeterRingBuffer(capacity);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        const blocks = 10; // more than capacity
        driveBlocks(processor, stripCount, blocks);

        const read = readSonareMeterRingBuffer(ring, 0);
        // The reader never returns more than capacity records.
        expect(read.meters.length).toBeLessThanOrEqual(capacity);
        expect(read.meters).toHaveLength(capacity);
        // nextReadIndex tracks the total number of published records.
        expect(read.nextReadIndex).toBe(blocks);

        // Returned records are the most-recent, coherent and finite.
        let previousFrame = -1;
        for (const meter of read.meters) {
          expect(Number.isFinite(meter.frame)).toBe(true);
          expect(meter.frame).toBeGreaterThan(previousFrame);
          previousFrame = meter.frame;
          // -Infinity is the documented dB value for digital silence.
          expect(meter.peakDbL === Number.NEGATIVE_INFINITY || Number.isFinite(meter.peakDbL)).toBe(
            true,
          );
          expect(Number.isFinite(meter.correlation)).toBe(true);
        }
        // Oldest readable record is (writeIndex - capacity).
        expect(read.meters[0].frame).toBe(blockSize * (blocks - capacity + 1));
        expect(read.meters[capacity - 1].frame).toBe(blockSize * blocks);
      } finally {
        processor.destroy();
      }
    });

    it('round-trips a manually populated SharedArrayBuffer using the documented layout', () => {
      const capacity = 4;
      const sab = new SharedArrayBuffer(sonareMeterRingBufferByteLength(capacity));
      const header = new Int32Array(sab, 0, SONARE_METER_RING_HEADER_INTS);
      const records = new Float32Array(
        sab,
        SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
        capacity * SONARE_METER_RING_RECORD_FLOATS,
      );
      const stateOffset =
        SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
        capacity * SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT;
      const slotStates = new Int32Array(sab, stateOffset, capacity);
      // Header layout: [writeIndex, capacity, recordFloats, protocolVersion].
      header[1] = capacity;
      header[2] = SONARE_METER_RING_RECORD_FLOATS;
      header[3] = SONARE_METER_RING_PROTOCOL_VERSION;

      const writeRecord = (
        frame: number,
        targetId: number,
        peakDbL: number,
        peakDbR: number,
        rmsDbL: number,
        rmsDbR: number,
        correlation: number,
        inputPeakDbL: number,
        inputPeakDbR: number,
      ): void => {
        const writeIndex = header[0];
        const offset = (writeIndex % capacity) * SONARE_METER_RING_RECORD_FLOATS;
        records[offset] = frame;
        records[offset + 1] = 0;
        records[offset + 2] = targetId;
        records[offset + 3] = peakDbL;
        records[offset + 4] = peakDbR;
        records[offset + 5] = rmsDbL;
        records[offset + 6] = rmsDbR;
        records[offset + 7] = correlation;
        records[offset + 8] = peakDbL + 0.5;
        records[offset + 9] = peakDbR + 0.5;
        records[offset + 10] = -23;
        records[offset + 11] = -24;
        records[offset + 12] = -25;
        records[offset + 13] = -2;
        records[offset + 14] = inputPeakDbL;
        records[offset + 15] = inputPeakDbR;
        header[0] = writeIndex + 1;
      };

      const ring = { sharedBuffer: sab, header, records, slotStates, capacity };

      writeRecord(128, 0, -1, -2, -3, -4, 0.5, -9, -10);
      writeRecord(256, 1, -5, -6, -7, -8, 0.25, -11, -12);

      const first = readSonareMeterRingBuffer(ring, 0);
      expect(first.meters).toHaveLength(2);
      expect(first.nextReadIndex).toBe(2);
      expect(first.meters[0]).toMatchObject({
        type: 'meter',
        targetId: 0,
        frame: 128,
        peakDbL: -1,
        peakDbR: -2,
        rmsDbL: -3,
        rmsDbR: -4,
        correlation: 0.5,
        truePeakDbL: -0.5,
        truePeakDbR: -1.5,
        momentaryLufs: -23,
        shortTermLufs: -24,
        integratedLufs: -25,
        gainReductionDb: -2,
        inputPeakDbL: -9,
        inputPeakDbR: -10,
      });

      // Overflow the ring; only the most-recent `capacity` survive.
      writeRecord(384, 0, -9, -10, -11, -12, 0, -13, -14);
      writeRecord(512, 1, -13, -14, -15, -16, 0, -15, -16);
      writeRecord(640, 33, -17, -18, -19, -20, 0, -17, -18);
      writeRecord(768, 0xffff, -21, -22, -23, -24, 0, -19, -20);

      const wrapped = readSonareMeterRingBuffer(ring, first.nextReadIndex);
      expect(wrapped.meters.length).toBeLessThanOrEqual(capacity);
      expect(wrapped.nextReadIndex).toBe(6);
      // Frames 128/256 were overwritten; 384..768 remain (most-recent capacity).
      expect(wrapped.meters.map((m) => m.frame)).toEqual([384, 512, 640, 768]);
      expect(wrapped.meters.map((m) => m.targetId)).toEqual([0, 1, 33, 0xffff]);
      for (const meter of wrapped.meters) {
        expect(Number.isFinite(meter.peakDbL)).toBe(true);
        expect(Number.isFinite(meter.correlation)).toBe(true);
      }
    });
  });

  describe('engine SAB command and telemetry rings', () => {
    it('round-trips fixed-layout command records and detects overflow', () => {
      const ring = createSonareEngineCommandRingBuffer(2);
      expect(ring.header[2]).toBe(2);
      expect(ring.header[3]).toBe(SONARE_ENGINE_COMMAND_RECORD_BYTES);
      expect(ring.sharedBuffer.byteLength).toBe(sonareEngineCommandRingBufferByteLength(2));

      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: 128,
        }),
      ).toBe(true);
      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportSeekSample,
          targetId: 7,
          sampleTime: 256n,
          argInt: 48000n,
        }),
      ).toBe(true);
      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportStop,
          sampleTime: 384,
        }),
      ).toBe(false);
      expect(ring.header[4]).toBe(1);

      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({
        type: SonareEngineCommandType.TransportPlay,
        targetId: 0,
        sampleTime: 128,
      });
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({
        type: SonareEngineCommandType.TransportSeekSample,
        targetId: 7,
        sampleTime: 256,
        argInt: 48000,
      });
      expect(popSonareEngineCommandRingBuffer(ring)).toBeNull();
    });

    it('holds type and targetId to the uint32 slot the readers are unsigned on', () => {
      const ring = createSonareEngineCommandRingBuffer(8);
      const push = (command: { type: number; targetId?: number }): boolean =>
        pushSonareEngineCommandRingBuffer(ring, command);

      // The slot is load-bearing: two ids round-trip as two distinct records,
      // and the top of the domain survives rather than folding to 0.
      expect(push({ type: SonareEngineCommandType.TransportSeekSample, targetId: 7 })).toBe(true);
      expect(push({ type: SonareEngineCommandType.TransportSeekSample, targetId: 9 })).toBe(true);
      expect(
        push({ type: SonareEngineCommandType.TransportSeekSample, targetId: 0xffff_ffff }),
      ).toBe(true);
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({ targetId: 7 });
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({ targetId: 9 });
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({ targetId: 0xffff_ffff });

      // A fraction truncates, a negative wraps to a legal id and 2**32 wraps to
      // 0; none of the three is distinguishable downstream from a deliberate id.
      for (const targetId of [7.5, -1, 2 ** 32, Number.NaN, Number.POSITIVE_INFINITY]) {
        expect(() => push({ type: SonareEngineCommandType.TransportSeekSample, targetId })).toThrow(
          /targetId must be an integer within \[0, 4294967295\]/,
        );
      }
      for (const type of [2.5, -1, 2 ** 32, Number.NaN]) {
        expect(() => push({ type })).toThrow(/type must be an integer within \[0, 4294967295\]/);
      }
      // A refused push leaves the write index alone, so the next one reuses the slot.
      expect(push({ type: SonareEngineCommandType.TransportStop, targetId: 11 })).toBe(true);
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({ targetId: 11 });
    });

    it('round-trips fixed-layout telemetry records with wraparound', () => {
      const ring = createTelemetryRing(2);
      expect(ring.header[1]).toBe(0);
      expect(ring.header[2]).toBe(2);
      expect(ring.header[3]).toBe(SONARE_ENGINE_TELEMETRY_RECORD_BYTES);
      expect(ring.sharedBuffer.byteLength).toBe(sonareEngineTelemetryRingBufferByteLength(2));
      expect(ring.publicationWrapped[0]).toBe(0);

      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 0,
        timelineSample: 128,
        audibleTimelineSample: 128,
        graphLatencySamplesQ8: 0,
        value: 128,
      });
      expect(ring.header[1]).toBe(SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION);
      expect(ring.header[4]).toBe(0);
      expect(ring.publicationWrapped[0]).toBe(0);
      const first = readSonareEngineTelemetryRingBuffer(ring);
      expect(first.nextReadIndex).toBe(1);
      expect(first.telemetry).toHaveLength(1);
      expect(first.telemetry[0]).toMatchObject({
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        timelineSample: 128,
      });

      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.Error,
        error: SonareEngineTelemetryError.MaxBlockExceeded,
        renderFrame: 128,
        timelineSample: 256,
        audibleTimelineSample: 256,
        graphLatencySamplesQ8: 0,
        value: 512,
      });
      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 256,
        timelineSample: 384,
        audibleTimelineSample: 384,
        graphLatencySamplesQ8: 0,
        value: 128,
      });

      const wrapped = readSonareEngineTelemetryRingBuffer(ring, first.nextReadIndex);
      expect(wrapped.nextReadIndex).toBe(3);
      expect(wrapped.telemetry).toHaveLength(2);
      expect(wrapped.telemetry.map((item) => item.timelineSample)).toEqual([256, 384]);
      expect(ring.header[4]).toBe(1);
      expect(ring.publicationWrapped[0]).toBe(1);
    });

    it('round-trips the worklet-only InvalidCommand and MaxChannelsExceeded errors', () => {
      expect(SonareEngineTelemetryError.InvalidCommand).toBe(19);
      expect(SonareEngineTelemetryError.MaxChannelsExceeded).toBe(20);

      const ring = createTelemetryRing(2);
      const baseRecord = {
        type: SonareEngineTelemetryType.Error,
        renderFrame: 0,
        timelineSample: 0,
        audibleTimelineSample: 0,
        graphLatencySamplesQ8: 0,
        value: 0,
      } as const;
      writeSonareEngineTelemetryRingBuffer(ring, {
        ...baseRecord,
        error: SonareEngineTelemetryError.InvalidCommand,
      });
      writeSonareEngineTelemetryRingBuffer(ring, {
        ...baseRecord,
        error: SonareEngineTelemetryError.MaxChannelsExceeded,
      });

      expect(readSonareEngineTelemetryRingBuffer(ring).telemetry.map((item) => item.error)).toEqual(
        [19, 20],
      );
    });

    it('documents the shared ring header layout', () => {
      const capacity = 4;
      const sab = new SharedArrayBuffer(sonareEngineCommandRingBufferByteLength(capacity));
      const header = new Int32Array(sab, 0, SONARE_ENGINE_RING_HEADER_INTS);
      header[0] = 3; // writeIndex
      header[1] = 1; // readIndex
      header[2] = capacity;
      header[3] = SONARE_ENGINE_COMMAND_RECORD_BYTES;
      header[4] = 2; // overflow/drop count

      expect(Array.from(header)).toEqual([3, 1, capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES, 2]);
    });
  });

  describe('counted overwrite reader protocol', () => {
    function telemetryRecord(generation: number) {
      return {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: generation,
        timelineSample: generation + 100,
        audibleTimelineSample: generation + 200,
        graphLatencySamplesQ8: generation + 300,
        value: generation + 400,
      };
    }

    function viewWithFirstReadHook(view: DataView, hook: () => void): DataView {
      let first = true;
      return new Proxy(view, {
        get(target, property) {
          const member = Reflect.get(target, property, target);
          if (property === 'getUint32' && first) {
            first = false;
            hook();
          }
          return typeof member === 'function' ? member.bind(target) : member;
        },
      });
    }

    it('claims one slot once and commits its cursor before clearing the writer bit', () => {
      const ring = createTelemetryRing(2);
      const cursor = tryBeginOverwriteWrite(ring.header, ring.slotStates, ring.capacity);
      expect(cursor).toBe(0);
      expect(Atomics.load(ring.slotStates, 0) >>> 0).toBe(0x8000_0000);
      commitOverwriteWrite(ring.header, ring.slotStates, ring.capacity, cursor as number);
      expect(ring.header[0]).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);

      const nextCursor = tryBeginOverwriteWrite(ring.header, ring.slotStates, ring.capacity);
      expect(nextCursor).toBe(1);
      commitSonareEngineTelemetryRingWrite(
        ring.header,
        ring.slotStates,
        ring.publicationWrapped,
        ring.capacity,
        nextCursor as number,
      );
      expect(ring.header[0]).toBe(2);
      expect(ring.header[4]).toBe(0);
      expect(ring.publicationWrapped[0]).toBe(0);
      expect(Atomics.load(ring.slotStates, 1)).toBe(0);
    });

    it('resumes at a record whose writer has stored the cursor but not yet cleared its bit', () => {
      const ring = createTelemetryRing(2);
      const recordA = telemetryRecord(1);
      const recordB = telemetryRecord(2);
      writeSonareEngineTelemetryRingBuffer(ring, recordA);
      writeSonareEngineTelemetryRingBuffer(ring, recordB);
      // commitOverwriteWrite stores the cursor first, so a reader can see record 1 as
      // published while its writer bit is still set.
      Atomics.or(ring.slotStates, 1, 0x8000_0000);

      const held = readSonareEngineTelemetryRingBuffer(ring, 0);
      expect(held).toEqual({ nextReadIndex: 1, telemetry: [recordA] });

      Atomics.and(ring.slotStates, 1, 0x7fff_ffff);
      expect(readSonareEngineTelemetryRingBuffer(ring, held.nextReadIndex)).toEqual({
        nextReadIndex: 2,
        telemetry: [recordB],
      });
    });

    it('lets two independent readers hold A together and both return the same cursor', () => {
      const ring = createTelemetryRing(1);
      const recordA = telemetryRecord(1);
      writeSonareEngineTelemetryRingBuffer(ring, recordA);

      let peakState = 0;
      let second: ReturnType<typeof readSonareEngineTelemetryRingBuffer> | undefined;
      const ringR2 = {
        ...ring,
        view: viewWithFirstReadHook(ring.view, () => {
          peakState = Atomics.load(ring.slotStates, 0);
        }),
      };
      const ringR1 = {
        ...ring,
        view: viewWithFirstReadHook(ring.view, () => {
          second = readSonareEngineTelemetryRingBuffer(ringR2, 0);
        }),
      };

      const first = readSonareEngineTelemetryRingBuffer(ringR1, 0);
      expect(peakState).toBe(2);
      expect(first).toEqual({ nextReadIndex: 1, telemetry: [recordA] });
      expect(second).toEqual({ nextReadIndex: 1, telemetry: [recordA] });
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
    });

    it('drops 65 claimed-slot writes without touching A, then resumes at C', () => {
      const ring = createTelemetryRing(1);
      const recordA = telemetryRecord(1);
      writeSonareEngineTelemetryRingBuffer(ring, recordA);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48),
      );

      expect(tryAcquireOverwriteRead(ring.slotStates, ring.capacity, 0)).toBe(true);
      expect(Atomics.load(ring.slotStates, 0)).toBe(1);
      for (let generation = 2; generation < 67; generation++) {
        writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(generation));
      }
      expect(ring.header[0]).toBe(1);
      expect(ring.header[4]).toBe(65);
      expect(ring.publicationWrapped[0]).toBe(0);
      expect(Array.from(ring.slotStates)).toEqual([1]);
      expect(
        Array.from(new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48)),
      ).toEqual(payloadA);

      releaseOverwriteRead(ring.slotStates, ring.capacity, 0);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      expect(readSonareEngineTelemetryRingBuffer(ring, 0).telemetry).toEqual([recordA]);

      const recordC = telemetryRecord(67);
      writeSonareEngineTelemetryRingBuffer(ring, recordC);
      expect(ring.header[0]).toBe(2);
      expect(ring.header[4]).toBe(66);
      expect(ring.publicationWrapped[0]).toBe(1);
      expect(readSonareEngineTelemetryRingBuffer(ring, 1).telemetry).toEqual([recordC]);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
    });

    it('separates capacity-two collision drops from an empty-slot resume and overwrite', () => {
      const ring = createTelemetryRing(2);
      const recordA = telemetryRecord(1);
      writeSonareEngineTelemetryRingBuffer(ring, recordA);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48),
      );

      // Cursor 1 targets the still-empty second slot. Holding it makes all
      // 65 attempted publications collide without making a physical wrap.
      expect(tryAcquireOverwriteRead(ring.slotStates, ring.capacity, 1)).toBe(true);
      for (let generation = 2; generation < 67; generation++) {
        writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(generation));
      }
      expect(ring.header[0]).toBe(1);
      expect(ring.header[4]).toBe(65);
      expect(ring.publicationWrapped[0]).toBe(0);
      expect(
        Array.from(new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48)),
      ).toEqual(payloadA);

      releaseOverwriteRead(ring.slotStates, ring.capacity, 1);
      const recordB = telemetryRecord(67);
      writeSonareEngineTelemetryRingBuffer(ring, recordB);
      expect(ring.header[0]).toBe(2);
      expect(ring.header[4]).toBe(65);
      expect(ring.publicationWrapped[0]).toBe(0);
      expect(readSonareEngineTelemetryRingBuffer(ring, 1).telemetry).toEqual([recordB]);

      const recordC = telemetryRecord(68);
      writeSonareEngineTelemetryRingBuffer(ring, recordC);
      expect(ring.header[0]).toBe(3);
      expect(ring.header[4]).toBe(66);
      expect(ring.publicationWrapped[0]).toBe(1);
      expect(readSonareEngineTelemetryRingBuffer(ring, 2).telemetry).toEqual([recordC]);
    });

    it('keeps the physical-wrap latch across reattachment and the uint32 cursor span', () => {
      const ring = createTelemetryRing(2);
      writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(1));
      writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(2));
      writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(3));
      expect(ring.header[0]).toBe(3);
      expect(ring.header[4]).toBe(1);
      expect(ring.publicationWrapped[0]).toBe(1);

      // The cursor is stored as signed Int32 but is loaded as uint32. Remap
      // the same SAB after seeding the full cursor-span boundary.
      ring.header[0] = -1;
      const reattached = engineTelemetryRingFromSharedBuffer(ring.sharedBuffer, 2);
      expect(reattached.sharedBuffer).toBe(ring.sharedBuffer);
      expect(reattached.publicationWrapped.buffer).toBe(ring.sharedBuffer);
      expect(reattached.publicationWrapped.byteOffset).toBe(
        SONARE_ENGINE_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
          2 * SONARE_ENGINE_TELEMETRY_RECORD_BYTES +
          2 * Int32Array.BYTES_PER_ELEMENT,
      );
      expect(reattached.publicationWrapped).toHaveLength(1);
      expect(reattached.publicationWrapped[0]).toBe(1);

      writeSonareEngineTelemetryRingBuffer(reattached, telemetryRecord(4));
      expect(reattached.header[0]).toBe(0);
      expect(reattached.header[4]).toBe(2);
      expect(reattached.publicationWrapped[0]).toBe(1);

      writeSonareEngineTelemetryRingBuffer(reattached, telemetryRecord(5));
      expect(reattached.header[0]).toBe(1);
      expect(reattached.header[4]).toBe(3);
      expect(reattached.publicationWrapped[0]).toBe(1);
      expect(readSonareEngineTelemetryRingBuffer(reattached, 0xffffffff).telemetry).toEqual([
        telemetryRecord(4),
        telemetryRecord(5),
      ]);

      const remappedAgain = engineTelemetryRingFromSharedBuffer(ring.sharedBuffer, 2);
      expect(remappedAgain.publicationWrapped.buffer).toBe(ring.sharedBuffer);
      expect(remappedAgain.publicationWrapped.byteOffset).toBe(
        reattached.publicationWrapped.byteOffset,
      );
      expect(remappedAgain.publicationWrapped[0]).toBe(1);
    });

    it('captures every getter-backed mapping once before the committed publication', () => {
      const ringA = createTelemetryRing(2);
      const ringB = createTelemetryRing(2);
      const reads = {
        sharedBuffer: 0,
        header: 0,
        view: 0,
        slotStates: 0,
        publicationWrapped: 0,
        capacity: 0,
      };
      const getterRing = {
        get sharedBuffer() {
          reads.sharedBuffer += 1;
          return reads.sharedBuffer === 1 ? ringA.sharedBuffer : ringB.sharedBuffer;
        },
        get header() {
          reads.header += 1;
          return reads.header === 1 ? ringA.header : ringB.header;
        },
        get view() {
          reads.view += 1;
          return reads.view === 1 ? ringA.view : ringB.view;
        },
        get slotStates() {
          reads.slotStates += 1;
          return reads.slotStates === 1 ? ringA.slotStates : ringB.slotStates;
        },
        get publicationWrapped() {
          reads.publicationWrapped += 1;
          return reads.publicationWrapped === 1
            ? ringA.publicationWrapped
            : ringB.publicationWrapped;
        },
        get capacity() {
          reads.capacity += 1;
          return reads.capacity === 1 ? ringA.capacity : ringB.capacity;
        },
      } as unknown as Parameters<typeof writeSonareEngineTelemetryRingBuffer>[0];
      const recordB = telemetryRecord(7);

      writeSonareEngineTelemetryRingBuffer(getterRing, recordB);

      expect(reads).toEqual({
        sharedBuffer: 1,
        header: 1,
        view: 1,
        slotStates: 1,
        publicationWrapped: 1,
        capacity: 1,
      });
      expect(ringA.header[0]).toBe(1);
      expect(ringA.header[1]).toBe(SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION);
      expect(ringA.publicationWrapped[0]).toBe(0);
      expect(readSonareEngineTelemetryRingBuffer(ringA).telemetry).toEqual([recordB]);
      expect(Array.from(ringB.header)).toEqual([0, 0, 2, SONARE_ENGINE_TELEMETRY_RECORD_BYTES, 0]);
      expect(Array.from(ringB.slotStates)).toEqual([0, 0]);
      expect(ringB.publicationWrapped[0]).toBe(0);
    });

    it('restores the saturated reader count before throwing on overrun', () => {
      const ring = createTelemetryRing(1);
      Atomics.store(ring.slotStates, 0, 0x7fff_ffff);
      expect(() => tryAcquireOverwriteRead(ring.slotStates, ring.capacity, 0)).toThrow(
        /reader count overflow/,
      );
      expect(Atomics.load(ring.slotStates, 0)).toBe(0x7fff_ffff);
      Atomics.store(ring.slotStates, 0, 0);
    });

    it('finishes late producer getters before claiming, preserving A and allowing C', () => {
      const ring = createTelemetryRing(1);
      const recordA = telemetryRecord(1);
      writeSonareEngineTelemetryRingBuffer(ring, recordA);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48),
      );
      let sawUnclaimedA = false;
      const lateThrow = {
        get type() {
          return SonareEngineTelemetryType.ProcessBlock;
        },
        get error() {
          return SonareEngineTelemetryError.None;
        },
        get renderFrame() {
          return 2;
        },
        get timelineSample() {
          return 102;
        },
        get audibleTimelineSample() {
          return 202;
        },
        get graphLatencySamplesQ8() {
          return 302;
        },
        get value() {
          sawUnclaimedA =
            Atomics.load(ring.slotStates, 0) === 0 &&
            ring.header[0] === 1 &&
            Array.from(
              new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48),
            ).every((byte, index) => byte === payloadA[index]);
          throw new Error('late telemetry getter');
        },
      } as unknown as Parameters<typeof writeSonareEngineTelemetryRingBuffer>[1];

      expect(() => writeSonareEngineTelemetryRingBuffer(ring, lateThrow)).toThrow(
        'late telemetry getter',
      );
      expect(sawUnclaimedA).toBe(true);
      expect(ring.header[0]).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      expect(
        Array.from(new Uint8Array(ring.sharedBuffer, SONARE_ENGINE_RING_HEADER_INTS * 4, 48)),
      ).toEqual(payloadA);
      expect(readSonareEngineTelemetryRingBuffer(ring, 0).telemetry).toEqual([recordA]);

      const recordC = telemetryRecord(3);
      writeSonareEngineTelemetryRingBuffer(ring, recordC);
      expect(readSonareEngineTelemetryRingBuffer(ring, 1).telemetry).toEqual([recordC]);
    });

    it('releases a reader exactly once when materialization throws', () => {
      const ring = createTelemetryRing(1);
      writeSonareEngineTelemetryRingBuffer(ring, telemetryRecord(1));
      const throwingRing = {
        ...ring,
        view: viewWithFirstReadHook(ring.view, () => {
          throw new Error('telemetry materializer');
        }),
      };
      expect(() => readSonareEngineTelemetryRingBuffer(throwingRing, 0)).toThrow(
        'telemetry materializer',
      );
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);

      const recordC = telemetryRecord(3);
      writeSonareEngineTelemetryRingBuffer(ring, recordC);
      expect(readSonareEngineTelemetryRingBuffer(ring, 1).telemetry).toEqual([recordC]);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
    });
  });
});
