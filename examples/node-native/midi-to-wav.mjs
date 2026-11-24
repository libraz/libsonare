import { Project, midiNoteOff, midiNoteOn } from '@libraz/libsonare-native';

import { writePcm16Wav } from './wav.mjs';

const [outputPath] = process.argv.slice(2);
if (!outputPath) {
  console.error(`usage: ${process.argv[1]} OUTPUT_WAV`);
  process.exit(2);
}

const project = new Project();
try {
  project.setSampleRate(48000);
  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60 }),
    midiNoteOn({ ppq: 1, group: 0, channel: 0, note: 64, velocity: 100 }),
    midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 64 }),
  ]);
  const { channels, sampleRate } = project.bounce({ numChannels: 2, synthInstruments: [{ instrument: { preset: 'sawLead' } }] });
  const samples = new Float32Array(channels[0].length * 2);
  channels[0].forEach((left, i) => {
    samples[2 * i] = left;
    samples[2 * i + 1] = channels[1][i];
  });
  writePcm16Wav(outputPath, samples, sampleRate, channels.length);
} finally {
  project.destroy();
}
