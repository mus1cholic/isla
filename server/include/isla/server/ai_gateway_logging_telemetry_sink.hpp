#pragma once

#include <memory>

#include "isla/server/ai_gateway_telemetry.hpp"

namespace isla::server::ai_gateway {

struct LoggingTelemetrySinkConfig {
    bool log_events = false;
};

[[nodiscard]] std::shared_ptr<const TelemetrySink>
CreateLoggingTelemetrySink(LoggingTelemetrySinkConfig config = {});

} // namespace isla::server::ai_gateway
