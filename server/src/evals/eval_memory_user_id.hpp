#pragma once

#include <string>
#include <string_view>

namespace isla::server::evals {

[[nodiscard]] std::string BuildBenchmarkMemoryUserId(std::string_view prefix,
                                                     std::string_view empty_fallback,
                                                     std::string_view benchmark_name);

} // namespace isla::server::evals
