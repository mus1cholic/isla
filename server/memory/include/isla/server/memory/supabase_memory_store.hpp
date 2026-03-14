#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isla/server/memory/memory_store.hpp"

namespace isla::server::memory {

struct SupabaseMemoryStoreConfig {
    bool enabled = false;
    std::string url;
    std::string service_role_key;
    std::string schema = "public";
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(10) };
    std::string user_agent = "isla-memory-store/phase-1";
    std::optional<std::string> trusted_ca_cert_pem;
};

[[nodiscard]] absl::Status ValidateSupabaseMemoryStoreConfig(
    const SupabaseMemoryStoreConfig& config);
[[nodiscard]] absl::StatusOr<MemoryStorePtr>
CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig config);

} // namespace isla::server::memory
