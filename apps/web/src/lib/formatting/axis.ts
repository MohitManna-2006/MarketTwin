/** Axis tick labels. Plot coordinates may be fractional; recorded values are never read from an axis. */
export function axisNumber(value: number, decimals = 0): string {
  const fixed = Math.abs(value).toFixed(decimals);
  const [whole = "0", fraction] = fixed.split(".");
  const grouped = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
  const sign = value < 0 && Number(fixed) !== 0 ? "−" : "";
  return sign + grouped + (fraction ? `.${fraction}` : "");
}

/** Large magnitudes on a narrow axis: 1,250,000 -> "1.25M". */
export function axisCompact(value: number): string {
  const magnitude = Math.abs(value);
  const sign = value < 0 ? "−" : "";
  if (magnitude >= 1e9) return `${sign}${(magnitude / 1e9).toFixed(2)}B`;
  if (magnitude >= 1e6) return `${sign}${(magnitude / 1e6).toFixed(2)}M`;
  if (magnitude >= 1e4) return `${sign}${(magnitude / 1e3).toFixed(1)}k`;
  return axisNumber(value);
}
