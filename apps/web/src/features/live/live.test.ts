import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { chartFrame } from "./chartFrame";
import { agesOf, availabilityOf, headline, noticesOf } from "./derive";
import { formatAge, formatEventTime, formatMicroUsd } from "./format";
import { LiveClient, liveStreamUrl } from "./liveClient";
import { LiveModel, MAX_POINTS, MAX_TAPE } from "./liveModel";
import type { LiveMessage } from "./protocol";
import {
  at,
  events,
  liveStatus,
  quote,
  resetSequence,
  snapshot,
  statusMessage,
  symbolState,
  trade,
} from "./testing";

beforeEach(() => resetSequence());

function connected(status = liveStatus()): LiveModel {
  const model = new LiveModel();
  model.connection = "open";
  model.apply(snapshot(10, status), 0);
  return model;
}

describe("formatting", () => {
  it("renders micro-USD exactly, without binary floating point", () => {
    expect(formatMicroUsd(170_100_000)).toBe("170.10");
    expect(formatMicroUsd(126_550_000)).toBe("126.55");
    expect(formatMicroUsd(87_661_234)).toBe("87.661234");
    expect(formatMicroUsd(1_234_567_000_000)).toBe("1,234,567.00");
    expect(formatMicroUsd(-20_000)).toBe("−0.02");
    expect(formatMicroUsd(0)).toBe("0.00");
    // Missing is a dash, never a zero.
    expect(formatMicroUsd(null)).toBe("—");
    expect(formatMicroUsd(undefined)).toBe("—");
  });

  it("keeps exactly the fractional digits the source sent", () => {
    expect(formatEventTime("1710163800335689322", 9, "utc")).toBe("13:30:00.335689322");
    expect(formatEventTime("1710163800208000000", 3, "utc")).toBe("13:30:00.208");
    expect(formatEventTime("1710163800000000000", 0, "utc")).toBe("13:30:00");
    expect(formatEventTime("1710163800000000000", 0, "eastern")).toBe("09:30:00");
  });

  it("formats ages for a status line", () => {
    expect(formatAge(400)).toBe("0.4 s");
    expect(formatAge(12_300)).toBe("12 s");
    expect(formatAge(185_000)).toBe("3 min 05 s");
    expect(formatAge(8_040_000)).toBe("2 h 14 min");
    expect(formatAge(null)).toBe("—");
  });
});

