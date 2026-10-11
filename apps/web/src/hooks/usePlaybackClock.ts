import { useEffect } from "react";

import { SAMPLES_PER_SECOND_AT_1X, usePlayback } from "@/state/playback";

/** Drives playback from animation frames while it is playing. Mount once per workspace. */
export function usePlaybackClock(): void {
  const playing = usePlayback((state) => state.playing);
  useEffect(() => {
    if (!playing) return;
    let frame = 0;
    let previous = performance.now();
    let owed = 0;
    const tick = (now: number) => {
      const state = usePlayback.getState();
      owed += ((now - previous) / 1000) * SAMPLES_PER_SECOND_AT_1X * state.speed;
      previous = now;
      const whole = Math.floor(owed);
      if (whole > 0) {
        owed -= whole;
        state.advance(whole);
      }
      if (usePlayback.getState().playing) frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [playing]);
}
