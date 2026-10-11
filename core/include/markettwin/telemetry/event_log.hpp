#pragma once

#include "markettwin/common/types.hpp"

#include <string_view>

namespace markettwin {

enum class TraceChannel { Commands, Execution };

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