describe("live model", () => {
  it("builds the watchlist state from a snapshot and applies trade and quote deltas", () => {
    const model = connected();
    expect(model.watchlist).toEqual(["AAPL", "NVDA", "MSFT", "SPY"]);
    const q = quote("AAPL", 170_090_000, 170_110_000, 0);
    const t = trade("AAPL", 170_100_000, 10);
    const result = model.apply(events(11, [q, t], { AAPL: symbolState("AAPL", t, q) }), 50);
    expect(result.resync).toBe(false);
    expect(model.states.AAPL?.last_trade?.price_micro_usd).toBe(170_100_000);
    expect(model.states.AAPL?.spread_micro_usd).toBe(20_000);
    expect(model.stateAt.get("AAPL")).toBe(50);
    expect(model.tapeOf("AAPL").map((row) => row.event?.kind)).toEqual(["quote", "trade"]);
    expect(model.tapeOf("NVDA")).toEqual([]);
  });

  it("charts applied observations only, carries the quote forward, and never invents a midpoint", () => {
    const model = connected();
    model.apply(
      events(11, [
        quote("AAPL", 10_000_000, 10_020_000, 0),
        trade("AAPL", 10_010_000, 10),
        quote("AAPL", 10_030_000, 10_020_000, 20),
        quote("AAPL", 10_030_000, null, 30),
        trade("AAPL", 10_000_000, 5, { classification: "out_of_order", applied: false }),
        trade("AAPL", 10_010_000, 10, { classification: "duplicate", applied: false }),
      ]),
      0,
    );
    const series = model.seriesOf("AAPL");
    expect(series.x).toHaveLength(4);
    expect(series.trade).toEqual([null, 10.01, null, null]);
    expect(series.bid).toEqual([10, 10, 10.03, 10.03]);
    expect(series.ask).toEqual([10.02, 10.02, 10.02, null]);
    // Two-sided: midpoint. Crossed or one-sided: none.
    expect(series.mid).toEqual([10.01, 10.01, null, null]);
    expect(series.version).toBe(4);
    // Late and repeated records are on the tape, flagged, and off the chart.
    expect(model.tapeOf("AAPL")).toHaveLength(6);
    expect(model.tapeOf("AAPL").at(-1)?.event?.classification).toBe("duplicate");
  });

  it("frames the chart on the window that ends at the newest observation", () => {
    const model = connected();
    const list = Array.from({ length: 120 }, (_, index) =>
      trade("AAPL", 10_000_000 + index * 10_000, index * 1_000),
    );
    model.apply(events(11, list), 0);
    const frame = chartFrame(model.seriesOf("AAPL"), 60_000);
    const end = Number(at(119_000).slice(0, -6));
    expect(frame.range).toEqual([end - 60_000, end]);
    // One row before the window is kept so a step line enters from the left edge.
    expect(frame.data[0]).toHaveLength(62);
    expect(chartFrame(model.seriesOf("AAPL"), 1_800_000).range[0]).toBe(Number(at(0).slice(0, -6)));
    expect(chartFrame(model.seriesOf("NVDA"), 60_000).data[0]).toHaveLength(0);
  });

  it("bounds every buffer however long the stream runs", () => {
    const model = connected();
    for (let batch = 0; batch < 20; batch += 1) {
      const list = Array.from({ length: 500 }, (_, index) =>
        trade("AAPL", 10_000_000, batch * 500 + index),
      );
      model.apply(events(11 + batch, list), 0);
    }
    expect(model.seriesOf("AAPL").x.length).toBeLessThanOrEqual(MAX_POINTS);
    expect(model.seriesOf("AAPL").trade.length).toBe(model.seriesOf("AAPL").x.length);
    expect(model.tapeOf("AAPL").length).toBe(MAX_TAPE);
    // Observations older than the longest window are dropped by time as well.
    const sparse = connected();
    sparse.apply(events(11, [trade("AAPL", 1_000_000, 0)]), 0);
    sparse.apply(events(12, [trade("AAPL", 1_000_000, 31 * 60_000)]), 0);
    expect(sparse.seriesOf("AAPL").x).toHaveLength(1);
  });

  it("detects a skipped hub sequence, marks the gap, and asks for a resync", () => {
    const model = connected();
    expect(model.apply(events(11, [trade("AAPL", 1_000_000, 0)]), 0).resync).toBe(false);
    const result = model.apply(events(14, [trade("AAPL", 1_010_000, 10)]), 0);
    expect(result.resync).toBe(true);
    expect(model.gaps.sequence).toBe(1);
    expect(model.tapeOf("AAPL").some((row) => row.gap?.reason === "sequence gap")).toBe(true);
    // The snapshot that answers the resync does not replay what is already held.
    const before = model.seriesOf("AAPL").x.length;
    model.apply(snapshot(14, liveStatus(), {}, [trade("AAPL", 1_020_000, 20)]), 0);
    expect(model.seriesOf("AAPL").x.length).toBe(before + 1);
    expect(model.apply(events(15, []), 0).resync).toBe(false);
  });

  it("records a slow-reader gap and a service overflow as lost observations", () => {
    const model = connected();
    const base = { v: 1, stream: "stream-a", server_wall_ns: at(0) };
    model.apply(
      {
        ...base,
        seq: 10,
        type: "gap",
        reason: "slow_client",
        scope: "this_client",
        dropped_messages: 4,
        dropped_events: 37,
      },
      0,
    );
    expect(model.gaps.slowClient).toBe(1);
    expect(model.gaps.droppedEvents).toBe(37);
    expect(model.tapeOf("AAPL").at(-1)?.gap).toEqual({ reason: "slow reader", dropped: 37 });
    const overflow = model.apply(
      {
        ...base,
        seq: 11,
        type: "gap",
        reason: "pipeline_overflow",
        scope: "all_clients",
        dropped_messages: 12,
      },
      0,
    );
    expect(overflow.resync).toBe(false);
    expect(model.gaps.pipeline).toBe(1);
    expect(model.gaps.droppedEvents).toBe(49);
  });

  it("starts again when the hub announces a different stream", () => {
    const model = connected();
    model.apply(events(11, [trade("AAPL", 1_000_000, 0)]), 0);
    resetSequence();
    const test = liveStatus({
      feed: "test",
      source_label: "TEST DATA",
      test_data: true,
      desired_symbols: ["FAKEPACA"],
    });
    model.apply(snapshot(3, test, {}, [trade("FAKEPACA", 2_000_000, 0)], "stream-b"), 0);
    expect(model.watchlist).toEqual(["FAKEPACA"]);
    expect(model.series.has("AAPL")).toBe(false);
    expect(model.seriesOf("FAKEPACA").trade).toEqual([2]);
    expect(model.status?.feed.test_data).toBe(true);
  });
});

