#include "markettwin/telemetry/replay.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/simulation/kernel.hpp"

#include "artifacts.hpp"
#include "codec.hpp"

#include <fstream>
#include <map>

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
        accounting_(paths.accounting, std::ios::binary), result_(&result) {
    if (!commands_ || !trace_ || !accounting_) {
      throw RunError{"IncompleteArtifacts", "command, execution, or accounting log is missing"};
    }
  }
  void append(TraceChannel channel, std::string_view record, Sequence sequence) override {
    const bool accounting = channel == TraceChannel::Accounting;
    auto& input = stream(channel);
    const auto expected = bounded_line(input, sequence);
    if (sequence == Sequence{}) {
      check_header(expected, accounting ? kAccountingSchema : 1);
    }
    if (expected != record) {
      result_->expected = expected;
      result_->actual = record;
      if (accounting) {
        // The commands and executions up to here agreed, so the same market produced
        // different books: the sequence is the accounting stream's own.
        throw RunError{"AccountingMismatch", "accounting record differs", sequence};
      }
      throw RunError{"TraceMismatch",
                     channel == TraceChannel::Commands ? "command differs before exchange dispatch"
                                                       : "execution output differs",
                     sequence};
    }
    if (channel == TraceChannel::Commands) {
      result_->previous_command = record;
    } else if (channel == TraceChannel::Execution) {
      result_->previous_output = record;
    }
  }
  void finish() {
    if (commands_.peek() != std::char_traits<char>::eof() ||
        trace_.peek() != std::char_traits<char>::eof() ||
        accounting_.peek() != std::char_traits<char>::eof()) {
      throw RunError{"ExtraRecords", "records remain after the terminal run state"};
    }
    if (commands_.bad() || trace_.bad() || accounting_.bad()) {
      throw RunError{"IoError", "failed checking trace EOF"};
    }
  }

private:
  [[nodiscard]] std::ifstream& stream(TraceChannel channel) noexcept {
    switch (channel) {
    case TraceChannel::Commands:
      return commands_;
    case TraceChannel::Execution:
      return trace_;
    case TraceChannel::Accounting:
      break;
    }
    return accounting_;
  }
  static void check_header(std::string_view header, std::uint64_t schema) {
    const auto value = codec::parse(header);
    if (!value.is_object() || !value.contains("schema_version") ||
        codec::unsigned_integer(value.at("schema_version")) != schema) {
      throw RunError{"UnsupportedSchema", "unsupported trace or accounting schema"};
    }
  }
  std::ifstream commands_;
  std::ifstream trace_;
  std::ifstream accounting_;
  ReplayResult* result_;
};

// Answers each AI desk decision the run asks a source for from the trace's own Decision
// record at that time, so replay never calls a model (ADR 0012). Decisions a fork stage
// fixed are recorded too but never asked for; a missing or extra decision still shows
// as a trace mismatch.
class RecordedDecisions final : public DecisionSource {
public:
  explicit RecordedDecisions(const std::filesystem::path& trace) {
    std::ifstream input{trace, std::ios::binary};
    std::string line;
    while (std::getline(input, line)) {
      if (line.find(R"("kind":"Decision")") == std::string::npos) {
        continue;
      }
      const auto record = codec::parse(line);
      if (codec::text(record.at("kind")) != "Decision") {
        continue;
      }
      auto decision = codec::read_decision_fields(
          record.at("payload"), SimTime{codec::unsigned_integer(record.at("timestamp_ns"))});
      if (decision.source == "model" || decision.source == "fallback") {
        decisions_.emplace(std::pair{decision.agent.value(), decision.at.value()},
                           std::move(decision));
      }
    }
  }

  DecisionResponse decide(const DecisionRequest& request) override {
    const auto found = decisions_.find({request.agent.value(), request.at.value()});
    if (found == decisions_.end()) {
      throw RunError{"DecisionMismatch", "the run asks for a decision the trace does not record"};
    }
    const auto& item = found->second;
    DecisionResponse response{
        .directive = {},
        .source = item.source,
        .model = item.model,
        .input_tokens = item.input_tokens,
        .output_tokens = item.output_tokens,
        .error_code = item.error_code,
    };
    if (item.source == "model") {
      response.directive = item.directive;
    }
    return response;
  }

private:
  std::map<std::pair<std::uint64_t, std::uint64_t>, RecordedDecision> decisions_;
};

Json load_manifest(const RunArtifacts& paths) {
  if (!std::filesystem::exists(paths.manifest)) {
    throw RunError{"IncompleteArtifacts", "completed manifest is missing"};
  }
  auto manifest = codec::parse(codec::read_file(paths.manifest, codec::kMaxRecordBytes));
  if (!manifest.is_object() || !manifest.contains("schema_version") ||
      codec::unsigned_integer(manifest.at("schema_version")) != codec::kManifestSchema) {
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
                              "schemas",
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
  codec::fields(manifest.at("artifacts"), {
                                              "config.json",
                                              "commands.jsonl",
                                              "trace.jsonl",
                                              "accounting.jsonl",
                                              "metrics.json",
                                          });
  const Json schemas{
      {"trace", 1},
      {"accounting", kAccountingSchema},
      {"metrics", kMetricsSchema},
  };
  if (manifest.at("schemas") != schemas) {
    throw RunError{"UnsupportedSchema", "unsupported trace, accounting, or metrics schema"};
  }
  return manifest;
}

void verify_manifest(const Json& manifest, const RunConfig& config, const RunSummary& summary,
                     std::string_view config_bytes) {
  if (codec::text(manifest.at("run_id")) != config.run_id ||
      manifest.at("summary") != codec::summary(summary)) {
    throw RunError{"SummaryMismatch", "manifest run identity or terminal summary differs"};
  }
  const Json expected{
      {"config.json", sha256(config_bytes)},    {"commands.jsonl", summary.commands_digest},
      {"trace.jsonl", summary.trace_digest},    {"accounting.jsonl", summary.accounting_digest},
      {"metrics.json", summary.metrics_digest},
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
    RecordedDecisions decisions{paths.trace};
    result.summary = run_kernel(config, sink, nullptr, &decisions);
    sink.finish();
    // The recomputed metrics must be the stored file, byte for byte.
    const auto metrics_bytes = codec::metrics(config, result.summary).dump() + '\n';
    if (codec::read_file(paths.metrics, metrics_bytes.size() + 1) != metrics_bytes) {
      throw RunError{"MetricsMismatch", "stored metrics differ from the recomputed run"};
    }
    verify_manifest(manifest, config, result.summary, config_bytes);
    result.verified = true;
    result.code = "Verified";
    result.message = "command trace, execution trace, accounting stream, metrics, final book, "
                     "trades, and checksums agree";
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

std::string metrics_json(const RunConfig& config, const RunSummary& summary) {
  return codec::metrics(config, summary).dump();
}

} // namespace markettwin
