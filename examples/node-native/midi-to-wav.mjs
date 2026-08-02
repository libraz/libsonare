import { Project } from '@libraz/libsonare-native';

import { writePcm16Wav } from './wav.mjs';

const [outputPath] = process.argv.slice(2);
if (!outputPath) {
  console.error(`usage: ${process.argv[1]} OUTPUT_WAV`);
  process.exit(2);
}

const project = Project.create();
try {
  project.setSampleRate(48000);
  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(1, 0, 0, 60),
    Project.midiNoteOn(1, 0, 0, 64, 100),
    Project.midiNoteOff(2, 0, 0, 64),
  ]);
  const samples = project.bounceWithSynthInstrument('saw-lead', { numChannels: 2 });
  writePcm16Wav(outputPath, samples, 48000, 2);
} finally {
  project.destroy();
}
