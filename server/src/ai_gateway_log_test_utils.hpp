#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"

namespace isla::server::test {

class CapturingLogSink final : public absl::LogSink {
  public:
    void Send(const absl::LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::string(entry.text_message()));
    }

    [[nodiscard]] bool Contains(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& message : messages_) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

} // namespace isla::server::test
