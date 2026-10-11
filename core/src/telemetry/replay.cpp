#include "markettwin/telemetry/replay.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/simulation/kernel.hpp"

#include "artifacts.hpp"
#include "codec.hpp"

#include <fstream>

namespace markettwin {
namespace {
using codec::Json;

std::string bounded_line(std::istream& input, Sequence sequence) {
  std::string line;
  char character = 0;
  while (input.get(character)) {
    line.push_back(character);
    if (line.size() > codec::kMaxRecordBytes) {
      throw RunError{"ResourceLimit", "trace record exceeds byte limit", sequence};
    }
    if (character == '\n') {
      return line;
    }
  }
  if (input.bad()) {
    throw RunError{"IoError", "failed reading trace", sequence};
  }
  throw RunError{"IncompleteTrace", "missing or nonterminated trace record", sequence};
}

class VerifySink final : public RecordSink {
public:
  VerifySink(const RunArtifacts& paths, ReplayResult& result)
      : commands_(paths.commands, std::ios::binary), trace_(paths.trace, std::ios::binary),
        result_(&result) {
    if (!commands_ || !trace_) {
      throw RunError{"IncompleteArtifacts", "command or execution log is missing"};
    }
  }
  void append(TraceChannel channel, std::string_view record, Sequence sequence) override {
    auto& input = channel == TraceChannel::Commands ? commands_ : trace_;
    const auto expected = bounded_line(input, sequence);
    if (sequence == Sequence{}) {
      check_header(expected);
    }
    if (expected != record) {
      result_->expected = expected;
      result_->actual = record;
      throw RunError{"TraceMismatch",
                     channel == TraceChannel::Commands ? "command differs before exchange dispatch"
                                                       : "execution output differs",
                     sequence};
    }
    if (channel == TraceChannel::Commands) {
      result_->previous_command = record;
    } else {
      result_->previous_output = record;
    }
  }
  void finish() {
    if (commands_.peek() != std::char_traits<char>::eof() ||
        trace_.peek() != std::char_traits<char>::eof()) {
      throw RunError{"ExtraRecords", "records remain after the terminal run state"};
    }
    if (commands_.bad() || trace_.bad()) {
      throw RunError{"IoError", "failed checking trace EOF"};
    }
  }

private:
  static void check_header(std::string_view header) {
    const auto value = codec::parse(header);
    if (!value.is_object() || !value.contains("schema_version") ||
        codec::unsigned_integer(value.at("schema_version")) != 1) {
      throw RunError{"UnsupportedSchema", "unsupported trace schema"};
    }
  }
  std::ifstream commands_;
  std::ifstream trace_;
  ReplayResult* result_;
};

Json load_manifest(const RunArtifacts& paths) {
  if (!std::filesystem::exists(paths.manifest)) {
    throw RunError{"IncompleteArtifacts", "completed manifest is missing"};
  }
  auto manifest = codec::parse(codec::read_file(paths.manifest, codec::kMaxRecordBytes));
  if (!manifest.is_object() || !manifest.contains("schema_version") ||
      codec::unsigned_integer(manifest.at("schema_version")) != 1) {
    throw RunError{"UnsupportedSchema", "unsupported manifest schema"};
  }
  codec::fields(manifest, {
                              "schema_version",
                              "status",
                              "run_id",
                              "engine_version",
                              "hash_algorithm",
                              "provenance",
                              "summary",
                              "artifacts",
                          });
  if (codec::text(manifest.at("status")) != "Completed") {
    throw RunError{"IncompleteArtifacts", "manifest is not complete"};
  }
  if (codec::text(manifest.at("engine_version")) != version() ||
      codec::text(manifest.at("hash_algorithm")) != "sha256") {
    throw RunError{"UnsupportedVersion", "engine or hash version is incompatible"};
  }
  codec::fields(manifest.at("provenance"), {"git_revision", "compiler", "build_type"});
  for (const auto& value : manifest.at("provenance")) {
    static_cast<void>(codec::text(value));
  }
  codec::fields(manifest.at("artifacts"), {"config.json", "commands.jsonl", "trace.jsonl"});
  return manifest;
}

void verify_manifest(const Json& manifest, const RunConfig& config, const RunSummary& summary,
                     std::string_view config_bytes) {
  if (codec::text(manifest.at("run_id")) != config.run_id ||
      manifest.at("summary") != codec::summary(summary)) {
    throw RunError{"SummaryMismatch", "manifest run identity or terminal summary differs"};
  }
  const Json expected{
      {"config.json", sha256(config_bytes)},
      {"commands.jsonl", summary.commands_digest},
      {"trace.jsonl", summary.trace_digest},
  };
  if (manifest.at("artifacts") != expected) {
    throw RunError{"ChecksumMismatch", "manifest artifact checksum differs"};
  }
}
} // namespace

ReplayResult verify_replay(const std::filesystem::path& directory) {
  ReplayResult result;
  try {
    const auto paths = artifact_paths(directory);
    const auto manifest = load_manifest(paths);
    const auto config_bytes = codec::read_file(paths.config, codec::kMaxConfigBytes + 1);
    const auto config = parse_config(config_bytes);
    if (config_bytes != canonical_config(config) + '\n') {
      throw RunError{"NonCanonicalConfig", "stored config is not canonical"};
    }
    VerifySink sink{paths, result};
    result.summary = run_kernel(config, sink);
    sink.finish();
    verify_manifest(manifest, config, result.summary, config_bytes);
    result.verified = true;
    result.code = "Verified";
    result.message = "command trace, execution trace, final book, trades, and checksums agree";
    result.previous_command.clear();
    result.previous_output.clear();
  } catch (const RunError& error) {
    result.code = error.code();
    result.message = error.what();
    result.sequence = error.sequence();
  } catch (const std::filesystem::filesystem_error& error) {
    result.code = "IoError";
    result.message = error.what();
  }
  return result;
}

std::string replay_result_json(const ReplayResult& result) {
  return Json{
      {"verified", result.verified},
      {"code", result.code},
      {"message", result.message},
      {"sequence", result.sequence.value()},
      {"expected", result.expected},
      {"actual", result.actual},
      {"previous_command", result.previous_command},
      {"previous_output", result.previous_output},
      {"summary", result.verified ? codec::summary(result.summary) : Json(nullptr)},
  }
      .dump();
}

std::string summary_json(const RunSummary& summary) {
  return codec::summary(summary).dump();
}

} // namespace markettwin
