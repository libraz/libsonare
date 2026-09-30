export const rms = (data: Float32Array): number => {
  let sum = 0;
  for (const value of data) {
    sum += value * value;
  }
  return Math.sqrt(sum / data.length);
};

export const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
  (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;
