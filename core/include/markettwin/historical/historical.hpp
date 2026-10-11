#ifndef MARKETTWIN_HISTORICAL_HISTORICAL_HPP
#define MARKETTWIN_HISTORICAL_HISTORICAL_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace markettwin::historical {

inline constexpr std::uint32_t kHistoricalSchema = 1;
inline constexpr std::uint32_t kHistoricalNormalizationVersion = 1;

// Historical observations never enter the synthetic exchange or its clock.
// Prices are exact millionths of USD; timestamps are UTC nanoseconds represented
// as decimal strings in JSON so browser clients cannot lose integer precision.
// A completed new directory contains raw.jsonl, historical_events.jsonl,
// historical_samples.jsonl, historical_metrics.json, quality_report.json, and
// manifest.json. A failed import publishes no completed directory.
std::string import_jsonl(const std::filesystem::path& input, const std::filesystem::path& output,
                         const std::string& metadata_json);
std::string verify(const std::filesystem::path& session_directory);

} // namespace markettwin::historical

#endif
