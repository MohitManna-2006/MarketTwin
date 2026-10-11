import { afterEach, describe, expect, it, vi } from "vitest";

import { ApiError, BackendUnavailableError, describeError, get, post } from "./client";

function respond(status: number, body: unknown) {
  return vi.fn(() =>
    Promise.resolve(
      new Response(JSON.stringify(body), {
        status,
        headers: { "content-type": "application/json" },
      }),
    ),
  );
}

afterEach(() => vi.unstubAllGlobals());

describe("API client", () => {
  it("returns the body of a successful response", async () => {
    vi.stubGlobal("fetch", respond(200, { status: "ok" }));
    await expect(get("/health")).resolves.toEqual({ status: "ok" });
    expect(fetch).toHaveBeenCalledWith(
      "/api/v1/health",
      expect.objectContaining({ method: "GET" }),
    );
  });

  it("repeats array parameters and drops absent ones", async () => {
    vi.stubGlobal("fetch", respond(200, {}));
    await get("/runs/r/events", { kind: ["Trade", "OrderStatus"], agent_id: undefined, limit: 5 });
    expect(fetch).toHaveBeenCalledWith(
      "/api/v1/runs/r/events?kind=Trade&kind=OrderStatus&limit=5",
      expect.anything(),
    );
  });

  it("raises the service's code, message, and field errors", async () => {
    vi.stubGlobal(
      "fetch",
      respond(422, {
        error: {
          code: "InvalidConfig",
          message: "the run config is not valid",
          details: [
            {
              source: "engine",
              path: "",
              message: "maker levels must fit twice within max_open_orders",
            },
          ],
        },
      }),
    );
    const error = await post("/runs", { config: {} }).catch((cause: unknown) => cause);
    expect(error).toBeInstanceOf(ApiError);
    expect(error).toMatchObject({ status: 422, code: "InvalidConfig" });
    expect((error as ApiError).fieldErrors[0]?.message).toBe(
      "maker levels must fit twice within max_open_orders",
    );
  });

  it("reports a silent API as unavailable, never as an empty result", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(() => Promise.reject(new TypeError("Failed to fetch"))),
    );
    await expect(get("/runs")).rejects.toBeInstanceOf(BackendUnavailableError);
    // The development proxy answers 502 itself when nothing is listening behind it.
    vi.stubGlobal(
      "fetch",
      vi.fn(() => Promise.resolve(new Response("", { status: 502 }))),
    );
    await expect(get("/runs")).rejects.toBeInstanceOf(BackendUnavailableError);
  });

  it("describes each failure for the reader", () => {
    expect(describeError(new BackendUnavailableError(null)).title).toBe("RESEARCH API UNREACHABLE");
    expect(
      describeError(new ApiError(404, "RunNotFound", "no run has the ID x", null)),
    ).toMatchObject({
      title: "RunNotFound",
      detail: "no run has the ID x",
    });
  });
});
