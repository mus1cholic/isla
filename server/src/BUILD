load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

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

cc_library(
    name = "ai_gateway_server",
    srcs = ["ai_gateway_server.cpp"],
    hdrs = ["ai_gateway_server.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":ai_gateway_logging_utils",
        ":ai_gateway_websocket_session",
        "@boost.beast//:boost.beast",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
    ],
)

cc_binary(
    name = "isla_ai_gateway",
    srcs = ["ai_gateway_server_main.cpp"],
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        ":ai_gateway_logging_utils",
        ":ai_gateway_server",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
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

cc_test(
    name = "ai_gateway_server_integration_test",
    srcs = ["ai_gateway_server_integration_test.cpp"],
    deps = [
        ":ai_gateway_server",
        "//shared/src:ai_gateway_protocol",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
    ],
)
