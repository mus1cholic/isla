load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "memory_headers",
    hdrs = [
        "memory_timestamp_utils.hpp",
        "memory_types.hpp",
    ],
    strip_include_prefix = "/server/memory/include",
    visibility = ["//visibility:public"],
    deps = ["@nlohmann_json_upstream//:nlohmann_json"],
)
