import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { createMemoryRouter, RouterProvider } from "react-router";

import { HistoricalLibraryPage } from "./HistoricalLibraryPage";

afterEach(() => vi.unstubAllGlobals());

it("labels offline data and keeps historical observations outside simulations", async () => {
  vi.stubGlobal(
    "fetch",
    vi.fn((input: string) => {
      const body = input.includes("/providers")
        ? {
            providers: [
              {
                id: "local_file",
                configured: true,
                feeds: ["fabricated_l1"],
                coverage: "fabricated offline fixture",
                cost: "none",
              },
              {
                id: "alpaca",
                configured: false,
                feeds: ["iex", "sip"],
                coverage: "historical equities",
                cost: "check entitlement",
              },
            ],
          }
        : { imports: [], count: 0 };
      return Promise.resolve(
        new Response(JSON.stringify(body), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      );
    }),
  );
  const router = createMemoryRouter([{ path: "/historical", element: <HistoricalLibraryPage /> }], {
    initialEntries: ["/historical"],
  });
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  render(
    <QueryClientProvider client={client}>
      <RouterProvider router={router} />
    </QueryClientProvider>,
  );

  expect(await screen.findByText("Local fabricated fixture")).toBeInTheDocument();
  expect(screen.getByText(/Imports never enter the synthetic exchange/)).toBeInTheDocument();
  await waitFor(() =>
    expect(screen.getByRole("button", { name: "Import observations" })).toBeEnabled(),
  );
});

it("keeps a wall-clock time out of the UTC import range", async () => {
  vi.stubGlobal(
    "fetch",
    vi.fn((input: string) => {
      const body = input.includes("/providers")
        ? {
            providers: [
              {
                id: "alpaca",
                configured: true,
                feeds: ["iex"],
                coverage: "historical equities",
                cost: "check your Alpaca subscription before approving an import",
              },
            ],
          }
        : { imports: [], count: 0 };
      return Promise.resolve(
        new Response(JSON.stringify(body), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      );
    }),
  );
  const router = createMemoryRouter([{ path: "/historical", element: <HistoricalLibraryPage /> }], {
    initialEntries: ["/historical"],
  });
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  render(
    <QueryClientProvider client={client}>
      <RouterProvider router={router} />
    </QueryClientProvider>,
  );

  fireEvent.change(await screen.findByLabelText("Provider"), { target: { value: "alpaca" } });
  fireEvent.change(screen.getByLabelText("Start (UTC RFC3339)"), {
    target: { value: "9:30 AM ET" },
  });
  fireEvent.change(screen.getByLabelText("End (UTC RFC3339)"), {
    target: { value: "9:35 AM ET" },
  });
  expect(screen.getAllByText("Use a UTC instant, such as 2026-10-09T13:30:00Z")).toHaveLength(2);
  expect(screen.getByRole("button", { name: "Import observations" })).toBeDisabled();
  expect(
    screen.getByText("I confirmed feed entitlement and possible provider charges"),
  ).toBeVisible();
});
