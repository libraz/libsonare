/** Convert one normalized float sample to a signed PCM16 code. */
export function floatToPcm16(value) {
  const narrowed = Math.fround(value);
  if (!Number.isFinite(narrowed)) {
    return 0;
  }
  const clamped = Math.max(-1, Math.min(1, narrowed));
  const scaled = Math.fround(clamped * 32768);
  const rounded = scaled < 0 ? Math.ceil(scaled - 0.5) : Math.floor(scaled + 0.5);
  return Math.max(-32768, Math.min(32767, rounded));
}
