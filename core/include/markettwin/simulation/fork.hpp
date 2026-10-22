#ifndef MARKETTWIN_SIMULATION_FORK_HPP
#define MARKETTWIN_SIMULATION_FORK_HPP

#include "markettwin/simulation/scenario.hpp"

#include <optional>
#include <string>
#include <vector>

namespace markettwin {

// One AI desk decision as its Decision trace record states it, without the observation.
// `directive` is the directive in force after the decision, so a fallback carries the one
// it kept.
struct RecordedDecision {
  AgentId agent;
  SimTime at;
  std::uint64_t index = 0; // 1-based, per agent
  std::string source;      // model, fallback, inherited, parent, hold, override
  Directive directive;
  std::string model;
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
  std::string error_code;
  std::string observation_digest;
  bool operator==(const RecordedDecision&) const = default;
};

// After a stage, how a desk's later decisions are made.
enum class DecisionMode {
  Hold,   // the directive in force at the stage stays to the horizon
  Parent, // each later decision is the parent's recorded one at the same time
  Live,   // the decision source decides again on the new path
};

// Replaces one desk's decision at exactly the stage time.
struct DirectiveOverride {
  AgentId agent;
  Directive directive;
  bool operator==(const DirectiveOverride&) const = default;
};

// One change to a parent run at one instant (ADR 0012). Everything outside the config's
// `forks` and `run_id` is the parent's; the last stage's parent digest proves it.
struct ForkStage {
  std::string parent_run_id;
  std::string parent_config_digest;
  SimTime at;
  std::vector<Shock> add_shocks;
  std::vector<std::size_t> remove_shocks; // indices into the shocks in force before the stage
  std::vector<PolicyOverride> policy_overrides;
  std::optional<SimTime> horizon; // may only shorten the run
  DecisionMode decisions = DecisionMode::Hold;
  std::vector<RecordedDecision> recorded; // every decision of the parent run
  std::vector<DirectiveOverride> directive_overrides;
  bool operator==(const ForkStage&) const = default;
};

// What a desk sees at a decision, already rendered as canonical JSON, and what it holds.
struct DecisionRequest {
  AgentId agent;
  SimTime at;
  std::uint64_t index = 0;
  std::string_view mandate;
  std::string_view observation; // canonical JSON
  const Directive* current = nullptr;
};

// A source's answer. No directive means the source failed; the kernel keeps the current
// directive and records a fallback with `error_code`.
struct DecisionResponse {
  std::optional<Directive> directive;
  std::string source = "model"; // "fallback" only when replaying a recorded fallback
  std::string model;
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
  std::string error_code;
};

// Answers the decisions no fork stage has already fixed. Live runs ask a model; replay
// answers from the trace's Decision records, so it never calls one.
class DecisionSource {
public:
  DecisionSource() = default;
  virtual ~DecisionSource() = default;
  DecisionSource(const DecisionSource&) = default;
  DecisionSource& operator=(const DecisionSource&) = default;
  DecisionSource(DecisionSource&&) = default;
  DecisionSource& operator=(DecisionSource&&) = default;
  [[nodiscard]] virtual DecisionResponse decide(const DecisionRequest& request) = 0;
};

// A deterministic stand-in labelled `fixture`, for tests and offline use: it quotes the
// initial directive, widens and leans against inventory past half its position cap, and
// reduces past four fifths of it.
class FixtureDecisionSource final : public DecisionSource {
public:
  [[nodiscard]] DecisionResponse decide(const DecisionRequest& request) override;
};

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_FORK_HPP
