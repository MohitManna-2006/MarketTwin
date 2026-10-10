# ADR 0001: Phase 1A exchange semantics

Date: 2026-10-10. Status: accepted.

Phase 1A implements architecture sections 5–6 and the pure exchange acceptance
cases. It does not implement a scheduler, persisted trace, ledger, agents, or a
market-running CLI.

## Decisions

- Prices, quantities, IDs, logical timestamps, and sequences are distinct explicit
  integer wrappers. There are no implicit conversions or unchecked arithmetic
  operators. Quantity addition/subtraction use checked helpers. Prices are signed
  64-bit ticks; quantities and identities use unsigned 64-bit values. No money
  multiplication is needed until the accounting phase.
- One `MatchingEngine` owns one symbol and an immutable allowlist of nonzero agent
  IDs. Zero IDs are invalid. Accepted order IDs cannot be reused for the lifetime
  of the engine, including filled and canceled orders. Rejections do not reserve IDs.
- Callers provide exchange arrival time and sequence, eventually assigned by the
  kernel. Accepted submissions must have nondecreasing time and strictly increasing
  nonzero sequence. Rejections do not advance this watermark. Cancels are synchronous;
  calling order decides cancel/fill races. Phase 1B will order all commands centrally.
- Self-trade prevention rejects the entire incoming order if its actual matching
  path would reach its owner's resting order. A preflight walks only executable
  liquidity, stopping at the incoming quantity or price limit. It rejects before
  any fills, ID allocation, or book mutation. Own orders beyond that path are harmless.
- Limit orders accept GTC or IOC. Market orders must use IOC and a zero unused
  limit-price field. Limit prices must be positive. New remaining quantity must
  equal original quantity. The `Order` default TIF is GTC; market callers explicitly
  choose IOC. No implicit normalization hides malformed inputs.
- Configurable limits bound order size, active orders, lifetime accepted orders,
  lifetime trades, and snapshot depth. Defaults are one million shares per order,
  100,000 active orders, one million accepted orders/trades, and 1,000 snapshot levels.
  Active-order capacity is checked against the *post-match* book. Checked level
  aggregation and trade-ID capacity are validated before mutation. Overflow rejects
  the whole incoming order, even when earlier fills would otherwise succeed.
- Trades have exchange-local consecutive IDs and execution sequences starting at
  one, plus the incoming timestamp and arrival sequence for causal context. This is
  not the future global event sequence. Report vectors are in matching order.
  Maker status updates follow each fill, followed by the final incoming status.
  Status quantities are cumulative filled, canceled, and resting amounts; accepted
  orders conserve their original quantity. Rejected orders have zero dispositions.
- Cancellation returns the canceled remainder and cumulative filled quantity.
  Wrong-owner requests fail without mutation. Unknown, filled, and already canceled
  orders return `OrderNotFound`; history is retained only for ID uniqueness.
- Snapshots are value copies, sorted best to worst, with quantity and order count
  per level. Zero depth or a depth above the configured cap raises a coded
  `ExchangeError`. Best quotes are optional. No fabricated midpoint is supplied.
- Maps contain FIFO lists; an unordered ID index contains stable list handles.
  No output depends on hash iteration. The book and engine cannot be copied or
  moved, preventing index handles from referring to another object's storage.
  Cancellation is expected O(1) index lookup/list removal plus O(log L) level lookup
  and possible removal. Submit preflight and execution scale with executable fills.
- Domain rejections are transactional. Allocation failures and invariant failures
  are fatal exceptions: abandon that engine/run, rather than catch and continue.
  This avoids copying the entire book per command or promising recovery from OOM.

## Verification and consequences

Unit tests cover both sides, price/FIFO ordering, partial fills, cancellation
positions and races, ownership, input validation, report conservation, bounds, and
arithmetic overflow. Randomized traces compare against a deliberately simple
vector-based reference matcher and repeat identical commands against a fresh
engine. Invariants are checked after every randomized operation under ASan/UBSan.
The accepted-ID set grows until the explicit lifetime order limit is reached.
There is no persistence or replay hash yet; Phase 1B owns those contracts.
