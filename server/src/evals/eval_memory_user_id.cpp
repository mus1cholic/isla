#include "server/src/evals/eval_memory_user_id.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace isla::server::evals {
namespace {

std::string NormalizeBenchmarkName(std::string_view benchmark_name) {
    std::string benchmark_suffix;
    benchmark_suffix.reserve(benchmark_name.size());

    bool previous_was_separator = true;
    for (const unsigned char ch : benchmark_name) {
        if (std::isalnum(ch) != 0) {
            benchmark_suffix.push_back(static_cast<char>(std::tolower(ch)));
            previous_was_separator = false;
            continue;
        }
        if (!previous_was_separator) {
            benchmark_suffix.push_back('_');
            previous_was_separator = true;
        }
    }
    while (!benchmark_suffix.empty() && benchmark_suffix.back() == '_') {
        benchmark_suffix.pop_back();
    }

    return benchmark_suffix;
}

} // namespace

std::string BuildBenchmarkMemoryUserId(std::string_view prefix, std::string_view empty_fallback,
                                       std::string_view benchmark_name) {
    std::string benchmark_suffix = NormalizeBenchmarkName(benchmark_name);
    if (benchmark_suffix.empty()) {
        return std::string(empty_fallback);
    }

    return std::string(prefix) + benchmark_suffix;
}

} // namespace isla::server::evals
