load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_library(
    name = "shared",
    srcs = ["build_info.cpp"],
    visibility = ["//visibility:public"],
    deps = ["//shared/include/isla/shared:shared_headers"],
)

cc_library(
    name = "ai_gateway_protocol",
    srcs = [
        "ai_gateway_protocol.cpp",
        "ai_gateway_session.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//shared/include/isla/shared:ai_gateway_headers",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "test_runfiles",
    testonly = True,
    srcs = ["test_runfiles.cpp"],
    hdrs = ["test_runfiles.hpp"],
    visibility = ["//visibility:public"],
    deps = ["@rules_cc//cc/runfiles"],
)

cc_test(
    name = "ai_gateway_protocol_test",
    srcs = ["ai_gateway_protocol_test.cpp"],
    deps = [
        ":ai_gateway_protocol",
        "@googletest//:gtest_main",
    ],
)

