import type { StatusSpec } from "@/components/ui/StatusIndicator";

import { formatAge } from "./format";
import type { LiveModel } from "./liveModel";
import type { FeedState, LiveStatus, SymbolState } from "./protocol";

// What the screen is allowed to claim, derived in one place.
//
// Two facts are kept apart everywhere: whether the transport to the provider is up, and
// whether observations are arriving. A connected feed with nothing arriving is never
// described as live or updating.

export const FEED_STATUS: Record<FeedState, StatusSpec> = {
  STOPPED: { label: "Feed stopped", tone: "muted", glyph: "ring" },
  CONNECTING: { label: "Connecting", tone: "active", glyph: "half", pulse: true },
  AUTHENTICATING: { label: "Authenticating", tone: "active", glyph: "half", pulse: true },
  SUBSCRIBING: { label: "Subscribing", tone: "active", glyph: "half", pulse: true },
  CONNECTED: { label: "Feed connected", tone: "positive", glyph: "dot" },
  RECONNECTING: { label: "Reconnecting", tone: "warning", glyph: "half", pulse: true },
  DEGRADED: { label: "Feed degraded", tone: "warning", glyph: "square" },
  STOPPING: { label: "Stopping", tone: "muted", glyph: "half" },
};

export const ACTIVITY_STATUS: Record<string, StatusSpec> = {
  offline: { label: "No market data", tone: "muted", glyph: "slash" },
  none_yet: { label: "No updates yet", tone: "neutral", glyph: "ring" },
  quiet: { label: "No market updates", tone: "warning", glyph: "ring" },
  active: { label: "Updates arriving", tone: "positive", glyph: "dot" },
};

const SESSION_TEXT: Record<string, string> = {
  closed_weekend: "New York clock: weekend",
  closed_overnight: "New York clock: overnight, outside trading hours",
  pre_market: "New York clock: before the regular session",
  after_hours: "New York clock: after the regular session",
  regular_hours: "New York clock: regular session hours",
  not_applicable: "this feed does not follow market hours",
};

const SESSION_SHORT: Record<string, string> = {
  closed_weekend: "weekend (NY clock)",
  closed_overnight: "overnight (NY clock)",
  pre_market: "pre-market (NY clock)",
  after_hours: "after hours (NY clock)",
  regular_hours: "regular hours (NY clock)",
  not_applicable: "not applicable",
};

/** The same hint, short enough for a status row. */
export function sessionShort(session: string): string {
  return SESSION_SHORT[session] ?? session;
}

export function sessionText(session: string): string {
  return SESSION_TEXT[session] ?? session;
}

export type HeadlineTone = "positive" | "negative" | "warning" | "neutral" | "muted" | "active";

export interface Headline {
  key:
    | "stream_disconnected"
    | "waiting"
    | "feed_stopped"
    | "reconnecting"
    | "connecting"
    | "degraded"
    | "market_closed"
    | "receiving";
  tone: HeadlineTone;
  title: string;
  detail: string;
}

/** The single most important thing to know about what the terminal is showing. */
export function headline(model: LiveModel, elapsedMs: number): Headline {
  const status = model.status;
  if (model.connection !== "open") {
    return {
      key: status ? "stream_disconnected" : "waiting",
      tone: status ? "negative" : "muted",
      title: status ? "STREAM DISCONNECTED" : "CONNECTING TO THE LIVE HUB",
      detail: status
        ? "The terminal lost its connection to the API. Every value shown is the last one received, not a current one. Reconnecting."
        : "Waiting for the API's live stream.",
    };
  }
  if (!status) {
    return {
      key: "waiting",
      tone: "muted",
      title: "CONNECTING TO THE LIVE HUB",
      detail: "Waiting for the first snapshot.",
    };
  }
  const feed = status.feed;
  switch (feed.state) {
    case "STOPPED":
    case "STOPPING":
      return {
        key: "feed_stopped",
        tone: feed.last_error ? "negative" : "muted",
        title: "FEED STOPPED",
        detail: feed.last_error
          ? `${feed.last_error.code}: ${feed.last_error.message}. No market data is being received.`
          : "No upstream connection is open. Connect a feed to observe market data.",
      };
    case "RECONNECTING":
      return {
        key: "reconnecting",
        tone: "warning",
        title: "UPSTREAM RECONNECTING",
        detail: `The provider connection was lost (attempt ${feed.attempt}${
          feed.next_retry_in_ms === null ? "" : `, next try in ${formatAge(feed.next_retry_in_ms)}`
        }). Values shown are the last received.`,
      };
    case "CONNECTING":
    case "AUTHENTICATING":
    case "SUBSCRIBING":
      return {
        key: "connecting",
        tone: "active",
        title: FEED_STATUS[feed.state].label.toUpperCase(),
        detail: "The provider connection is being established. No market data yet.",
      };
    default:
      break;
  }
  const market = status.market;
  if (market.activity !== "active") {
    const age =
      market.last_observation_age_ms === null
        ? "Nothing has been observed on this connection"
        : `Last observation ${formatAge(market.last_observation_age_ms + elapsedMs)} ago`;
    return {
      key: "market_closed",
      tone: feed.state === "DEGRADED" ? "warning" : "neutral",
      title: "CONNECTED · NO MARKET UPDATES",
      detail: `${age}. ${sessionText(market.session)}. The connection is healthy; a feed delivers nothing while its market is closed or quiet.`,
    };
  }
  if (feed.state === "DEGRADED") {
    return {
      key: "degraded",
      tone: "warning",
      title: "FEED DEGRADED",
      detail:
        feed.impairments.map((item) => item.message).join("; ") ||
        "The provider connection is impaired.",
    };
  }
  return {
    key: "receiving",
    tone: "positive",
    title: "RECEIVING MARKET DATA",
    detail: `${feed.source_label} · ${market.session === "not_applicable" ? "continuous stream" : sessionText(market.session)}`,
  };
}

