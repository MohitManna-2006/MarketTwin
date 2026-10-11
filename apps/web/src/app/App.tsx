import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useState } from "react";
import { RouterProvider } from "react-router";

import { TooltipProvider } from "@/components/ui/Tooltip";
import { ApiError } from "@/lib/api/client";

import { createRouter } from "./router";

function createQueryClient(): QueryClient {
  return new QueryClient({
    defaultOptions: {
      queries: {
        // An answer from the API is final for its request; only a silent API is retried.
        retry: (failures, error) => !(error instanceof ApiError) && failures < 1,
        refetchOnWindowFocus: false,
        staleTime: 2_000,
      },
    },
  });
}

export function App() {
  const [client] = useState(createQueryClient);
  const [router] = useState(createRouter);
  return (
    <QueryClientProvider client={client}>
      <TooltipProvider delayDuration={120} skipDelayDuration={300}>
        <RouterProvider router={router} />
      </TooltipProvider>
    </QueryClientProvider>
  );
}
