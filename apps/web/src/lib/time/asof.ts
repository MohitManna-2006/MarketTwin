// As-of lookups over recorded observations.
//
// A cursor at time T shows the latest observation recorded at or before T. Nothing is
// interpolated between two observations, and before the first one there is nothing to show.

/** Index of the last element <= target in an ascending array, or -1. */
export function indexAtOrBefore(sorted: ArrayLike<number>, target: number): number {
  let low = 0;
  let high = sorted.length - 1;
  let found = -1;
  while (low <= high) {
    const middle = (low + high) >>> 1;
    if ((sorted[middle] as number) <= target) {
      found = middle;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return found;
}

/** Index of the first element >= target in an ascending array, or length. */
export function indexAtOrAfter(sorted: ArrayLike<number>, target: number): number {
  let low = 0;
  let high = sorted.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if ((sorted[middle] as number) < target) low = middle + 1;
    else high = middle;
  }
  return low;
}

export function clamp(value: number, low: number, high: number): number {
  return Math.min(high, Math.max(low, value));
}
