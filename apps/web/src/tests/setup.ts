import "@testing-library/jest-dom/vitest";

import { cleanup } from "@testing-library/react";
import { afterEach } from "vitest";

// jsdom has no layout. Components that measure themselves are told they have room.
class ResizeObserverStub implements ResizeObserver {
  constructor(private readonly callback: ResizeObserverCallback) {}
  observe(target: Element) {
    this.callback(
      [
        {
          target,
          contentRect: { width: 720, height: 240 } as DOMRectReadOnly,
        } as ResizeObserverEntry,
      ],
      this,
    );
  }
  unobserve() {}
  disconnect() {}
}
globalThis.ResizeObserver = ResizeObserverStub;

Object.defineProperty(HTMLElement.prototype, "offsetHeight", {
  configurable: true,
  get: () => 480,
});
Object.defineProperty(HTMLElement.prototype, "offsetWidth", { configurable: true, get: () => 960 });
Element.prototype.scrollTo = () => {};

afterEach(() => cleanup());
