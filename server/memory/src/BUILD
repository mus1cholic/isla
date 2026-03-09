load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "memory_types",
    srcs = ["memory_timestamp_utils.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "memory_types_test",
    srcs = ["memory_types_test.cpp"],
    deps = [
        ":memory_types",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)
