import { useEffect, useRef } from "react";

export type HotkeyMap = Record<string, (event: KeyboardEvent) => void>;

/** True while the reader is typing: shortcuts never take keys away from a form. */
export function isTypingTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  if (target.isContentEditable) return true;
  const tag = target.tagName;
  if (tag === "TEXTAREA" || tag === "SELECT") return true;
  if (tag !== "INPUT") return false;
  const type = (target as HTMLInputElement).type;
  return !["checkbox", "radio", "button", "range", "submit"].includes(type);
}

function chord(event: KeyboardEvent): string {
  const key = event.key === " " ? "Space" : event.key;
  return `${event.shiftKey && key.length > 1 ? "Shift+" : ""}${key}`;
}

/**
 * Binds keys while the component is mounted. With a `scope` element the keys work only
 * when focus is inside it; without one they are global.
 */
export function useHotkeys(
  keys: HotkeyMap,
  options: { enabled?: boolean; scope?: React.RefObject<HTMLElement | null> } = {},
): void {
  const latest = useRef(keys);
  useEffect(() => {
    latest.current = keys;
  });
  const { enabled = true, scope } = options;

  useEffect(() => {
    if (!enabled) return;
    function onKeyDown(event: KeyboardEvent) {
      if (event.metaKey || event.ctrlKey || event.altKey) return;
      if (isTypingTarget(event.target)) return;
      if (scope && !(scope.current && scope.current.contains(event.target as Node))) return;
      const handler = latest.current[chord(event)];
      if (handler) handler(event);
    }
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [enabled, scope]);
}
