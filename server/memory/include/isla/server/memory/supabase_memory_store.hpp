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
    bool telemetry_logging_enabled = false;
    std::string url;
    std::string service_role_key;
    std::string schema = "public";
    std::chrono::milliseconds request_timeout{ std::chrono::seconds(10) };
    std::string user_agent = "isla-memory-store/phase-1";
    std::optional<std::string> trusted_ca_cert_pem;
};

// Validates the transport and credential settings needed for the Supabase-backed MemoryStore when
// the store is enabled. Disabled configs are treated as a no-op and pass validation unchanged.
[[nodiscard]] absl::Status
ValidateSupabaseMemoryStoreConfig(const SupabaseMemoryStoreConfig& config);

// Creates an HTTP-backed MemoryStore that persists sessions, conversation state, and mid-term
// episodes through Supabase's REST and RPC endpoints.
[[nodiscard]] absl::StatusOr<MemoryStorePtr>
CreateSupabaseMemoryStore(SupabaseMemoryStoreConfig config);

} // namespace isla::server::memory
