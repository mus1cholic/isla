load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "memory_headers",
    hdrs = [
        "conversation.hpp",
        "memory_timestamp_utils.hpp",
        "memory_types.hpp",
        "memory_orchestrator.hpp",
        "working_memory_utils.hpp",
        "working_memory.hpp",
    ],
    strip_include_prefix = "/server/memory/include",
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)
