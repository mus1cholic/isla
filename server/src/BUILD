load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "ai_gateway_session_handler",
    srcs = ["ai_gateway_session_handler.cpp"],
    hdrs = ["ai_gateway_session_handler.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//shared/src:ai_gateway_protocol",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_test(
    name = "ai_gateway_session_handler_test",
    srcs = ["ai_gateway_session_handler_test.cpp"],
    deps = [
        ":ai_gateway_session_handler",
        "@googletest//:gtest_main",
    ],
)
