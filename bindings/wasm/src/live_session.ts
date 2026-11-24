import { ErrorCode } from './errors.js';
import { codedError, entryClass } from './internal/entry.js';
import type { ProjectMidiEvent } from './project_types.js';
import type {
  LiveAudioDevice,
  LiveMidiDevice,
  LiveSessionConfig,
  LiveSessionStatus,
} from './public_types_realtime.js';
import type { RealtimeEngine } from './realtime_engine.js';

const LIVE_DEVICES_UNAVAILABLE = 'live device support is not compiled in';

function unavailable(): ReturnType<typeof codedError> {
  return codedError(ErrorCode.NotSupported, LIVE_DEVICES_UNAVAILABLE, {
    reason: 'notCompiledIn',
  });
}

/** Native audio and MIDI device session. This build reports the API but does not open devices. */
@entryClass('LiveSession')
export class LiveSession {
  /** Device enumeration is unavailable in this build. */
  static audioDevices(): LiveAudioDevice[] {
    throw unavailable();
  }

  /** MIDI source enumeration is unavailable in this build. */
  static midiSources(): LiveMidiDevice[] {
    throw unavailable();
  }

  /**
   * Opening a native device session is unavailable in this build.
   * @param config - The realtime engine and its device selection settings.
   */
  constructor(config: LiveSessionConfig & { readonly engine: RealtimeEngine }) {
    void config;
    throw unavailable();
  }

  /** Reads device state and counters. */
  status(): LiveSessionStatus {
    throw unavailable();
  }

  /** Drains captured MIDI events. */
  capturedMidi(): ProjectMidiEvent[] {
    throw unavailable();
  }

  /** Releases the device session. */
  destroy(): void {
    throw unavailable();
  }

  /** Releases the session through {@link destroy}, so it works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
