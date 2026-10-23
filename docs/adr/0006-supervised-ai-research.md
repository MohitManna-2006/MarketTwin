# ADR 0006: Supervised AI research workflow

**Date:** 2026-10-11
**Status:** Accepted for Phase 3B

## Context

Phase 3A already has native experiment validation, a bounded local worker pool, SQLite metadata, native paired statistics, progress, materialized run cells, and replay verification. Phase 3B needs durable natural-language planning and evidence-based interpretation without altering the C++ simulator or adding a queue service.

## Decision

The model-facing plan schema lists the native experiment's fixed metric identifiers. The backend checks them again before it builds the experiment configuration. An unsupported metric triggers at most one model correction; the correction may select a supported exposure proxy or request clarification, but may not change the experiment design. This keeps natural-language analysis topics separate from executable comparison metrics and preserves the native metric catalogue as the source of truth.

Use one typed Python workflow in the FastAPI process. SQLite stores investigations, conversation messages, and observable tool events in the existing registry. A model provider proposes a constrained `Proposal`; the backend independently chooses only a supported scenario and maker policy pair, builds a versioned experiment configuration, and calls the existing native validator. A plan's SHA-256 digest binds human approval to the exact validated plan. The `AWAITING_APPROVAL` to `APPROVED` transition is a SQLite compare-and-swap; only that transition may submit the experiment. Repeated approval returns the existing investigation. No model tool call can approve or execute.

A typed read-only tool registry mediates scenario discovery, native validation, experiment evidence, run and agent metrics, paired seeds, and replay status. It validates argument schemas, enforces investigation ownership and result limits, and records safe tool events. The model cannot call an execution tool; the approval transition is the only submission path.

The provider adapter uses OpenAI-compatible Chat Completions JSON mode with backend-only credentials, a request timeout, bounded retries, and usage capture. The adapter is an interface, so a Claude adapter can be added without changing the workflow. Model interpretation cannot introduce digits into report or follow-up prose. Every numeric report claim is assembled from native results and compared again against the authoritative experiment document before publication. Evidence references name the experiment and metric. Paired seed evidence is read from the native paired-values artifact. The frontend displays these records and links to the existing Experiment Lab.

The workflow states are `CREATED`, `PLANNING`, `AWAITING_APPROVAL`, `APPROVED`, `EXECUTING`, `ANALYZING`, `REPORT_READY`, plus terminal `REJECTED`, `CANCELLED`, `FAILED`, `PARTIAL`, and `UNSUPPORTED`. At process startup, incomplete native jobs are failed by the existing registry; investigations reflect interrupted jobs. Completed experiments awaiting analysis can be finished after restart. The local process is still a single-user service without authentication, as in Phase 3A.

## Scope and limits

Phase 3B initially executes only a baseline maker versus inventory-aware maker comparison within one catalogued scenario. A liquidity-withdrawal scenario can be the base scenario. A direct shocked-versus-unshocked paired comparison is not supported by the current native experiment schema and is rejected instead of simulated by an invalid proxy. (Superseded for a scenario's own single shock by [ADR 0013](0013-ai-shock-impact-research.md), once experiment schema 2 could express the pair.) The model cannot name local paths, arbitrary policies, or executable code. The AI seed cap is lower than the native API cap. Full run traces are not sent to the model.

A separate distributed graph runtime, Redis, vector database, or C++ change would add operational complexity without a demonstrated need. This decision does not change `architecture.md` because it implements its bounded AI boundary within existing Phase 3A interfaces.

## Consequences

The API process must remain alive while a model request or native experiment is in progress. In-process monitoring records a clear interrupted state after a crash. The current OpenAI-compatible JSON mode is supported by many providers but requires a configured model that actually honors JSON responses. Live provider evaluations remain separate from CI, while deterministic mock-model tests run the actual native experiment engine.
