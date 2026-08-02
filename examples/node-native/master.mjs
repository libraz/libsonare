import { Audio } from '@libraz/libsonare-native';

import { writePcm16Wav } from './wav.mjs';

const [inputPath, outputPath] = process.argv.slice(2);
if (!inputPath || !outputPath) {
  console.error(`usage: ${process.argv[1]} INPUT_AUDIO OUTPUT_WAV`);
  process.exit(2);
}

const audio = Audio.fromFile(inputPath);
try {
  const result = audio.masteringChain({
    loudness: { enabled: true, targetLufs: -14, ceilingDb: -1 },
  });
  writePcm16Wav(outputPath, result.samples, result.sampleRate);
  console.log(`LUFS: ${result.inputLufs.toFixed(1)} -> ${result.outputLufs.toFixed(1)}`);
  console.log(JSON.stringify(result.report, null, 2));
} finally {
  audio.destroy();
}
