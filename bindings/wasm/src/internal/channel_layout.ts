import type { ChannelLayout } from '../audio.js';

/** Ordinal of each {@link ChannelLayout} name in the C enum `SonareChannelLayout`. */
export const CHANNEL_LAYOUT_VALUES: Readonly<Record<ChannelLayout, number>> = {
  mono: 0,
  stereo: 1,
  '5.1': 2,
  '7.1': 3,
};

/** The layout names, in ordinal order. */
export const CHANNEL_LAYOUTS = Object.keys(CHANNEL_LAYOUT_VALUES) as ChannelLayout[];
