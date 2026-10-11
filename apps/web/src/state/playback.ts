import { create } from "zustand";

import { clamp } from "@/lib/time/asof";

export const SPEEDS = [0.5, 1, 2, 5, 10] as const;
export type Speed = (typeof SPEEDS)[number];

/**
 * Playback steps through recorded samples. Its speed is a viewing pace and nothing else:
 * at 1x the cursor advances five recorded samples per second of wall-clock time,
 * whatever simulated interval separates them. The run itself finished long ago.
 */
export const SAMPLES_PER_SECOND_AT_1X = 5;

export interface PlaybackState {
  /** What the cursor belongs to. A different key resets it. */
  key: string | null;
  /** Number of recorded samples. */
  count: number;
  /** Index of the sample under the cursor. */
  index: number;
  playing: boolean;
  speed: Speed;
  attach: (key: string, count: number, index?: number) => void;
  seek: (index: number) => void;
  step: (delta: number) => void;
  play: () => void;
  pause: () => void;
  toggle: () => void;
  setSpeed: (speed: Speed) => void;
  faster: () => void;
  slower: () => void;
  /** Advances by a whole number of samples and stops at the last one. */
  advance: (samples: number) => void;
}

export const usePlayback = create<PlaybackState>((set, get) => ({
  key: null,
  count: 0,
  index: 0,
  playing: false,
  speed: 1,
  attach: (key, count, index) => {
    const state = get();
    if (state.key === key && state.count === count) return;
    set({
      key,
      count,
      index: clamp(index ?? Math.max(0, count - 1), 0, Math.max(0, count - 1)),
      playing: false,
    });
  },
  seek: (index) =>
    set((state) => ({ index: clamp(Math.round(index), 0, Math.max(0, state.count - 1)) })),
  step: (delta) =>
    set((state) => ({
      index: clamp(state.index + delta, 0, Math.max(0, state.count - 1)),
      playing: false,
    })),
  play: () =>
    set((state) => {
      if (state.count < 2) return {};
      // Play from the start when the cursor is already on the last sample.
      return { playing: true, index: state.index >= state.count - 1 ? 0 : state.index };
    }),
  pause: () => set({ playing: false }),
  toggle: () => (get().playing ? get().pause() : get().play()),
  setSpeed: (speed) => set({ speed }),
  faster: () =>
    set((state) => ({
      speed: SPEEDS[Math.min(SPEEDS.length - 1, SPEEDS.indexOf(state.speed) + 1)] ?? 1,
    })),
  slower: () =>
    set((state) => ({ speed: SPEEDS[Math.max(0, SPEEDS.indexOf(state.speed) - 1)] ?? 1 })),
  advance: (samples) =>
    set((state) => {
      const last = Math.max(0, state.count - 1);
      const index = Math.min(last, state.index + samples);
      return { index, playing: index < last };
    }),
}));
