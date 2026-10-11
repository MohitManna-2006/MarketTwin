# ADR 0005: Phase 3A bindings, research API, and the research terminal

Date: 2026-10-11. Status: accepted.

## Scope

Implement architecture sections 12.3, 15, and 17: a pybind11 binding at the run boundary,
a local HTTP API with background jobs, and a browser terminal that reads what the engine
recorded. This ADR also settles the decisions the architecture lists as ADR-0008 (pybind11
or the CLI) and ADR-0009 (worker queue and result store). The AI research assistant,
WebSocket progress, authentication, and hosted deployment remain later work. No exchange,
agent, accounting, or statistics behaviour changes.

## Versions and compatibility

- Engine version stays 0.4.0. No stream, manifest, or experiment schema changes, and every
  golden digest is unchanged.
- One function is added to the core: `experiment_paired_values_json(config, result)`. It
  returns, for each compared metric, every paired seed's baseline value, candidate value,
  and difference, from the same extractors and the same pairing the comparison already
  uses. It is not written to `experiment.json`. A test summarizes its output and requires
  every native summary back, exactly.
- The API is versioned in its path (`/api/v1`). Its OpenAPI document is committed with the
  terminal, and the terminal's types are generated from it.

## Binding

- `markettwin._native` is built by CMake with `-DMARKETTWIN_BUILD_PYTHON=ON` (default
  off, so the engine's presets and CI jobs are unaffected) and packaged by
  `python/pyproject.toml` with scikit-build-core. pybind11 3.0.1 is pinned by URL and
  SHA-256 like the other dependencies.
- **pybind11, not a CLI subprocess.** The service needs a stable error code and sequence
  from a failed run, run counts from an experiment while it is in progress, and
  cooperative cancellation. A subprocess would need its stderr parsed and a side channel
  for progress. The CLI remains the reference: a test holds a run through the binding to
  the same golden summary and metrics the CLI tests pin.
- Every function is one complete native operation. JSON text crosses the boundary in both
  directions: `canonicalize_config`, `run`, `verify_replay`, `canonicalize_experiment`,
  `experiment_cell_config`, `run_experiment`, `version`, `build_info`. No Python object is
  created per event, trade, or level, and Python is never called during a run.
- The GIL is released for the whole of every native call. The core has no global mutable
  state, so independent runs execute concurrently on threads; a test runs six at once and
  requires the digests of the same six run in sequence.
- `markettwin::RunError` becomes `markettwin.NativeRunError` with `code` and `sequence`.
- Experiment progress uses the runner's existing `RunFunction` hook: a C++ callable that
  runs the cell and updates atomic counters Python may read from another thread. A cancel
  request makes each remaining cell fail with code `Canceled`; the experiment still
  finishes and publishes, with those cells counted as failed.

## Service

- **SQLite and an in-process worker, not PostgreSQL and Redis.** Architecture section 20
  named PostgreSQL metadata and an optional Redis queue for this phase; section 15.3
  allows a local process with SQLite. The service is single-user and local, the artifacts
  are files the engine already writes atomically, and what remains is a small registry: who
  asked for a run, when, how long it took, and what replay last said. That is one SQLite
  file behind a `Registry` class. Jobs run on a bounded thread pool in the API process
  (two at a time, sixteen waiting, then `429`), which is what lets a status request read a
  running experiment's counters directly. A job belongs to the process that started it:
  after a restart, anything still marked running is recorded as failed with
  `WorkerInterrupted`. PostgreSQL and a queue are deferred until there is more than one
  process to coordinate.
- **Trust boundary.** A request carries IDs, never paths. Run and experiment directories
  are built from IDs the registry issued; the only downloadable files are the six
  artifacts of a run, by name. An experiment's `base_config` must be embedded: the CLI
  accepts a relative path there and the API refuses one. Request bodies, horizon, event
  and byte budgets, experiment size, and sample count are capped. The service binds the
  loopback interface and has no authentication.
- **Validation in three layers, reported separately.** A structural model of the config
  schema gives field paths; the service's own budgets come next; the engine's validation
  is last and is the authority. The engine's message is passed through verbatim and has no
  field path, because the engine does not report one.
- **Integers a browser can hold.** JSON numbers above 2^53 - 1 lose precision in a
  browser. The API therefore accepts seeds up to 2^53 - 1, agent IDs up to 2^20 - 1 (so
  agent order IDs stay exact), and cash up to 9e15 minor units, and refuses to index a run
  in which any recorded value exceeds 2^53 - 1 (`UnsupportedRange`). The CLI keeps the
  engine's full ranges.

## What the service derives, and how it is labelled

The engine's files are never modified. On first use the service makes one pass over a
run's three streams and writes a columnar copy to the workspace's `derived` directory.
Every number in it is one the engine recorded. Three series are computed from recorded
ones and are named as derived wherever they are served:

| Series | Definition | Authoritative figure |
| --- | --- | --- |
| `mid_twice_ticks` | The recorded mark where its recorded source is `Midpoint` | The mark itself |
| `drawdown_minor` | Running peak of sampled equity, starting at opening cash, minus equity | `max_drawdown_minor` in `metrics.json`; a test requires the series' maximum to equal it |
| `cum_bought`, `cum_sold` | Fill quantities of ledger entries booked before each sample, in accounting-sequence order | `bought_quantity`, `sold_quantity`; a test requires the final values to equal them |

The terminal computes plot coordinates, formats, histogram bins, and the difference
between two recorded integers shown side by side (midpoint minus fundamental, volume
between plotted samples). It computes no P&L, no drawdown figure, no mean, and no interval.

## Time

- All recorded time is elapsed simulation nanoseconds. No scenario defines a session
  clock, so no time of day is shown anywhere. Wall-clock submission time and processing
  time come from the registry and are labelled as such.
- A lookup at time T returns the latest record at or before T. Depth is never
  interpolated; the ladder names the snapshot it shows. Before the first sample there is
  nothing to show, and the service says so.
- The playback cursor is an index into the recorded samples. Its speed is a viewing pace
  (1x is five samples per second of wall-clock time), not a property of the run.
- A series longer than `max_points` (20,000 by default) is decimated: each bucket keeps
  its first and last sample and the samples where the primary series is lowest and
  highest, and samples around every shock transition are kept. Every plotted point is a
  recorded sample; the response says the series is decimated and lists the sample index
  of each point. The cursor, the ladder, and the readouts always use exact samples.
- Sampled series are drawn as steps (last observation carried forward).

## Experiments in the terminal

- Statistics come from `experiment.json`; per-seed values from the paired-values document.
- An experiment that kept no run directories has no time series. Any cell can be
  **materialized**: the service asks the core for that cell's exact canonical config
  (`variant_config`), runs it as an ordinary run, and compares its digests with the ones
  the experiment recorded. The terminal shows whether they match. This is what the
  per-seed side-by-side view rests on.
- The side-by-side view links two runs by elapsed time and states that their market
  histories differ.

## Terminal

- React, TypeScript in strict mode, Vite, Tailwind for tokens and utilities, TanStack
  Query, Table, and Virtual, Zustand for the playback cursor only, Radix primitives without
  their styling. One charting library, uPlot, for every time series; statistical plots
  (interval, histogram, paired scatter) are small SVG components.
- Design tokens are CSS custom properties in `apps/web/src/styles/tokens.css`. A script
  checks every text token against every surface for WCAG 2.1 contrast, and an automated
  axe audit runs over every screen in the end-to-end suite.
- When the API does not answer, the terminal says so. It has no cached or demonstration
  data to fall back to.

## Layout

`bindings/python/` holds the extension source, `python/` the `markettwin` package,
`services/api/` the service (`markettwin_api`, not the `app` package sketched in
architecture section 4), and `apps/web/` the terminal. Run and experiment output goes to
`data/research/` by default (`MARKETTWIN_WORKSPACE`), which is untracked.

## Limits and what is not implemented

- A single run reports no progress and cannot be canceled: the core runs it as one call.
  Only an experiment reports counts, and only of finished runs.
- VWAP, return volatility, time in each book state, fill rates, and spread capture exist
  only as run totals. The terminal shows them as run totals, never as of a time.
- Depth exists only at sampling times and to the sampled depth.
- At most 500,000 samples per run are indexed.
- Progress is polled; the WebSocket of architecture 15.2 is not implemented.
- Runs made outside the workspace, for example by the CLI, are not imported.
- The registry and the derived index assume one API process per workspace.

## Verification

Native: one new test for the paired values; the existing suite and goldens unchanged.
Python: binding tests for golden parity, error translation, concurrency, GIL release,
progress, cancellation, and cell reproducibility; service tests against the real engine
for runs, every telemetry endpoint and its as-of semantics, decimation, the derived series
against their native totals, replay including three kinds of tampering, experiments whose
results must equal a direct native run, cell materialization, bounded queueing, recovery
after an interrupted job, and requests that try to leave the registry. Terminal: unit
tests for formatting, as-of lookup, the playback cursor, the table, the ladder, the
interval plot against the engine's golden numbers, and failure states; an end-to-end suite
that drives a browser through a real run, compares what each panel shows at a time with
what the API recorded there, verifies replay, runs an experiment, regenerates a seed pair,
audits accessibility, and cuts the API off.
