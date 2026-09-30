import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, RealtimeEngine } from '../src/index.js';
import { midi1Word, rms } from './_engine_signals.js';

describe('RealtimeEngine native binding', () => {
  it('renders scheduled MIDI clips through built-in instruments', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    engine.setMidiClips([
      {
        id: 1,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 8192,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
          { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    const out = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(rms(out[0]), rms(out[1]))).toBeGreaterThan(0);

    let badGroupError: unknown;
    try {
      engine.setMidiClips([
        {
          id: 2,
          trackId: 5,
          destinationId: 5,
          events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1, group: 16 }],
        },
      ]);
    } catch (error) {
      badGroupError = error;
    }
    expect(isSonareError(badGroupError)).toBe(true);
    if (!isSonareError(badGroupError)) {
      throw new Error('expected SonareError');
    }
    expect(badGroupError.code).toBe(ErrorCode.InvalidParameter);

    let badChannelError: unknown;
    try {
      engine.pushMidiNoteOn(5, 0, 16, 60, 100);
    } catch (error) {
      badChannelError = error;
    }
    expect(isSonareError(badChannelError)).toBe(true);
    if (!isSonareError(badChannelError)) {
      throw new Error('expected SonareError');
    }
    expect(badChannelError.code).toBe(ErrorCode.InvalidParameter);

    let badSoundFontError: unknown;
    try {
      engine.loadSoundFont(new Uint8Array([0x6e, 0x6f, 0x74, 0x20, 0x73, 0x66, 0x32]));
    } catch (error) {
      badSoundFontError = error;
    }
    expect(isSonareError(badSoundFontError)).toBe(true);
    if (!isSonareError(badSoundFontError)) {
      throw new Error('expected SonareError');
    }
    expect(badSoundFontError.code).toBe(ErrorCode.InvalidFormat);

    engine.setMidiClips([]);
    engine.destroy();
  });

  it('rejects MIDI byte arguments that would wrap through the uint8 cast', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiInputSource(0);
    // group 256 must NOT silently wrap to a valid group 0 — it must throw.
    expect(() => engine.pushMidiInputNoteOn(256, 0, 60, 100)).toThrow();
    // A channel of 256 likewise wraps to 0 under a plain cast; reject it.
    expect(() => engine.pushMidiInputNoteOn(0, 256, 60, 100)).toThrow();
    // A valid in-range call still works once an input source is enabled.
    expect(() => engine.pushMidiInputNoteOn(0, 0, 60, 100)).not.toThrow();
    engine.destroy();
  });

  it('takes a clip event group from word0 and rejects one that would wrap', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.prepare(48000, 128, 16, 16);
    engine.setMidiDestinationExternal(7, true);

    // A group of 256 narrows to 0 in the uint8 struct field, which is a valid
    // group, so the C-ABI range check would accept it. Reject the wrap here.
    expect(() =>
      engine.setMidiClips([
        {
          id: 1,
          trackId: 7,
          destinationId: 7,
          events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1, group: 256 }],
        },
      ]),
    ).toThrow();

    // `group` is ignored on input: the pair below is authored on one word0
    // group and disagrees only in the redundant field. Read under two groups
    // the note-off would not match its note-on, so the note would still be
    // considered sounding and stopping would emit a hang-note release.
    const group = 3;
    const packed = (status: number) =>
      midi1Word(status, 0, 60, status === 0x9 ? 100 : 0) | (group << 24);
    engine.setMidiClips([
      {
        id: 2,
        trackId: 7,
        destinationId: 7,
        lengthSamples: 256,
        events: [
          { renderFrame: 0, word0: packed(0x9), wordCount: 1, group: 5 },
          { renderFrame: 48, word0: packed(0x8), wordCount: 1, group: 0 },
        ],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(engine.drainExternalMidi().length).toBe(2);

    engine.stop();
    engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(engine.drainExternalMidi().length).toBe(0);
    engine.destroy();
  });

  it('drains external MIDI routing to the host', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Route destination 5 to the external queue instead of an instrument.
    engine.setMidiDestinationExternal(5, true);
    engine.setMidiClips([
      {
        id: 7,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 256,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 1, 64, 110), wordCount: 1 },
          { renderFrame: 48, word0: midi1Word(0x8, 1, 64, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const drained = engine.drainExternalMidi(16);
    expect(drained.length).toBe(2);
    expect(drained[0].destinationId).toBe(5);
    expect(drained[0].bytes.length).toBe(3);
    expect(drained[0].bytes[0] & 0xf0).toBe(0x90); // note on
    expect(drained[0].renderFrame).toBe(0);
    expect(drained[1].destinationId).toBe(5);
    expect(drained[1].bytes[0] & 0xf0).toBe(0x80); // note off
    expect(drained[1].renderFrame).toBe(48);

    expect(engine.externalMidiDroppedCount()).toBe(0);
    engine.destroy();
  });

  it('drains a MIDI 2.0 RPN as its four MIDI 1.0 CC messages', () => {
    // RPN 0/0 on channel 1, Data Entry MSB 12: CC 101, 100, 6, 38.
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiDestinationExternal(5, true);
    const rpnWord0 = ((0x4 << 28) | (0x2 << 20) | (1 << 16)) >>> 0;
    engine.setMidiClips([
      {
        id: 7,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 256,
        events: [{ renderFrame: 0, word0: rpnWord0, word1: (12 << 25) >>> 0, wordCount: 2 }],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(() => engine.drainExternalMidi(3)).toThrow(RangeError);
    const drained = engine.drainExternalMidi(4);
    expect(drained.map((e) => Array.from(e.bytes))).toEqual([
      [0xb1, 101, 0],
      [0xb1, 100, 0],
      [0xb1, 6, 12],
      [0xb1, 38, 0],
    ]);
    engine.destroy();
  });

  it('reports external-destination table overflow instead of silently routing internally', () => {
    const engine = new RealtimeEngine(48000, 128);
    for (let id = 0; id < 16; id++) {
      engine.setMidiDestinationExternal(id, true);
    }
    // Re-marking an existing destination is idempotent.
    expect(() => engine.setMidiDestinationExternal(3, true)).not.toThrow();
    // The 17th distinct destination overflows the slot table.
    expect(() => engine.setMidiDestinationExternal(99, true)).toThrow();
    // Freeing a slot lets the next mark succeed.
    engine.setMidiDestinationExternal(3, false);
    expect(() => engine.setMidiDestinationExternal(99, true)).not.toThrow();
    engine.destroy();
  });

  it('drains external MIDI losslessly when maxRecords is below the queued count', () => {
    // Regression for the over-drain data-loss bug: a small maxRecords cap must
    // not discard events the native call already dequeued. Queue many events,
    // drain in batches with a cap that is neither a multiple of the internal
    // 256-slot buffer nor large enough to take everything at once, and assert
    // every event survives across repeated calls.
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiDestinationExternal(5, true);
    const events = [];
    const kNotes = 24; // 48 events total (on+off), well above the cap below
    for (let i = 0; i < kNotes; i++) {
      events.push({ renderFrame: i, word0: midi1Word(0x9, 1, 40 + i, 100), wordCount: 1 });
      events.push({ renderFrame: i, word0: midi1Word(0x8, 1, 40 + i, 0), wordCount: 1 });
    }
    engine.setMidiClips([{ id: 7, trackId: 5, destinationId: 5, lengthSamples: 256, events }]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const collected = [];
    for (let guard = 0; guard < 1000; guard++) {
      const batch = engine.drainExternalMidi(5); // cap < count, not a 256 multiple
      if (batch.length === 0) {
        break;
      }
      expect(batch.length).toBeLessThanOrEqual(5);
      for (const ev of batch) {
        collected.push(ev);
      }
    }

    expect(collected.length).toBe(2 * kNotes);
    expect(engine.externalMidiDroppedCount()).toBe(0);
    // Losslessness: every queued note-on and note-off survives the capped drain.
    const noteOns = new Set();
    const noteOffs = new Set();
    for (const ev of collected) {
      const status = ev.bytes[0] & 0xf0;
      if (status === 0x90) {
        noteOns.add(ev.bytes[1]);
      } else if (status === 0x80) {
        noteOffs.add(ev.bytes[1]);
      }
    }
    for (let i = 0; i < kNotes; i++) {
      expect(noteOns.has(40 + i)).toBe(true);
      expect(noteOffs.has(40 + i)).toBe(true);
    }
    engine.destroy();
  });

  it('forwards MIDI clock/transport to the external queue', () => {
    const engine = new RealtimeEngine(48000, 24000);
    engine.setTempo(120);
    engine.setExternalMidiClockEnabled(true);
    engine.play(0);

    engine.process([new Float32Array(24000), new Float32Array(24000)]);

    // One Start plus 24 clock ticks (one every 1000 samples at 120 BPM).
    const drained = engine.drainExternalMidi(64);
    expect(drained.length).toBe(25);
    for (const event of drained) {
      expect(event.destinationId).toBe(0xffffffff);
      expect(event.bytes.length).toBe(1);
    }
    expect(drained[0].bytes[0]).toBe(0xfa); // Start
    expect(drained[1].bytes[0]).toBe(0xf8); // Clock
    engine.destroy();
  });

  /**
   * An engine with `queued` external-MIDI messages already waiting in the
   * output queue. One note-on plus one note-off lower to one message each.
   */
  const engineWithQueuedExternalMidi = (): { engine: RealtimeEngine; queued: number } => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiDestinationExternal(5, true);
    engine.setMidiClips([
      {
        id: 7,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 256,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 1, 64, 110), wordCount: 1 },
          { renderFrame: 48, word0: midi1Word(0x8, 1, 64, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);
    return { engine, queued: 2 };
  };

  // The maxRecords domain, one row per boundary of the argument space: the
  // type boundary, the non-integer and non-finite boundaries, the negative and
  // above-MAX_SAFE_INTEGER boundaries, the "cannot make forward progress"
  // window below the 3-message worst case, and the first accepting value.
  const MAX_RECORDS_CASES: Array<{ label: string; value: unknown; outcome: unknown }> = [
    { label: 'a string', value: '16', outcome: TypeError },
    { label: 'a boolean', value: true, outcome: TypeError },
    { label: 'NaN', value: Number.NaN, outcome: RangeError },
    { label: 'Infinity', value: Number.POSITIVE_INFINITY, outcome: RangeError },
    { label: 'a fraction', value: 2.5, outcome: RangeError },
    { label: '-1', value: -1, outcome: RangeError },
    { label: 'past MAX_SAFE_INTEGER', value: 2 ** 53, outcome: RangeError },
    { label: '1', value: 1, outcome: RangeError },
    { label: '2', value: 2, outcome: RangeError },
    { label: '3', value: 3, outcome: RangeError },
    { label: '0', value: 0, outcome: 'empty' },
    { label: '4', value: 4, outcome: 'drains' },
    { label: '1024', value: 1024, outcome: 'drains' },
    { label: 'undefined', value: undefined, outcome: 'drains' },
    { label: 'null', value: null, outcome: 'drains' },
  ];

  it('applies one maxRecords domain rule across the whole argument space', () => {
    // Drives every boundary against both queue states: an out-of-domain
    // maxRecords must be rejected identically whether or not there is anything
    // to drain, and must never consume a queued event on the way out.
    for (const { label, value, outcome } of MAX_RECORDS_CASES) {
      for (const queueState of ['empty', 'non-empty'] as const) {
        const { engine, queued } =
          queueState === 'non-empty'
            ? engineWithQueuedExternalMidi()
            : { engine: new RealtimeEngine(48000, 128), queued: 0 };
        try {
          const call = () => (engine.drainExternalMidi as (max: unknown) => unknown[])(value);
          let drained = 0;
          if (outcome === 'empty') {
            expect(call(), `${label} on a ${queueState} queue`).toEqual([]);
          } else if (outcome === 'drains') {
            drained = call().length;
            // Forward progress: an accepted budget never returns empty while
            // events are queued, which is the whole point of the domain rule.
            expect(drained > 0, `${label} on a ${queueState} queue`).toBe(queued > 0);
          } else {
            expect(call, `${label} on a ${queueState} queue`).toThrow(outcome as ErrorConstructor);
          }
          // Lossless: nothing above consumed a queued event it did not return,
          // so the remainder is still drainable with a generous budget.
          expect(engine.drainExternalMidi(1024).length, `${label} left the queue drainable`).toBe(
            queued - drained,
          );
        } finally {
          engine.destroy();
        }
      }
    }
  });

  it('rejects a bad maxRecords with the same error class as the sibling drains', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      const drains = [
        (max: unknown) => (engine.drainTelemetry as (m: unknown) => unknown)(max),
        (max: unknown) => (engine.drainMeterTelemetry as (m: unknown) => unknown)(max),
        (max: unknown) => (engine.drainMeterTelemetryWide as (m: unknown) => unknown)(max),
        (max: unknown) => (engine.drainScopeTelemetry as (m: unknown) => unknown)(max),
        (max: unknown) => (engine.drainExternalMidi as (m: unknown) => unknown)(max),
      ];
      for (const drain of drains) {
        expect(() => drain(-1)).toThrow(RangeError);
        expect(() => drain(2.5)).toThrow(RangeError);
        expect(() => drain(Number.NaN)).toThrow(RangeError);
        expect(() => drain('16')).toThrow(TypeError);
        expect(drain(0)).toEqual([]);
      }
    } finally {
      engine.destroy();
    }
  });

  it('bounds every drained record to the fixed 3-byte message array', () => {
    // Covers both ends of the byte_count range in one drain: clock bytes lower
    // to one byte, channel-voice messages to three.
    const engine = new RealtimeEngine(48000, 24000);
    engine.setTempo(120);
    engine.setExternalMidiClockEnabled(true);
    engine.setMidiDestinationExternal(5, true);
    engine.setMidiClips([
      {
        id: 9,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 24000,
        events: [{ renderFrame: 0, word0: midi1Word(0x9, 1, 64, 110), wordCount: 1 }],
      },
    ]);
    engine.play(0);
    engine.process([new Float32Array(24000), new Float32Array(24000)]);

    const drained = engine.drainExternalMidi(1024);
    const lengths = new Set(drained.map((event) => event.bytes.length));
    // Positive control: a run that lowered only clock bytes would make the
    // bound below hold without ever exercising a full-width record.
    expect(lengths.has(1)).toBe(true);
    expect(lengths.has(3)).toBe(true);
    for (const event of drained) {
      expect(event.bytes.length).toBeGreaterThanOrEqual(1);
      expect(event.bytes.length).toBeLessThanOrEqual(3);
      for (const byte of event.bytes) {
        expect(Number.isInteger(byte)).toBe(true);
        expect(byte).toBeGreaterThanOrEqual(0);
        expect(byte).toBeLessThanOrEqual(0xff);
      }
    }
    engine.destroy();
  });
});
