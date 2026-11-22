import type { WebMidiAudioContext, WebMidiEngineInputOptions } from '../../dist/index.js';

const context: WebMidiAudioContext = {
  getOutputTimestamp: () => ({ contextTime: 0, performanceTime: 0 }),
  sampleRate: 48000,
};
const options: WebMidiEngineInputOptions = { contextTimeSec: 0 };
void context;
void options;
