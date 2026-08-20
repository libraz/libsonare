// The worklet entry ships as its own self-contained bundle, so the classes it
// shares with the main entry are emitted into both declaration files. Both
// carry `private` members, which makes them nominal: two textually identical
// declarations are mutually unassignable, and a consumer could not hand an
// object obtained from one entry to the other. These assignments consume the
// built `dist/` because that is the only place the emitted declarations exist.
import type { ClipPageProvider, RealtimeEngine } from '../../dist/index.js';
import type { OpfsClipStream, SonareEngineOptions } from '../../dist/worklet.js';

// `SonareEngine` accepts an engine created through the main entry.
const engineFromIndex = null as unknown as RealtimeEngine;
const engineIntoWorklet: SonareEngineOptions['offlineEngine'] = engineFromIndex;

// A provider returned by the worklet entry is scheduled through the main entry.
const providerFromWorklet = null as unknown as OpfsClipStream['provider'];
const providerIntoIndex: ClipPageProvider = providerFromWorklet;

void engineIntoWorklet;
void providerIntoIndex;
