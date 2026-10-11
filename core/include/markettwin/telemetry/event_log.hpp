#ifndef MARKETTWIN_TELEMETRY_EVENT_LOG_HPP
#define MARKETTWIN_TELEMETRY_EVENT_LOG_HPP

#include "markettwin/common/types.hpp"

#include <string_view>

namespace markettwin {

// Accounting records carry their own sequence, separate from the trace sequence that
// orders commands and their outcomes.
enum class TraceChannel { Commands, Execution, Accounting };

// Native internal boundary. Implementations write artifacts or verify replay records.
class RecordSink {
public:
  RecordSink() = default;
  virtual ~RecordSink() = default;
  RecordSink(const RecordSink&) = default;
  RecordSink& operator=(const RecordSink&) = default;
  RecordSink(RecordSink&&) = default;
  RecordSink& operator=(RecordSink&&) = default;
  virtual void append(TraceChannel channel, std::string_view record, Sequence sequence) = 0;
};

class NullSink final : public RecordSink {
public:
  void append(TraceChannel /*channel*/, std::string_view /*record*/,
              Sequence /*sequence*/) override {}
};

} // namespace markettwin
#endif // MARKETTWIN_TELEMETRY_EVENT_LOG_HPP
