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

cc_library(
    name = "ai_gateway_logging_utils",
    srcs = ["ai_gateway_logging_utils.cpp"],
    hdrs = ["ai_gateway_logging_utils.hpp"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ai_gateway_websocket_session",
    srcs = ["ai_gateway_websocket_session.cpp"],
    hdrs = ["ai_gateway_websocket_session.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":ai_gateway_logging_utils",
        ":ai_gateway_session_handler",
        "//shared/src:ai_gateway_protocol",
        "@abseil-cpp//absl/log:log",
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

cc_test(
    name = "ai_gateway_logging_utils_test",
    srcs = ["ai_gateway_logging_utils_test.cpp"],
    deps = [
        ":ai_gateway_logging_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_websocket_session_test",
    srcs = ["ai_gateway_websocket_session_test.cpp"],
    deps = [
        ":ai_gateway_websocket_session",
        "@googletest//:gtest_main",
    ],
)