describe("what the screen may claim", () => {
  it("says the stream is disconnected and that values are not current", () => {
    const model = connected();
    model.connection = "closed";
    const banner = headline(model, 0);
    expect(banner.key).toBe("stream_disconnected");
    expect(banner.detail).toMatch(/last one received, not a current one/);
    expect(availabilityOf("AAPL", model, model.status, agesOf(undefined, 0))).toBe("offline");
  });

  it("separates a connected feed from an updating market", () => {
    const model = connected(
      liveStatus(
        {},
        {
          market: {
            activity: "quiet",
            last_observation_age_ms: 185_000,
            session: "closed_weekend",
          },
        },
      ),
    );
    const banner = headline(model, 0);
    expect(banner.key).toBe("market_closed");
    expect(banner.title).toBe("CONNECTED · NO MARKET UPDATES");
    expect(banner.detail).toMatch(/3 min 05 s ago/);
    expect(banner.detail).toMatch(/weekend/);
    const never = connected(
      liveStatus({}, { market: { activity: "none_yet", last_observation_age_ms: null } }),
    );
    expect(headline(never, 0).detail).toMatch(/Nothing has been observed on this connection/);
  });

  it("names a stopped, failed, reconnecting, and degraded feed", () => {
    const stopped = connected(
      liveStatus({ state: "STOPPED" }, { market: { activity: "offline" } }),
    );
    expect(headline(stopped, 0).key).toBe("feed_stopped");
    const failed = connected(
      liveStatus({
        state: "STOPPED",
        last_error: { code: "ProviderAuthentication", message: "auth failed", at: "" },
      }),
    );
    expect(headline(failed, 0).detail).toMatch(/ProviderAuthentication: auth failed/);
    expect(headline(failed, 0).tone).toBe("negative");
    const retrying = connected(
      liveStatus({ state: "RECONNECTING", attempt: 3, next_retry_in_ms: 4_000 }),
    );
    expect(headline(retrying, 0).detail).toMatch(/attempt 3, next try in 4.0 s/);
    const degraded = connected(
      liveStatus({
        state: "DEGRADED",
        impairments: [{ code: "ProviderSymbolLimit", message: "symbol limit exceeded" }],
      }),
    );
    expect(headline(degraded, 0)).toMatchObject({
      key: "degraded",
      detail: "symbol limit exceeded",
    });
    expect(headline(connected(), 0).key).toBe("receiving");
  });

  it("lets ages keep counting between hub messages and marks stale observations", () => {
    const model = connected();
    const q = quote("AAPL", 10_000_000, 10_020_000, 0);
    const t = trade("AAPL", 10_010_000, 0);
    const state = symbolState("AAPL", t, q, 2_000_000_000);
    expect(agesOf(state, 0).quote).toBe(2_000);
    const later = agesOf(state, 20_000);
    expect(later.quote).toBe(22_000);
    expect(availabilityOf("AAPL", model, model.status, agesOf(state, 0))).toBe("updating");
    expect(availabilityOf("AAPL", model, model.status, later)).toBe("stale");
    expect(availabilityOf("AAPL", model, model.status, agesOf(undefined, 0))).toBe("awaiting");
    const keys = noticesOf(state, model.status, later).map((notice) => notice.key);
    expect(keys).toEqual(["quote_stale", "trade_stale"]);
    expect(noticesOf(state, model.status, agesOf(state, 0))).toEqual([]);
  });

  it("discloses a quote and a trade that are from different moments", () => {
    const model = connected();
    const state = symbolState(
      "AAPL",
      trade("AAPL", 10_010_000, 0),
      quote("AAPL", 10_000_000, null, 0),
      100_000_000,
      { quote_age_ns: 40_000_000_000, skew_alert: true },
    );
    const keys = noticesOf(state, model.status, agesOf(state, 0)).map((notice) => notice.key);
    expect(keys).toEqual(["quote_stale", "skew", "one_sided"]);
    const unsubscribed = connected(
      liveStatus({ subscribed: { trades: ["NVDA"], quotes: ["NVDA"] } }),
    );
    expect(availabilityOf("AAPL", unsubscribed, unsubscribed.status, agesOf(state, 0))).toBe(
      "unsubscribed",
    );
  });

  it("applies a status heartbeat to every instrument's freshness", () => {
    const model = connected();
    const state = symbolState("AAPL", null, quote("AAPL", 1_000_000, 1_020_000, 0), 5e9);
    model.apply(statusMessage(11, liveStatus({ reconnects: 2 }), { AAPL: state }), 700);
    expect(model.status?.feed.reconnects).toBe(2);
    expect(model.refreshedAt).toBe(700);
    expect(model.stateAt.get("AAPL")).toBe(700);
  });
});