export interface Ages {
  update: number | null;
  trade: number | null;
  quote: number | null;
}

/** Ages now: the hub's monotonic ages plus the time since the hub last reported them. */
export function agesOf(state: SymbolState | undefined, elapsedMs: number): Ages {
  const at = (ns: number | null | undefined) =>
    ns === null || ns === undefined ? null : ns / 1e6 + Math.max(0, elapsedMs);
  return {
    update: at(state?.freshness.update_age_ns),
    trade: at(state?.freshness.trade_age_ns),
    quote: at(state?.freshness.quote_age_ns),
  };
}

export type Availability = "offline" | "unsubscribed" | "awaiting" | "updating" | "stale";

export const AVAILABILITY_STATUS: Record<Availability, StatusSpec> = {
  offline: { label: "No feed", tone: "muted", glyph: "slash" },
  unsubscribed: { label: "Not subscribed", tone: "warning", glyph: "square" },
  awaiting: { label: "Awaiting", tone: "muted", glyph: "ring" },
  updating: { label: "Updating", tone: "positive", glyph: "dot" },
  stale: { label: "Stale", tone: "warning", glyph: "half" },
};

export function availabilityOf(
  symbol: string,
  model: LiveModel,
  status: LiveStatus | null,
  ages: Ages,
): Availability {
  if (model.connection !== "open" || !status) return "offline";
  const feed = status.feed;
  if (feed.state !== "CONNECTED" && feed.state !== "DEGRADED") return "offline";
  if (!feed.subscribed.trades.includes(symbol) && !feed.subscribed.quotes.includes(symbol)) {
    return "unsubscribed";
  }
  if (ages.update === null) return "awaiting";
  return ages.update > status.market.stale_after_ms ? "stale" : "updating";
}

export interface Notice {
  key: string;
  text: string;
}

/** Qualifications of the selected instrument's numbers that must be shown beside them. */
export function noticesOf(
  state: SymbolState | undefined,
  status: LiveStatus | null,
  ages: Ages,
): Notice[] {
  if (!state || !status) return [];
  const limit = status.market.stale_after_ms;
  const notices: Notice[] = [];
  const quoteStale = ages.quote !== null && ages.quote > limit;
  const tradeStale = ages.trade !== null && ages.trade > limit;
  if (quoteStale) {
    notices.push({
      key: "quote_stale",
      text: `QUOTE STALE · last quote ${formatAge(ages.quote)} ago; spread and midpoint are from that quote`,
    });
  }
  if (tradeStale) {
    notices.push({ key: "trade_stale", text: `LAST TRADE ${formatAge(ages.trade)} OLD` });
  }
  const skew =
    ages.quote !== null && ages.trade !== null ? Math.abs(ages.quote - ages.trade) : null;
  if (state.freshness.skew_alert || (skew !== null && skew > limit)) {
    notices.push({
      key: "skew",
      text: "TRADE AND QUOTE ARE FROM DIFFERENT MOMENTS · do not read the last trade against this quote",
    });
  }
  if (state.quote_state === "locked" || state.quote_state === "crossed") {
    notices.push({
      key: "quote_state",
      text: `QUOTE ${state.quote_state.toUpperCase()} · as reported by the venue`,
    });
  }
  if (state.quote && (state.quote_state === "bid_only" || state.quote_state === "ask_only")) {
    notices.push({
      key: "one_sided",
      text: `${state.quote_state === "bid_only" ? "NO ASK" : "NO BID"} REPORTED · spread and midpoint unavailable`,
    });
  }
  return notices;
}
