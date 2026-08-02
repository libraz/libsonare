export function downloadPcm16Wav(filename, samples, sampleRate, channels = 1) {
  if (samples.length % channels !== 0) throw new RangeError('samples must be interleaved');
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);
  const write = (offset, value) => view.setUint8(offset, value.charCodeAt(0));
  for (const [offset, value] of [[0, 'R'], [1, 'I'], [2, 'F'], [3, 'F'], [8, 'W'], [9, 'A'], [10, 'V'], [11, 'E'], [12, 'f'], [13, 'm'], [14, 't'], [15, ' '], [36, 'd'], [37, 'a'], [38, 't'], [39, 'a']]) write(offset, value);
  view.setUint32(4, 36 + samples.length * 2, true);
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, channels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * channels * 2, true);
  view.setUint16(32, channels * 2, true);
  view.setUint16(34, 16, true);
  view.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; i += 1) {
    view.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, samples[i])) * 32767), true);
  }
  const url = URL.createObjectURL(new Blob([buffer], { type: 'audio/wav' }));
  const link = Object.assign(document.createElement('a'), { href: url, download: filename });
  link.click();
  URL.revokeObjectURL(url);
}