class FakeSocket {
  static instances: FakeSocket[] = [];
  readyState = 0;
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(readonly url: string) {
    FakeSocket.instances.push(this);
  }
  send(data: string) {
    this.sent.push(data);
  }
  close() {
    this.readyState = 3;
    this.onclose?.();
  }
  open() {
    this.readyState = 1;
    this.onopen?.();
  }
  deliver(message: unknown) {
    this.onmessage?.({ data: typeof message === "string" ? message : JSON.stringify(message) });
  }
}

describe("stream client", () => {
  beforeEach(() => {
    FakeSocket.instances = [];
    vi.useFakeTimers();
  });
  afterEach(() => vi.useRealTimers());

  function client(onMessage: (message: LiveMessage) => { resync: boolean }) {
    const states: string[] = [];
    const instance = new LiveClient({
      url: "ws://terminal/api/v1/live/stream",
      onMessage,
      onConnection: (state, reconnects) => states.push(`${state}:${reconnects}`),
      socket: (url) => new FakeSocket(url) as unknown as WebSocket,
      baseDelayMs: 100,
      maxDelayMs: 400,
    });
    return { instance, states };
  }

  it("derives the stream address from the page", () => {
    expect(liveStreamUrl({ protocol: "http:", host: "127.0.0.1:5173" })).toBe(
      "ws://127.0.0.1:5173/api/v1/live/stream",
    );
    expect(liveStreamUrl({ protocol: "https:", host: "desk.example" })).toBe(
      "wss://desk.example/api/v1/live/stream",
    );
  });

  it("reconnects with bounded backoff and reports every connection state", () => {
    const { instance, states } = client(() => ({ resync: false }));
    instance.start();
    FakeSocket.instances[0]?.open();
    FakeSocket.instances[0]?.close();
    expect(states).toEqual(["connecting:0", "open:0", "closed:0"]);
    vi.advanceTimersByTime(49);
    expect(FakeSocket.instances).toHaveLength(1);
    vi.advanceTimersByTime(60);
    expect(FakeSocket.instances).toHaveLength(2);
    // Repeated failures back off, and never past the ceiling.
    for (let round = 0; round < 6; round += 1) {
      FakeSocket.instances.at(-1)?.close();
      vi.advanceTimersByTime(400);
    }
    expect(FakeSocket.instances).toHaveLength(8);
    FakeSocket.instances.at(-1)?.open();
    expect(states.at(-1)).toBe("open:7");
    instance.stop();
    vi.advanceTimersByTime(5_000);
    expect(FakeSocket.instances).toHaveLength(8);
  });

  it("asks for a resync when the model reports a gap and ignores other protocol versions", () => {
    const seen: LiveMessage[] = [];
    const { instance } = client((message) => {
      seen.push(message);
      return { resync: message.type === "events" };
    });
    instance.start();
    const socket = FakeSocket.instances[0];
    socket?.open();
    socket?.deliver(events(5, []));
    expect(socket?.sent).toEqual(['{"type":"resync"}']);
    socket?.deliver({ ...events(6, []), v: 2 });
    socket?.deliver("not json");
    expect(seen).toHaveLength(1);
    instance.stop();
  });
});
