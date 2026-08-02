import { closeSync, openSync, writeSync } from 'node:fs';

/** Write interleaved float samples as a PCM16 WAV without a full PCM copy. */
export function writePcm16Wav(path, samples, sampleRate, channels = 1) {
  if (!Number.isInteger(sampleRate) || sampleRate <= 0) throw new RangeError('sampleRate must be positive');
  if (!Number.isInteger(channels) || channels <= 0 || samples.length % channels !== 0) {
    throw new RangeError('samples must be interleaved for channels');
  }
  const bytesPerSample = 2;
  const dataBytes = samples.length * bytesPerSample;
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + dataBytes, 4);
  header.write('WAVEfmt ', 8);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(sampleRate * channels * bytesPerSample, 28);
  header.writeUInt16LE(channels * bytesPerSample, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(dataBytes, 40);

  const fd = openSync(path, 'w');
  try {
    writeSync(fd, header);
    for (let offset = 0; offset < samples.length; offset += 8192) {
      const count = Math.min(8192, samples.length - offset);
      const pcm = Buffer.allocUnsafe(count * bytesPerSample);
      for (let i = 0; i < count; i += 1) {
        const sample = Math.max(-1, Math.min(1, samples[offset + i]));
        pcm.writeInt16LE(Math.round(sample * 32767), i * bytesPerSample);
      }
      writeSync(fd, pcm);
    }
  } finally {
    closeSync(fd);
  }
}
