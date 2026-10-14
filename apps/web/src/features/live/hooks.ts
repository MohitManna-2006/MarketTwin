import { useEffect, useState } from "react";

import type { LiveModel } from "./liveModel";
import { connectLive, liveModel, useLive } from "./liveStore";

/** The live model, re-read whenever the store publishes a revision. */
export function useLiveModel(): LiveModel {
  useLive((state) => state.revision);
  return liveModel();
}

/** Holds the hub connection open while the Live terminal is on screen. */
export function useLiveConnection(): void {
  useEffect(() => connectLive(), []);
}

/** A monotonic "now" that advances on a timer, so ages keep counting between messages. */
export function useNow(intervalMs = 500): number {
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    const timer = setInterval(() => setNow(performance.now()), intervalMs);
    return () => clearInterval(timer);
  }, [intervalMs]);
  return now;
}
