import { nsToMs } from "./format";
import type { LiveEvent, LiveMessage, LiveStatus, SymbolState } from "./protocol";

// Everything the Live terminal knows, held outside React.
//
// The stream can deliver dozens of batches a second. The model absorbs each one in place
// and the store publishes a revision at a bounded rate, so rendering cost follows the
// screen's refresh needs and never the message rate. Every buffer here is bounded: the
// browser never retains an unbounded history.

export const MAX_POINTS = 6_000;
export const MAX_TAPE = 400;
export const MAX_WINDOW_MS = 30 * 60_000;

export type Connection = "idle" | "connecting" | "open" | "closed";

/** One plotted row: the instrument's state just after one applied observation. */
export interface SeriesBuffer {
  x: number[];
  trade: (number | null)[];
  bid: (number | null)[];
  ask: (number | null)[];
  mid: (number | null)[];
  /** Counts every append, so a chart can tell "new data" from "same data". */
  version: number;
}

export interface TapeRow {
  key: number;
  event: LiveEvent | null;
  /** A marker for observations that were not delivered. */
  gap?: { reason: string; dropped: number };
}

function emptySeries(): SeriesBuffer {
  return { x: [], trade: [], bid: [], ask: [], mid: [], version: 0 };
}

export interface ApplyResult {
  /** The client must ask the hub for a snapshot. */
  resync: boolean;
}

export class LiveModel {
  connection: Connection = "idle";
  /** Browser reconnects to the hub, not provider reconnects. */
  reconnects = 0;
  stream: string | null = null;
  seq: number | null = null;
  status: LiveStatus | null = null;
  states: Record<string, SymbolState> = {};
  watchlist: string[] = [];
  /** performance.now() when `status` was last refreshed by the hub. */
  refreshedAt = 0;
  /** performance.now() when each instrument's state, and so its ages, was last refreshed. */
  stateAt = new Map<string, number>();
  series = new Map<string, SeriesBuffer>();
  tape = new Map<string, TapeRow[]>();
  lastLiveSequence = 0;
  gaps = { sequence: 0, slowClient: 0, pipeline: 0, droppedEvents: 0, resyncs: 0 };
  private tapeKey = 0;

  reset(): void {
    this.series.clear();
    this.tape.clear();
    this.states = {};
    this.stateAt.clear();
    this.lastLiveSequence = 0;
  }

  seriesOf(symbol: string): SeriesBuffer {
    let buffer = this.series.get(symbol);
    if (!buffer) {
      buffer = emptySeries();
      this.series.set(symbol, buffer);
    }
    return buffer;
  }

  tapeOf(symbol: string): TapeRow[] {
    return this.tape.get(symbol) ?? [];
  }

  apply(message: LiveMessage, now: number): ApplyResult {
    switch (message.type) {
      case "hello":
      case "pong":
        return { resync: false };
      case "snapshot": {
        if (message.stream !== this.stream) {
          // A different stream is a different feed session: nothing carries over.
          this.reset();
          this.stream = message.stream;
        }
        this.seq = message.seq;
        this.status = message.status;
        this.states = message.states;
        this.watchlist = message.watchlist;
        this.refreshedAt = now;
        this.touch(message.states, now);
        for (const symbol of [...this.series.keys()]) {
          if (!message.watchlist.includes(symbol)) {
            this.series.delete(symbol);
            this.tape.delete(symbol);
          }
        }
        for (const event of message.recent) this.ingest(event);
        return { resync: false };
      }
      case "gap": {
        if (message.scope === "this_client") {
          this.gaps.slowClient += 1;
          this.gaps.droppedEvents += message.dropped_events ?? 0;
          this.markGap("slow reader", message.dropped_events ?? 0);
          // The hub sends this outside the numbered sequence, then a snapshot.
          return { resync: false };
        }
        this.gaps.pipeline += 1;
        this.gaps.droppedEvents += message.dropped_messages;
        this.markGap("service overflow", message.dropped_messages);
        return this.advance(message.seq);
      }
      case "status": {
        const result = this.advance(message.seq);
        this.status = message.status;
        this.states = message.states;
        this.refreshedAt = now;
        this.touch(message.states, now);
        return result;
      }
      case "events": {
        const result = this.advance(message.seq);
        for (const event of message.events) this.ingest(event);
        this.states = { ...this.states, ...message.states };
        this.touch(message.states, now);
        return result;
      }
    }
  }

  private touch(states: Record<string, SymbolState>, now: number): void {
    for (const symbol of Object.keys(states)) this.stateAt.set(symbol, now);
  }

  /** Checks the hub sequence. A skipped number means messages were lost on the way. */
  private advance(seq: number): ApplyResult {
    const expected = this.seq === null ? seq : this.seq + 1;
    this.seq = seq;
    if (seq === expected) return { resync: false };
    this.gaps.sequence += 1;
    this.gaps.resyncs += 1;
    this.markGap("sequence gap", 0);
    return { resync: true };
  }

  private markGap(reason: string, dropped: number): void {
    for (const symbol of this.watchlist) {
      this.pushTape(symbol, { key: ++this.tapeKey, event: null, gap: { reason, dropped } });
    }
  }

  private pushTape(symbol: string, row: TapeRow): void {
    let rows = this.tape.get(symbol);
    if (!rows) {
      rows = [];
      this.tape.set(symbol, rows);
    }
    rows.push(row);
    if (rows.length > MAX_TAPE) rows.splice(0, rows.length - MAX_TAPE);
  }

  private ingest(event: LiveEvent): void {
    if (event.live_sequence <= this.lastLiveSequence) return;
    this.lastLiveSequence = event.live_sequence;
    this.pushTape(event.symbol, { key: ++this.tapeKey, event });
    // Late and repeated records are shown on the tape, flagged, and kept off the chart.
    if (!event.applied) return;
    const buffer = this.seriesOf(event.symbol);
    const x = nsToMs(event.timestamp_ns);
    const last = buffer.x.length - 1;
    const previous = last >= 0 ? last : null;
    let bid = previous === null ? null : (buffer.bid[previous] ?? null);
    let ask = previous === null ? null : (buffer.ask[previous] ?? null);
    let trade: number | null = null;
    if (event.kind === "trade") {
      trade = event.price_micro_usd / 1_000_000;
    } else {
      bid = event.bid_micro_usd === null ? null : event.bid_micro_usd / 1_000_000;
      ask = event.ask_micro_usd === null ? null : event.ask_micro_usd / 1_000_000;
    }
    // A midpoint is drawn only for a two-sided or locked quote, as the core defines it.
    const mid = bid !== null && ask !== null && bid <= ask ? (bid + ask) / 2 : null;
    // Trades and quotes carry independent source clocks, so one can be slightly older
    // than the other's newest. The plot needs ascending x; such a row is placed at the
    // newest time already drawn instead of being moved earlier than it was observed.
    const at = previous !== null && x < (buffer.x[previous] ?? x) ? (buffer.x[previous] ?? x) : x;
    buffer.x.push(at);
    buffer.trade.push(trade);
    buffer.bid.push(bid);
    buffer.ask.push(ask);
    buffer.mid.push(mid);
    buffer.version += 1;
    const horizon = at - MAX_WINDOW_MS;
    let drop = buffer.x.length > MAX_POINTS ? buffer.x.length - MAX_POINTS : 0;
    while (drop < buffer.x.length - 1 && (buffer.x[drop] ?? horizon) < horizon) drop += 1;
    if (drop > 0) {
      for (const column of [buffer.x, buffer.trade, buffer.bid, buffer.ask, buffer.mid]) {
        column.splice(0, drop);
      }
    }
  }
}
