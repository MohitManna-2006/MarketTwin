// WCAG 2.1 contrast audit of the design tokens.
//
// Reads src/styles/tokens.css and checks every text and data colour against every
// surface it may sit on. Body-size text needs 4.5:1; disabled text is exempt from the
// criterion and is held to 3:1 here. Exits non-zero if any pair fails.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const css = readFileSync(
  fileURLToPath(new URL("../src/styles/tokens.css", import.meta.url)),
  "utf8",
);
const tokens = Object.fromEntries(
  [...css.matchAll(/--([a-z0-9-]+):\s*(#[0-9a-fA-F]{6})\s*;/g)].map((m) => [m[1], m[2]]),
);

function luminance(hex) {
  const channel = (index) => {
    const value = parseInt(hex.slice(1 + index * 2, 3 + index * 2), 16) / 255;
    return value <= 0.03928 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(0) + 0.7152 * channel(1) + 0.0722 * channel(2);
}

function contrast(foreground, background) {
  const [light, dark] = [luminance(foreground), luminance(background)].sort((a, b) => b - a);
  return (light + 0.05) / (dark + 0.05);
}

const surfaces = [
  "canvas",
  "surface-0",
  "surface-1",
  "surface-2",
  "surface-hover",
  "surface-selected",
];
const text = {
  "text-primary": 4.5,
  "text-secondary": 4.5,
  "text-tertiary": 4.5,
  "text-disabled": 3,
  "accent-primary": 4.5,
  "accent-hover": 4.5,
  "accent-label": 4.5,
  "data-positive-text": 4.5,
  "data-negative-text": 4.5,
  "data-neutral": 4.5,
  "data-warning": 4.5,
  "chart-axis": 4.5,
  "focus-ring": 3,
};

let failures = 0;
const width = Math.max(...Object.keys(text).map((name) => name.length));
console.log(`${"token".padEnd(width)}  ${surfaces.map((s) => s.padStart(16)).join("")}   min`);
for (const [name, required] of Object.entries(text)) {
  const cells = surfaces.map((surface) => {
    const ratio = contrast(tokens[name], tokens[surface]);
    const ok = ratio >= required;
    if (!ok) failures += 1;
    return `${ratio.toFixed(2)}${ok ? " " : "!"}`.padStart(16);
  });
  console.log(`${name.padEnd(width)}  ${cells.join("")}   ${required}:1`);
}
// Non-text: a border that separates panels should be visible against either neighbour.
for (const border of ["border-default", "border-strong"]) {
  const ratio = contrast(tokens[border], tokens.canvas);
  console.log(
    `${border.padEnd(width)}  ${ratio.toFixed(2).padStart(16)}   (non-text, informative)`,
  );
}
if (failures > 0) {
  console.error(`\n${failures} pair(s) below the required ratio.`);
  process.exit(1);
}
console.log("\nEvery text token meets its ratio on every surface.");
