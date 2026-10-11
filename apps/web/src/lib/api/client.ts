// The only place the terminal talks to the research API.
//
// A failed request is always surfaced as one of two typed errors. There is no cached
// sample data and no demonstration mode: when the API cannot be reached, screens say so.

const BASE = "/api/v1";

export interface ApiFieldError {
  source?: "schema" | "api" | "engine";
  code?: string;
  path: string;
  message: string;
}

/** The API answered with an error. `code` is the service's or the engine's stable code. */
export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly details: unknown;

  constructor(status: number, code: string, message: string, details: unknown) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.code = code;
    this.details = details;
  }

  get fieldErrors(): ApiFieldError[] {
    return Array.isArray(this.details) ? (this.details as ApiFieldError[]) : [];
  }
}

/** The API did not answer at all. */
export class BackendUnavailableError extends Error {
  constructor(cause: unknown) {
    super("The research API did not respond.");
    this.name = "BackendUnavailableError";
    this.cause = cause;
  }
}

type Query = Record<string, string | number | boolean | readonly string[] | null | undefined>;

function url(path: string, query?: Query): string {
  if (!query) return BASE + path;
  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(query)) {
    if (value === null || value === undefined) continue;
    if (Array.isArray(value)) {
      for (const item of value) search.append(key, String(item));
    } else {
      search.set(key, String(value));
    }
  }
  const text = search.toString();
  return text ? `${BASE}${path}?${text}` : BASE + path;
}

async function request<T>(path: string, init: RequestInit, query?: Query): Promise<T> {
  let response: Response;
  try {
    response = await fetch(url(path, query), init);
  } catch (cause) {
    if (cause instanceof DOMException && cause.name === "AbortError") throw cause;
    throw new BackendUnavailableError(cause);
  }
  // The dev proxy answers 502/504 itself when nothing is listening behind it.
  if (response.status === 502 || response.status === 503 || response.status === 504) {
    throw new BackendUnavailableError(new Error(`HTTP ${response.status}`));
  }
  if (response.status === 204) return undefined as T;
  let body: unknown;
  try {
    body = await response.json();
  } catch (cause) {
    if (!response.ok) throw new BackendUnavailableError(cause);
    throw new ApiError(
      response.status,
      "InvalidResponse",
      "The API returned a non-JSON body.",
      null,
    );
  }
  if (!response.ok) {
    const error = (body as { error?: { code?: string; message?: string; details?: unknown } })
      .error;
    throw new ApiError(
      response.status,
      error?.code ?? "HttpError",
      error?.message ?? `HTTP ${response.status}`,
      error?.details ?? null,
    );
  }
  return body as T;
}

export function get<T>(path: string, query?: Query, signal?: AbortSignal): Promise<T> {
  return request<T>(path, { method: "GET", signal: signal ?? null }, query);
}

export function post<T>(path: string, body?: unknown): Promise<T> {
  const payload = JSON.stringify(body ?? {});
  return request<T>(path, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: payload,
  });
}

export function del(path: string): Promise<void> {
  return request<undefined>(path, { method: "DELETE" });
}

export function artifactUrl(runId: string, name: string): string {
  return `${BASE}/runs/${encodeURIComponent(runId)}/artifacts/${encodeURIComponent(name)}`;
}

export function describeError(error: unknown): { title: string; detail: string; code: string } {
  if (error instanceof BackendUnavailableError) {
    return {
      code: "BackendUnavailable",
      title: "RESEARCH API UNREACHABLE",
      detail:
        "The terminal could not reach the API at /api/v1. Start it with `uv run python -m markettwin_api` in services/api.",
    };
  }
  if (error instanceof ApiError) {
    return { code: error.code, title: error.code, detail: error.message };
  }
  return {
    code: "UnexpectedError",
    title: "UNEXPECTED ERROR",
    detail: error instanceof Error ? error.message : String(error),
  };
}
