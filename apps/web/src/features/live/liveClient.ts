import { LIVE_PROTOCOL, type LiveMessage } from "./protocol";

// The browser's one connection to the hub. It reconnects on its own with bounded backoff
// and reports every change of connection state, so the screen can always say whether what
// it shows is arriving now or is the last thing received.

export interface LiveClientOptions {
  url: string;
  onMessage: (message: LiveMessage) => { resync: boolean };
  onConnection: (state: "connecting" | "open" | "closed", reconnects: number) => void;
  /** Injected in tests. */
  socket?: (url: string) => WebSocket;
  baseDelayMs?: number;
  maxDelayMs?: number;
}

export function liveStreamUrl(location: Pick<Location, "protocol" | "host">): string {
  return `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/api/v1/live/stream`;
}

export class LiveClient {
  private socket: WebSocket | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private attempt = 0;
  private reconnects = 0;
  private stopped = true;

  constructor(private readonly options: LiveClientOptions) {}

  start(): void {
    if (!this.stopped) return;
    this.stopped = false;
    this.open();
  }

  stop(): void {
    this.stopped = true;
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    const socket = this.socket;
    this.socket = null;
    if (socket) {
      socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
      socket.close();
    }
  }

  resync(): void {
    if (this.socket?.readyState === 1) this.socket.send(JSON.stringify({ type: "resync" }));
  }

  private open(): void {
    this.options.onConnection("connecting", this.reconnects);
    const socket = (this.options.socket ?? ((url) => new WebSocket(url)))(this.options.url);
    this.socket = socket;
    socket.onopen = () => {
      this.attempt = 0;
      this.options.onConnection("open", this.reconnects);
    };
    socket.onmessage = (event: MessageEvent) => {
      let message: LiveMessage;
      try {
        message = JSON.parse(String(event.data)) as LiveMessage;
      } catch {
        return;
      }
      // A newer protocol is not guessed at.
      if (message.v !== LIVE_PROTOCOL) return;
      if (this.options.onMessage(message).resync) this.resync();
    };
    socket.onclose = () => {
      if (this.socket !== socket) return;
      this.socket = null;
      this.options.onConnection("closed", this.reconnects);
      if (this.stopped) return;
      const base = this.options.baseDelayMs ?? 500;
      const ceiling = this.options.maxDelayMs ?? 10_000;
      const step = Math.min(ceiling, base * 2 ** Math.min(this.attempt, 16));
      this.attempt += 1;
      this.reconnects += 1;
      // Half fixed, half random: never a tight loop, never a synchronized herd.
      this.timer = setTimeout(() => this.open(), step * (0.5 + Math.random() * 0.5));
    };
  }
}
