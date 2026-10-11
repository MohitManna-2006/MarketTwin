#!/usr/bin/env bash
# Run clang-tidy against MarketTwin C++ sources.
# Usage: scripts/lint.sh [build-directory]
# The build directory must contain compile_commands.json.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${root}/build/debug}"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  echo "error: ${build_dir}/compile_commands.json was not found." >&2
  echo "Configure a build first:" >&2
  echo "  cmake --preset debug" >&2
  echo "  cmake --build --preset debug" >&2
  exit 1
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
  echo "  Ubuntu: sudo apt-get install clang-tidy" >&2
  return 127
}

tidy="$(find_tool clang-tidy)"

extra_args=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v xcrun >/dev/null 2>&1; then
  sdk_path="$(xcrun --show-sdk-path)"
  # Homebrew clang-tidy does not inherit Apple Clang's SDK lookup.
  extra_args+=(--extra-arg=-isysroot --extra-arg="${sdk_path}")
fi

files=()
while IFS= read -r file; do
  files+=("${file}")
done < <(
  ruby -rjson -e '
    database = JSON.parse(File.read(ARGV[0]))
    root = File.realpath(ARGV[1])
    core_stat = File.stat(File.join(root, "core"))
    seen = {}
    inside = lambda do |path|
      dir = File.dirname(path)
      loop do
        stat = File.stat(dir)
        return true if stat.dev == core_stat.dev && stat.ino == core_stat.ino
        parent = File.dirname(dir)
        return false if parent == dir
        dir = parent
      rescue Errno::ENOENT
        return false
      end
    end
    database.each do |entry|
      file = entry["file"]
      next unless file&.end_with?(".cpp")
      next if seen[file]
      # macOS can record this repository through a different-case path or a
      # symlink. Identify it by the core directory inode, and pass clang-tidy
      # the path stored in the database so its lookup still matches.
      real = File.realpath(file)
      next unless inside.call(real)
      seen[file] = true
      puts file
    rescue Errno::ENOENT
      next
    end
  ' "${build_dir}/compile_commands.json" "${root}" | sort
)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "error: compile_commands.json has no MarketTwin sources under ${root}/core" >&2
  exit 1
fi

"${tidy}" -p "${build_dir}" --warnings-as-errors='*' "${extra_args[@]}" "${files[@]}"
echo "clang-tidy passed (${#files[@]} files, ${tidy})."
