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

cc_library(
    name = "conversation",
    srcs = ["conversation.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_types",
        "//server/memory/include/isla/server/memory:memory_headers",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
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

cc_library(
    name = "working_memory",
    srcs = [
        "working_memory.cpp",
        "working_memory_utils.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":conversation",
        ":memory_types",
        "//server/memory/include/isla/server/memory:memory_headers",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_test(
    name = "conversation_test",
    srcs = ["conversation_test.cpp"],
    deps = [
        ":conversation",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "working_memory_test",
    srcs = ["working_memory_test.cpp"],
    deps = [
        ":working_memory",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)
