load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

config_setting(
    name = "windows_gnu",
    values = {"define": "isla_windows_gnu=true"},
)

config_setting(
    name = "windows_msvc",
    values = {"define": "isla_windows_gnu=false"},
)

cc_library(
    name = "ai_gateway_startup_config",
    srcs = ["ai_gateway_startup_config.cpp"],
    hdrs = ["ai_gateway_startup_config.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":ai_gateway_string_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "ai_gateway_string_utils",
    srcs = ["ai_gateway_string_utils.cpp"],
    hdrs = ["ai_gateway_string_utils.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
    ],
)

cc_library(
    name = "ai_gateway_step_registry",
    srcs = ["ai_gateway_step_registry.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":openai_llms",
        ":openai_responses_client",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "ai_gateway_planner",
    srcs = ["ai_gateway_planner.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        "//server/memory/src:prompt_loader",
        ":openai_llms",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "openai_llms",
    srcs = ["openai_llms.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":openai_responses_client",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "openai_responses_client",
    srcs = [
        "openai_responses_client.cpp",
        "openai_responses_curl_transport.cpp",
        "openai_responses_http_utils.cpp",
        "openai_responses_inprocess_transport.cpp",
        "openai_responses_json_utils.cpp",
        "openai_responses_sse_parser.cpp",
        "openai_responses_transport_utils.cpp",
    ],
    hdrs = [
        "openai_responses_inprocess_transport_test_hooks.hpp",
    ],
    includes = [
        "../../server/include",
        ".",
    ],
    visibility = ["//visibility:public"],
    linkopts = select({
        ":windows_gnu": [
            "-lws2_32",
            "-lsecur32",
            "-ladvapi32",
        ],
        ":windows_msvc": [
            "Ws2_32.lib",
            "Secur32.lib",
            "Advapi32.lib",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "//server/include/isla/server:server_headers",
        "//server/src:ai_gateway_string_utils",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@boost.beast//:boost.beast",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "ai_gateway_executor",
    srcs = ["ai_gateway_executor.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":openai_llms",
        ":openai_responses_client",
        ":ai_gateway_step_registry",
        "@abseil-cpp//absl/log:log",
    ],
)

cc_library(
    name = "ai_gateway_session_handler",
    srcs = ["ai_gateway_session_handler.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        "//shared/src:ai_gateway_protocol",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "ai_gateway_logging_utils",
    srcs = ["ai_gateway_logging_utils.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
    ],
)

cc_library(
    name = "ai_gateway_websocket_session",
    srcs = ["ai_gateway_websocket_session.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
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
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":ai_gateway_websocket_session",
        "@boost.beast//:boost.beast",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
    ],
)

cc_library(
    name = "ai_gateway_stub_responder_utils",
    srcs = ["ai_gateway_stub_responder_utils.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        "//server/memory/include/isla/server/memory:memory_headers",
    ],
)

cc_library(
    name = "ai_gateway_stub_responder",
    srcs = ["ai_gateway_stub_responder.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_executor",
        ":ai_gateway_logging_utils",
        ":ai_gateway_planner",
        ":ai_gateway_server",
        ":ai_gateway_stub_responder_utils",
        "//server/memory/src:memory_orchestrator",
        "@abseil-cpp//absl/container:flat_hash_map",
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
        "//server/include/isla/server:server_headers",
        ":ai_gateway_startup_config",
        ":ai_gateway_logging_utils",
        ":ai_gateway_server",
        ":ai_gateway_stub_responder",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_test(
    name = "ai_gateway_startup_config_test",
    srcs = ["ai_gateway_startup_config_test.cpp"],
    deps = [
        ":ai_gateway_startup_config",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "openai_responses_test_utils",
    testonly = True,
    hdrs = ["openai_responses_test_utils.hpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        "@abseil-cpp//absl/status",
    ],
)

cc_test(
    name = "ai_gateway_step_registry_test",
    srcs = ["ai_gateway_step_registry_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_step_registry",
        ":openai_responses_test_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_planner_test",
    srcs = ["ai_gateway_planner_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        "//server/memory/src:prompt_loader",
        ":ai_gateway_executor",
        ":openai_llms",
        ":ai_gateway_planner",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_executor_test",
    srcs = ["ai_gateway_executor_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_executor",
        ":openai_llms",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "openai_responses_client_test",
    srcs = ["openai_responses_client_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":openai_responses_client",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "openai_llms_test",
    srcs = ["openai_llms_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":openai_llms",
        ":openai_responses_test_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_session_handler_test",
    srcs = ["ai_gateway_session_handler_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_session_handler",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_logging_utils_test",
    srcs = ["ai_gateway_logging_utils_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_stub_responder_test",
    srcs = ["ai_gateway_stub_responder_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_server",
        ":openai_responses_test_utils",
        ":ai_gateway_stub_responder",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_websocket_session_test",
    srcs = ["ai_gateway_websocket_session_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_websocket_session",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_server_integration_test",
    srcs = ["ai_gateway_server_integration_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_server",
        ":openai_responses_test_utils",
        ":ai_gateway_stub_responder",
        "//shared/src:ai_gateway_protocol",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
    ],
)
