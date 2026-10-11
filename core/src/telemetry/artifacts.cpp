#include "artifacts.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/simulation/kernel.hpp"

#include "codec.hpp"

#include <fstream>

namespace markettwin {
namespace {
using codec::Json;

class FileSink final : public RecordSink {
public:
  explicit FileSink(const RunArtifacts& paths)
      : commands_(paths.commands.string() + ".partial", std::ios::binary),
        trace_(paths.trace.string() + ".partial", std::ios::binary) {
    if (!commands_ || !trace_) {
      throw RunError{"IoError", "cannot create trace streams"};
    }
  }
  void append(TraceChannel channel, std::string_view record, Sequence sequence) override {
    auto& output = channel == TraceChannel::Commands ? commands_ : trace_;
    output << record;
    if (!output) {
      throw RunError{"IoError", "failed writing trace record", sequence};
    }
  }
  void close() {
    commands_.close();
    trace_.close();
    if (!commands_ || !trace_) {
      throw RunError{"IoError", "failed closing trace streams"};
    }
  }

private:
  std::ofstream commands_;
  std::ofstream trace_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
  output.close();
  if (!output) {
    throw RunError{"IoError", "failed writing " + path.string()};
  }
}
} // namespace

RunArtifacts artifact_paths(const std::filesystem::path& directory) {
  return {
      .directory = directory,
      .config = directory / "config.json",
      .commands = directory / "commands.jsonl",
      .trace = directory / "trace.jsonl",
      .manifest = directory / "manifest.json",
  };
}

RunSummary write_run(const RunConfig& config, const std::filesystem::path& directory) {
  const auto config_bytes = canonical_config(config) + '\n';
  const auto paths = artifact_paths(directory);
  try {
    if (!directory.parent_path().empty()) {
      std::filesystem::create_directories(directory.parent_path());
    }
    if (!std::filesystem::create_directory(directory)) {
      throw RunError{"OutputExists", "output directory already exists: " + directory.string()};
    }
    write_file(paths.config.string() + ".partial", config_bytes);
    FileSink sink{paths};
    auto summary = run_kernel(config, sink);
    sink.close();
    for (const auto& path : {paths.config, paths.commands, paths.trace}) {
      std::filesystem::rename(path.string() + ".partial", path);
    }
    const Json manifest{
        {"schema_version", 1},
        {"status", "Completed"},
        {"run_id", config.run_id},
        {"engine_version", version()},
        {"hash_algorithm", "sha256"},
        {
            "provenance",
            {
                {"git_revision", MARKETTWIN_GIT_REVISION},
                {"compiler", MARKETTWIN_COMPILER},
                {"build_type", MARKETTWIN_BUILD_TYPE},
            },
        },
        {"summary", codec::summary(summary)},
        {
            "artifacts",
            {
                {"config.json", sha256(config_bytes)},
                {"commands.jsonl", summary.commands_digest},
                {"trace.jsonl", summary.trace_digest},
            },
        },
    };
    write_file(paths.manifest.string() + ".partial", manifest.dump() + '\n');
    std::filesystem::rename(paths.manifest.string() + ".partial", paths.manifest);
    return summary;
  } catch (const std::filesystem::filesystem_error& error) {
    throw RunError{"IoError", error.what()};
  }
}

} // namespace markettwin
