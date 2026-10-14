/// <reference types="vitest/config" />
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const api = process.env.MARKETTWIN_API ?? "http://127.0.0.1:8000";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: { "@": fileURLToPath(new URL("./src", import.meta.url)) },
  },
  server: {
    host: "127.0.0.1",
    port: Number(process.env.MARKETTWIN_WEB_PORT ?? 5173),
    strictPort: true,
    // The terminal talks to the research API on the same origin; nothing else is proxied.
    // `ws` carries the live market-data stream through the same proxy.
    proxy: { "/api": { target: api, changeOrigin: false, ws: true } },
  },
  preview: {
    host: "127.0.0.1",
    port: 4173,
    proxy: { "/api": { target: api, changeOrigin: false, ws: true } },
  },
  test: {
    globals: true,
    environment: "jsdom",
    setupFiles: ["./src/tests/setup.ts"],
    include: ["src/**/*.test.{ts,tsx}"],
    css: false,
  },
});
