import { ErrorCode } from './errors.js';
import { codedError, entryClass } from './internal/entry.js';
import type { RealtimeEngine } from './realtime_engine.js';
import type {
  LiveAudioDevice,
  LiveMidiDevice,
  LiveSessionConfig,
  LiveSessionStatus,
} from './types_engine.js';
import type { ProjectMidiEvent } from './types_project.js';

const UNAVAILABLE_MESSAGE = 'live device support is not compiled in';

function notCompiledIn(): never {
  throw codedError(ErrorCode.NotSupported, UNAVAILABLE_MESSAGE, { reason: 'notCompiledIn' });
}

/**
 * A live audio and MIDI device session. The native Node addon does not include
 * a live-device backend, so construction and every operation report that
 * support is unavailable.
 */
@entryClass('LiveSession')
export class LiveSession implements Disposable {
  /**
   * @param config - Engine to drive, device selections and negotiated audio settings.
   */
  constructor(config: LiveSessionConfig & { readonly engine: RealtimeEngine }) {
    void config;
    notCompiledIn();
  }

  /** Enumerates audio devices. */
  static audioDevices(): LiveAudioDevice[] {
    return notCompiledIn();
  }

  /** Enumerates MIDI input sources. */
  static midiSources(): LiveMidiDevice[] {
    return notCompiledIn();
  }

  /** Reads the session state and device counters. */
  status(): LiveSessionStatus {
    return notCompiledIn();
  }

  /** Drains captured channel-voice MIDI events. */
  capturedMidi(): ProjectMidiEvent[] {
    return notCompiledIn();
  }

  /** Stops and releases the session. */
  destroy(): void {
    notCompiledIn();
  }

  /** Releases the session when used with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
