#!/usr/bin/env bash
# Check or apply .clang-format to MarketTwin C++ sources.
# Usage: scripts/format.sh [--fix]

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="check"
if [[ "${1:-}" == "--fix" ]]; then
  mode="fix"
elif [[ -n "${1:-}" ]]; then
  echo "Usage: scripts/format.sh [--fix]" >&2
  exit 2
fi

find_tool() {
  local name="$1"
  if command -v "${name}" >/dev/null 2>&1; then
    command -v "${name}"
    return 0
  fi

  local candidate
  for candidate in \
    "/opt/homebrew/opt/llvm/bin/${name}" \
    "/opt/homebrew/opt/llvm@20/bin/${name}" \
    "/usr/lib/llvm-18/bin/${name}" \
    "/usr/lib/llvm-20/bin/${name}"; do
    if [[ -x "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done

  echo "error: ${name} was not found on PATH." >&2
  echo "Install it, then re-run this script:" >&2
  echo "  macOS:  brew install llvm" >&2
  echo "          export PATH=\"\$(brew --prefix llvm)/bin:\$PATH\"" >&2
  echo "  Ubuntu: sudo apt-get install clang-format clang-tidy" >&2
  return 127
}

formatter="$(find_tool clang-format)"

files=()
while IFS= read -r file; do
  files+=("${file}")
done < <(find "${root}/core" -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "error: no C++ sources found under ${root}/core" >&2
  exit 1
fi

if [[ "${mode}" == "fix" ]]; then
  "${formatter}" -i "${files[@]}"
  echo "Formatted ${#files[@]} files with ${formatter}."
else
  "${formatter}" --dry-run --Werror "${files[@]}"
  echo "clang-format check passed (${#files[@]} files, ${formatter})."
fi
