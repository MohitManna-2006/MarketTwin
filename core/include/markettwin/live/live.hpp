#ifndef MARKETTWIN_LIVE_LIVE_HPP
#define MARKETTWIN_LIVE_LIVE_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace markettwin::live {

inline constexpr std::uint32_t kLiveSchema = 1;

// Normalizes live provider trades and L1 quotes and keeps the last observed state of each
// instrument. Like historical observations, live observations never enter the synthetic
// exchange or its clock.
//
// It uses the Phase 4A conversions: prices are decimal strings checked into signed 64-bit
// millionths of a US dollar, and timestamps are RFC3339 UTC text checked into nanoseconds
// with the source's fractional-digit count kept beside them. No floating point is used.
//
// Three clocks stay separate in every record and are never mixed:
//   * the source event timestamp, as the provider reported it;
//   * the gateway receipt time, as a wall-clock value and a monotonic value, both
//     supplied by the caller;
//   * the native processing time of a batch, measured here on a steady clock.
// Ages are differences of the caller's monotonic values. The one cross-domain figure,
// source_age_ns, is labelled as such: it includes unknown clock offset and transit time.
//
// Ordering: the provider gives no sequence number and no ordering guarantee, so arrival
// order is not treated as exchange order. Per instrument and kind, a record whose source
// timestamp is older than the newest one already seen is classified out_of_order and does
// not replace state; an exact repeat inside a bounded window is classified duplicate.
// Both are still emitted, flagged, and counted.
//
// A trade never changes quote state and a quote never changes trade state. An absent
// quote side is null, never zero. Quotes of different venues are not merged.
//
// One instance is one feed. Calls are serialized internally.
class LiveNormalizer {
public:
  // config_json: {"provider": string, "feed": string, "stale_after_ns": int > 0,
  // "skew_alert_ns": int > 0, "max_symbols": int in [1, 4096],
  // "duplicate_window": int in [0, 65536]}. The last four are optional.
  explicit LiveNormalizer(const std::string& config_json);
  ~LiveNormalizer();
  LiveNormalizer(const LiveNormalizer&) = delete;
  LiveNormalizer& operator=(const LiveNormalizer&) = delete;
  LiveNormalizer(LiveNormalizer&&) noexcept;
  LiveNormalizer& operator=(LiveNormalizer&&) noexcept;

  // One coarse call per batch of provider frames, in arrival order:
  //   {"now_wall_ns": "<utc ns>", "now_monotonic_ns": int,
  //    "frames": [{"frame": int, "receipt_wall_ns": "<utc ns>",
  //                "receipt_monotonic_ns": int, "messages": [<provider message>...]}]}
  // A provider message is the provider's object with prices as decimal strings.
  // Returns {"schema_version", "events", "rejects", "unsupported", "states"} as JSON text.
  // "states" holds only the instruments this batch touched. The result is a pure function
  // of the configuration and of every batch given so far. A malformed batch throws
  // RunError InvalidLiveBatch and changes nothing; a bad message becomes a reject.
  std::string process_batch(const std::string& batch_json);

  // State and freshness of every instrument, and the normalizer's counters, as of the
  // caller's clocks.
  [[nodiscard]] std::string snapshot(std::int64_t now_wall_ns, std::int64_t now_monotonic_ns) const;

  // Forgets every instrument not named in this JSON array of symbols.
  void retain_symbols(const std::string& symbols_json);

  // Steady-clock duration of the last process_batch call. Kept out of its result so the
  // result stays deterministic.
  [[nodiscard]] std::int64_t last_batch_elapsed_ns() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace markettwin::live

#endif
