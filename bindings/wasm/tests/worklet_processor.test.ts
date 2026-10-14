import {
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_SPECTRUM_RING_PROTOCOL_VERSION,
} from '../dist/worklet.js';
import {
  countFloat32Subarrays,
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  describe,
  expect,
  it,
  Mixer,
  mixingScenePresetJson,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareWorkletProcessor,
  SonareWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

describe('SonareWorkletProcessor', () => {
  setupWorklet();

  it('matches the offline mixer for one 128-sample render quantum', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const sceneJson = mixingScenePresetJson('vocalReverbSend');

    const offline = Mixer.fromSceneJson(sceneJson, sampleRate, blockSize);
    const processor = new SonareWorkletProcessor({ sceneJson, sampleRate, blockSize });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const alive = processor.process(
        [
          [vocalL, vocalR],
          [returnL, returnR],
        ],
        [[outL, outR]],
      );

      expect(alive).toBe(true);
      expect(Array.from(outL)).toEqual(Array.from(expected.left));
      expect(Array.from(outR)).toEqual(Array.from(expected.right));
    } finally {
      processor.destroy();
      offline.delete();
    }
  });

  it('keeps processing silence when an input strip is missing', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(processor.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(outL.length).toBe(blockSize);
      expect(outR.length).toBe(blockSize);
    } finally {
      processor.destroy();
    }
  });

  it('reads the realtime views once per callback whatever the strip count', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const checksPerCallback = (preset: string, strips: number): number => {
      const processor = new SonareWorkletProcessor({
        sceneJson: mixingScenePresetJson(preset),
        sampleRate,
        blockSize,
      });
      try {
        const mixer = (processor as unknown as { mixer: Mixer }).mixer;
        expect(mixer.stripCount()).toBe(strips);
        const realStripCount = mixer.stripCount;
        let checks = 0;
        mixer.stripCount = function (this: Mixer) {
          checks++;
          return realStripCount.call(this);
        };
        const inputs = Array.from({ length: strips }, () => [
          new Float32Array(blockSize),
          new Float32Array(blockSize),
        ]);
        processor.process(inputs, [[new Float32Array(blockSize), new Float32Array(blockSize)]]);
        return checks;
      } finally {
        processor.destroy();
      }
    };
    const twoStrips = checksPerCallback('vocalReverbSend', 2);
    const threeStrips = checksPerCallback('commentaryDucking', 3);
    expect(threeStrips).toBe(twoStrips);
    // Input planes, process and the two outputs: one topology check each.
    expect(twoStrips).toBeLessThanOrEqual(5);
  });

  it('creates no subarray view for a full render quantum', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const quantum = (): void => {
        processor.process(
          [
            [vocalL, vocalR],
            [new Float32Array(blockSize), new Float32Array(blockSize)],
          ],
          [[outL, outR]],
        );
      };
      quantum();
      expect(countFloat32Subarrays(quantum)).toBe(0);
      expect(outL.some((sample) => sample !== 0)).toBe(true);
    } finally {
      processor.destroy();
    }
  });

  it('accepts postMessage-style automation and publishes meters', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const meters: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
      },
      { onMeter: (meter) => meters.push(meter) },
    );
    try {
      processor.receiveMessage({
        type: 'scheduleInsertAutomation',
        stripIndex: 0,
        insertIndex: 0,
        paramId: 0,
        samplePos: 0,
        value: 0,
        curve: 'linear',
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);

      expect(
        processor.process(
          [
            [vocalL, vocalR],
            [returnL, returnR],
          ],
          [[outL, outR]],
        ),
      ).toBe(true);
      expect(meters).toHaveLength(1);
      expect(meters[0]).toMatchObject({ type: 'meter', frame: blockSize });
    } finally {
      processor.destroy();
    }
  });

  // The SAB meter ring exists so the audio render callback allocates and posts
  // nothing. Wiring true-peak metering in brought back a fresh fourteen-field
  // object per interval, built before the ring branch was even chosen, so the
  // zero-allocation claim the engine processor's comment makes about this class
  // stopped being true. Count the objects rather than trusting the shape of the
  // code: `meterSnapshot()` is the one allocation left, and it belongs to the
  // embind accessor rather than to this file.
  it('allocates no meter record per publish when a ring is configured', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const ring = createSonareMeterRingBuffer(64);
    const posted: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
        meterSharedBuffer: ring.sharedBuffer,
      },
      { postMessage: (meter) => posted.push(meter) },
    );
    try {
      const silence = () => new Float32Array(blockSize);
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      // Observe what the ring writer is handed, not what the field holds: a
      // per-publish copy would leave the field identical while still allocating.
      const seen = new Set<object>();
      const internals = processor as unknown as {
        writeMeterRing: (meter: object) => void;
      };
      const realWrite = internals.writeMeterRing;
      internals.writeMeterRing = function (meter: object) {
        seen.add(meter);
        return realWrite.call(this, meter);
      };
      const publishes = 8;
      for (let block = 0; block < publishes; block++) {
        processor.process(
          [
            [silence(), silence()],
            [silence(), silence()],
          ],
          [[outL, outR]],
        );
      }
      internals.writeMeterRing = realWrite;
      expect(seen.size).toBe(1);
      expect(posted).toHaveLength(0);
      const read = readSonareMeterRingBuffer(ring);
      expect(read.meters).toHaveLength(publishes);
      // Reuse must not smear one publish over another: each record reached the
      // ring with its own frame before the next overwrote the fields.
      expect(read.meters.map((meter) => meter.frame)).toEqual(
        Array.from({ length: publishes }, (_, i) => (i + 1) * blockSize),
      );
    } finally {
      processor.destroy();
    }
  });

  it('can publish meters through a SharedArrayBuffer ring', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const ring = createSonareMeterRingBuffer(4);
    const posted: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
        meterSharedBuffer: ring.sharedBuffer,
      },
      { postMessage: (meter) => posted.push(meter) },
    );
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);

      expect(
        processor.process(
          [
            [vocalL, vocalR],
            [returnL, returnR],
          ],
          [[outL, outR]],
        ),
      ).toBe(true);

      expect(posted).toHaveLength(0);
      const read = readSonareMeterRingBuffer(ring);
      expect(read.nextReadIndex).toBe(1);
      expect(read.meters).toHaveLength(1);
      expect(read.meters[0]).toMatchObject({ type: 'meter', frame: blockSize });
    } finally {
      processor.destroy();
    }
  });

  it('holds mixer meter records stable while a reader is active and resumes after release', () => {
    const ring = createSonareMeterRingBuffer(1);
    // A newly-created viewer ring is explicitly unacknowledged until a producer
    // attaches it. This guards the version cut without depending on a record.
    expect(ring.header[3]).toBe(0);
    expect(readSonareMeterRingBuffer(ring)).toEqual({ nextReadIndex: 0, meters: [] });

    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate: 48000,
      blockSize: 128,
      meterSharedBuffer: ring.sharedBuffer,
    });
    try {
      expect(ring.header[3]).toBe(SONARE_METER_RING_PROTOCOL_VERSION);
      const internals = processor as unknown as {
        writeMeterRing: (meter: unknown) => void;
      };
      const write = (meter: unknown): void => internals.writeMeterRing.call(processor, meter);
      const meter = (frame: number) => ({
        type: 'meter' as const,
        targetId: 0,
        frame,
        peakDbL: frame + 0.1,
        peakDbR: frame + 0.2,
        rmsDbL: frame + 0.3,
        rmsDbR: frame + 0.4,
        correlation: frame + 0.5,
        truePeakDbL: frame + 0.6,
        truePeakDbR: frame + 0.7,
        momentaryLufs: frame + 0.8,
        shortTermLufs: frame + 0.9,
        integratedLufs: frame + 1,
        gainReductionDb: frame + 1.1,
        inputPeakDbL: frame + 1.2,
        inputPeakDbR: frame + 1.3,
      });

      // Establish a real publication first. Without this precondition the
      // held-reader loop could pass while never reaching the writer branch.
      write(meter(1));
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
      );

      expect(Atomics.add(ring.slotStates, 0, 1)).toBe(0);
      for (let attempt = 0; attempt < 65; attempt++) {
        write(meter(100 + attempt));
      }
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(
        Array.from(
          new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
        ),
      ).toEqual(payloadA);
      expect(Atomics.load(ring.slotStates, 0)).toBe(1);

      expect(Atomics.sub(ring.slotStates, 0, 1)).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      write(meter(2));
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(2);
      expect(readSonareMeterRingBuffer(ring, 1).meters.at(-1)?.frame).toBe(2);
    } finally {
      processor.destroy();
    }
  });

  it('materializes mixer meter properties before claiming and preserves the old record on throw', () => {
    const ring = createSonareMeterRingBuffer(1);
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate: 48000,
      blockSize: 128,
      meterSharedBuffer: ring.sharedBuffer,
    });
    try {
      const internals = processor as unknown as {
        writeMeterRing: (meter: unknown) => void;
      };
      const write = (meter: unknown): void => internals.writeMeterRing.call(processor, meter);
      const baseline = {
        type: 'meter' as const,
        targetId: 0,
        frame: 7,
        peakDbL: 1,
        peakDbR: 2,
        rmsDbL: 3,
        rmsDbR: 4,
        correlation: 5,
        truePeakDbL: 6,
        truePeakDbR: 7,
        momentaryLufs: 8,
        shortTermLufs: 9,
        integratedLufs: 10,
        gainReductionDb: 11,
        inputPeakDbL: 12,
        inputPeakDbR: 13,
      };
      write(baseline);
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
      );
      const accesses: number[] = [];
      const source = {
        frame: 8,
        targetId: 0,
        peakDbL: '21',
        peakDbR: '22',
        rmsDbL: '23',
        rmsDbR: '24',
        correlation: '25',
        truePeakDbL: '26',
        truePeakDbR: '27',
        momentaryLufs: '28',
        shortTermLufs: '29',
        integratedLufs: '30',
        gainReductionDb: '31',
        inputPeakDbL: '32',
        get inputPeakDbR(): never {
          accesses.push(Atomics.load(ring.slotStates, 0));
          throw new Error('late mixer meter getter');
        },
      };
      for (const key of [
        'frame',
        'targetId',
        'peakDbL',
        'peakDbR',
        'rmsDbL',
        'rmsDbR',
        'correlation',
        'truePeakDbL',
        'truePeakDbR',
        'momentaryLufs',
        'shortTermLufs',
        'integratedLufs',
        'gainReductionDb',
        'inputPeakDbL',
      ]) {
        const value = source[key as keyof typeof source];
        Object.defineProperty(source, key, {
          configurable: true,
          get: () => {
            accesses.push(Atomics.load(ring.slotStates, 0));
            return value;
          },
        });
      }
      expect(() => write(source)).toThrow('late mixer meter getter');
      // Every source access happened before the claim; the writer bit was never
      // visible to the throwing getter and the completed A record remains intact.
      expect(accesses).toEqual(new Array(15).fill(0));
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      expect(
        Array.from(
          new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
        ),
      ).toEqual(payloadA);
    } finally {
      processor.destroy();
    }
  });

  it('refuses a publication interval instead of rounding it into range', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const counts = (
      options: { meterIntervalFrames?: number; spectrumIntervalFrames?: number },
      blocks = 8,
    ): { meters: number; spectra: number } => {
      const meters: unknown[] = [];
      const spectra: unknown[] = [];
      const processor = new SonareWorkletProcessor(
        {
          sceneJson: mixingScenePresetJson('vocalReverbSend'),
          sampleRate,
          blockSize,
          spectrumBands: 8,
          ...options,
        },
        {
          onMeter: (meter) => meters.push(meter),
          onSpectrum: (spectrum) => spectra.push(spectrum),
        },
      );
      try {
        for (let block = 0; block < blocks; block++) {
          const inL = new Float32Array(blockSize).fill(0.25);
          const inR = new Float32Array(blockSize).fill(0.25);
          expect(
            processor.process(
              [[inL, inR]],
              [[new Float32Array(blockSize), new Float32Array(blockSize)]],
            ),
          ).toBe(true);
        }
      } finally {
        processor.destroy();
      }
      return { meters: meters.length, spectra: spectra.length };
    };

    // Both fields are load-bearing: a longer interval publishes strictly fewer
    // snapshots over the same block count, and 0 turns the publication off.
    const perBlock = counts({ meterIntervalFrames: blockSize, spectrumIntervalFrames: blockSize });
    expect(perBlock.meters).toBeGreaterThan(0);
    expect(perBlock.spectra).toBeGreaterThan(0);
    const sparse = counts({
      meterIntervalFrames: blockSize * 4,
      spectrumIntervalFrames: blockSize * 4,
    });
    expect(sparse.meters).toBeLessThan(perBlock.meters);
    expect(sparse.spectra).toBeLessThan(perBlock.spectra);
    const off = counts({ meterIntervalFrames: 0, spectrumIntervalFrames: 0 });
    expect(off).toEqual({ meters: 0, spectra: 0 });

    for (const frames of [2048.5, -1, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => counts({ meterIntervalFrames: frames })).toThrow(
        /meterIntervalFrames must be an integer of at least 0/,
      );
      expect(() => counts({ spectrumIntervalFrames: frames })).toThrow(
        /spectrumIntervalFrames must be an integer of at least 0/,
      );
    }
  });

  it('can publish spectrum snapshots through postMessage and SharedArrayBuffer', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const spectra: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        spectrumIntervalFrames: blockSize,
        spectrumBands: 8,
      },
      { onSpectrum: (spectrum) => spectra.push(spectrum) },
    );
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(processor.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(spectra).toHaveLength(1);
      expect(spectra[0]).toMatchObject({ type: 'spectrum', frame: blockSize });
      expect((spectra[0] as { bands: Float32Array }).bands).toHaveLength(8);
    } finally {
      processor.destroy();
    }

    const ring = createSonareSpectrumRingBuffer(4, 8);
    const ringProcessor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
      spectrumIntervalFrames: blockSize,
      spectrumSharedBuffer: ring.sharedBuffer,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      expect(ringProcessor.process([[vocalL, vocalR]], [[new Float32Array(blockSize)]])).toBe(true);
      const read = readSonareSpectrumRingBuffer(ring);
      expect(read.nextReadIndex).toBe(1);
      expect(read.spectra).toHaveLength(1);
      expect(read.spectra[0].bands).toHaveLength(8);
    } finally {
      ringProcessor.destroy();
    }
  });

  it('holds mixer spectrum records stable across 65 failed claims and resumes after release', () => {
    const ring = createSonareSpectrumRingBuffer(1, 4);
    expect(ring.header[4]).toBe(0);
    expect(readSonareSpectrumRingBuffer(ring)).toEqual({ nextReadIndex: 0, spectra: [] });
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate: 48000,
      blockSize: 128,
      spectrumSharedBuffer: ring.sharedBuffer,
      spectrumBands: ring.bands,
    });
    try {
      expect(ring.header[4]).toBe(SONARE_SPECTRUM_RING_PROTOCOL_VERSION);
      const internals = processor as unknown as {
        writeSpectrumRing: (frame: number, bands: Float32Array) => void;
      };
      const write = (frame: number, bands: Float32Array): void =>
        internals.writeSpectrumRing.call(processor, frame, bands);
      const bandsA = Float32Array.from([1, 2, 3, 4]);
      write(1, bandsA);
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
      );

      expect(Atomics.add(ring.slotStates, 0, 1)).toBe(0);
      for (let attempt = 0; attempt < 65; attempt++) {
        write(100 + attempt, Float32Array.from([attempt, attempt + 1, attempt + 2, attempt + 3]));
      }
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(
        Array.from(
          new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
        ),
      ).toEqual(payloadA);
      expect(Atomics.load(ring.slotStates, 0)).toBe(1);

      expect(Atomics.sub(ring.slotStates, 0, 1)).toBe(1);
      write(2, Float32Array.from([5, 6, 7, 8]));
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(2);
      expect(readSonareSpectrumRingBuffer(ring, 1).spectra.at(-1)?.frame).toBe(2);
    } finally {
      processor.destroy();
    }
  });

  it('clamps requested spectrum bands to the per-block Nyquist bin', () => {
    // With a block of n samples the single-bin DFT can only resolve bins up to
    // floor(n / 2); higher bands would alias, so they must be pinned to the
    // silence floor (magnitudeToDb(0) === -120) regardless of input content.
    const sampleRate = 48000;
    const blockSize = 8;
    const spectra: { bands: Float32Array }[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        spectrumIntervalFrames: blockSize,
        spectrumBands: 8,
      },
      { onSpectrum: (spectrum) => spectra.push(spectrum as { bands: Float32Array }) },
    );
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      expect(
        processor.process(
          [[vocalL, vocalR]],
          [[new Float32Array(blockSize), new Float32Array(blockSize)]],
        ),
      ).toBe(true);
      expect(spectra).toHaveLength(1);
      const bands = spectra[0].bands;
      expect(bands).toHaveLength(8);
      // maxBand = floor(8 / 2) = 4: bands 4..7 are above Nyquist and pinned.
      for (let band = 4; band < 8; band++) {
        expect(bands[band]).toBe(-120);
      }
    } finally {
      processor.destroy();
    }
  });

  it('routes messages and meters through a registered processor port', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {
          port = {
            posted: [] as unknown[],
            postMessage: (message: unknown) => {
              this.port.posted.push(message);
            },
          };
        },
        registerProcessor: (_name: string, ctor: unknown) => {
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor('sonare-test-processor');
      const Ctor = registeredCtor as new (options: {
        processorOptions: {
          sceneJson: string;
          sampleRate: number;
          blockSize: number;
          meterIntervalFrames: number;
        };
      }) => {
        port: { posted: unknown[]; onmessage?: (event: { data: unknown }) => void };
        process: (inputs: Float32Array[][], outputs: Float32Array[][]) => boolean;
      };
      const blockSize = 128;
      const instance = new Ctor({
        processorOptions: {
          sceneJson: mixingScenePresetJson('vocalReverbSend'),
          sampleRate: 48000,
          blockSize,
          meterIntervalFrames: blockSize,
        },
      });
      instance.port.onmessage?.({
        data: {
          type: 'scheduleInsertAutomation',
          stripIndex: 0,
          insertIndex: 0,
          paramId: 0,
          value: 0,
        },
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1;
      vocalR[0] = 1;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(instance.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(instance.port.posted).toHaveLength(1);
      expect(instance.port.posted[0]).toMatchObject({ type: 'meter', frame: blockSize });
      instance.port.onmessage?.({ data: { type: 'destroy' } });
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });

  it('registers in an AudioWorklet-like global scope', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredName = '';
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {},
        registerProcessor: (name: string, ctor: unknown) => {
          registeredName = name;
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor();
      expect(registeredName).toBe('sonare-worklet-processor');
      expect(typeof registeredCtor).toBe('function');
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });
});
