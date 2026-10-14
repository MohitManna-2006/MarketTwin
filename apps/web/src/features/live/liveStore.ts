import { create } from "zustand";

import { LiveClient, liveStreamUrl } from "./liveClient";
import { LiveModel } from "./liveModel";
import type { LiveMessage } from "./protocol";

// React sees the live model through one number. The model changes at the stream's rate;
// `revision` changes at most every PUBLISH_MS, and components re-read the model when it
// does. The chart reads the model on animation frames and does not use the revision.

const PUBLISH_MS = 120;

interface LiveStore {
  revision: number;
  selected: string | null;
  select: (symbol: string) => void;
}

export const useLive = create<LiveStore>((set) => ({
  revision: 0,
  selected: null,
  select: (symbol) => set({ selected: symbol }),
}));

let model = new LiveModel();
let pending: ReturnType<typeof setTimeout> | null = null;

export function liveModel(): LiveModel {
  return model;
}

/** Publishes now. Tests call this instead of waiting for the timer. */
export function publishLive(): void {
  if (pending !== null) clearTimeout(pending);
  pending = null;
  useLive.setState((state) => ({ revision: state.revision + 1 }));
}

function schedule(): void {
  pending ??= setTimeout(publishLive, PUBLISH_MS);
}

export function ingestLive(message: LiveMessage): { resync: boolean } {
  const result = model.apply(message, performance.now());
  // A snapshot or a status change is shown at once; deltas are batched.
  if (message.type === "events") schedule();
  else publishLive();
  return result;
}

export function resetLive(): void {
  if (pending !== null) clearTimeout(pending);
  pending = null;
  model = new LiveModel();
  useLive.setState({ revision: 0, selected: null });
}

let client: LiveClient | null = null;
let users = 0;

/** Opens the hub connection for the Live terminal; the returned function releases it. */
export function connectLive(): () => void {
  users += 1;
  if (!client) {
    client = new LiveClient({
      url: liveStreamUrl(window.location),
      onMessage: ingestLive,
      onConnection: (state, reconnects) => {
        model.connection = state;
        model.reconnects = reconnects;
        publishLive();
      },
    });
    client.start();
  }
  return () => {
    users -= 1;
    if (users === 0 && client) {
      client.stop();
      client = null;
      model.connection = "idle";
      publishLive();
    }
  };
}

export function requestLiveResync(): void {
  client?.resync();
}
