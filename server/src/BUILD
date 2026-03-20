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
        "//server/memory/src:supabase_memory_store",
        ":ai_gateway_logging_telemetry_sink",
        ":ai_gateway_logging_utils",
        ":ai_gateway_string_utils",
        ":gemini_api_embedding_client",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "gemini_api_embedding_client",
    srcs = ["gemini_api_embedding_client.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":http_json_client",
        ":ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@boost.beast//:boost.beast",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "tools",
    srcs = [
        "tools/expand_mid_term_tool.cpp",
        "tools/tool_registry.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "eval_runner",
    srcs = ["evals/eval_runner.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_server",
        ":ai_gateway_stub_responder",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
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
    name = "http_json_client",
    srcs = ["http_json_client.cpp"],
    hdrs = ["http_json_client.hpp"],
    includes = ["."],
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
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@boost.beast//:boost.beast",
    ],
)

cc_library(
    name = "openai_llm_client",
    srcs = ["openai_llm_client.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":openai_responses_client",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "ai_gateway_step_registry",
    srcs = ["ai_gateway_step_registry.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        ":openai_llm_client",
        ":openai_llms",
        ":openai_responses_client",
        ":tools",
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
    name = "openai_responses_inprocess_transport_resolver",
    hdrs = ["openai_responses_inprocess_transport_resolver.hpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        "@abseil-cpp//absl/status:statusor",
        "@boost.beast//:boost.beast",
    ],
)

cc_library(
    name = "openai_responses_inprocess_transport_test_hooks",
    testonly = True,
    srcs = ["openai_responses_inprocess_transport_test_hooks.cpp"],
    hdrs = ["openai_responses_inprocess_transport_test_hooks.hpp"],
    deps = [
        ":openai_responses_inprocess_transport_resolver",
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
        ":openai_responses_inprocess_transport_resolver",
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
    name = "ai_gateway_logging_telemetry_sink",
    srcs = ["ai_gateway_logging_telemetry_sink.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
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
        "@boost.uuid//:boost.uuid",
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
        "//server/memory/src:mid_term_compactor",
        "//server/memory/src:mid_term_flush_decider",
        ":ai_gateway_executor",
        ":ai_gateway_logging_utils",
        ":gemini_api_embedding_client",
        ":openai_llm_client",
        ":ai_gateway_planner",
        ":ai_gateway_server",
        ":ai_gateway_stub_responder_utils",
        ":tools",
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
        ":ai_gateway_logging_telemetry_sink",
        ":ai_gateway_startup_config",
        ":ai_gateway_logging_utils",
        ":ai_gateway_server",
        ":ai_gateway_stub_responder",
        "//server/memory/src:supabase_memory_store",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_test(
    name = "ai_gateway_telemetry_test",
    srcs = ["ai_gateway_telemetry_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "ai_gateway_telemetry_test_utils",
    testonly = True,
    hdrs = ["ai_gateway_telemetry_test_utils.hpp"],
    visibility = [
        "//server/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
    ],
)

cc_library(
    name = "ai_gateway_log_test_utils",
    testonly = True,
    hdrs = ["ai_gateway_log_test_utils.hpp"],
    visibility = [
        "//server/src:__pkg__",
        "//server/memory/src:__pkg__",
    ],
    deps = [
        "@abseil-cpp//absl/log:log_sink_registry",
    ],
)

cc_test(
    name = "ai_gateway_logging_telemetry_sink_test",
    srcs = ["ai_gateway_logging_telemetry_sink_test.cpp"],
    deps = [
        ":ai_gateway_log_test_utils",
        "//server/include/isla/server:server_headers",
        ":ai_gateway_logging_telemetry_sink",
        "@abseil-cpp//absl/log:log_sink_registry",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_startup_config_test",
    srcs = ["ai_gateway_startup_config_test.cpp"],
    deps = [
        ":ai_gateway_startup_config",
        "@abseil-cpp//absl/log:globals",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "openai_responses_test_utils",
    testonly = True,
    hdrs = ["openai_responses_test_utils.hpp"],
    visibility = [
        "//server/src:__pkg__",
        "//client/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        "@abseil-cpp//absl/status",
    ],
)

cc_library(
    name = "embedding_client_mock",
    testonly = True,
    hdrs = ["embedding_client_mock.hpp"],
    visibility = [
        "//server/src:__pkg__",
        "//server/memory/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "llm_client_mock",
    testonly = True,
    hdrs = ["llm_client_mock.hpp"],
    visibility = [
        "//server/src:__pkg__",
        "//server/memory/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "openai_responses_client_mock",
    testonly = True,
    hdrs = ["openai_responses_client_mock.hpp"],
    visibility = [
        "//server/src:__pkg__",
        "//client/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "ai_gateway_test_mocks",
    testonly = True,
    hdrs = ["ai_gateway_test_mocks.hpp"],
    visibility = [
        "//server/src:__pkg__",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest",
    ],
)

cc_test(
    name = "openai_llm_client_test",
    srcs = ["openai_llm_client_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":openai_llm_client",
        ":openai_responses_test_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "http_json_client_test",
    srcs = ["http_json_client_test.cpp"],
    deps = [
        ":http_json_client",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_step_registry_test",
    srcs = ["ai_gateway_step_registry_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_step_registry",
        ":ai_gateway_telemetry_test_utils",
        ":llm_client_mock",
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
        ":openai_responses_client_mock",
        ":ai_gateway_telemetry_test_utils",
        ":openai_llms",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "eval_runner_test",
    srcs = ["evals/eval_runner_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        "//server/memory/src:prompt_loader",
        ":eval_runner",
        ":openai_responses_test_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "openai_responses_client_test",
    srcs = ["openai_responses_client_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_telemetry_test_utils",
        ":openai_responses_client",
        ":openai_responses_inprocess_transport_test_hooks",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "openai_llms_test",
    srcs = ["openai_llms_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_telemetry_test_utils",
        ":llm_client_mock",
        ":openai_llms",
        ":openai_responses_test_utils",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "gemini_api_embedding_client_test",
    srcs = ["gemini_api_embedding_client_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":gemini_api_embedding_client",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "tools_test",
    srcs = [
        "tools/expand_mid_term_tool_test.cpp",
        "tools/tool_registry_test.cpp",
    ],
    deps = [
        "//server/include/isla/server:server_headers",
        ":tools",
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
        ":ai_gateway_test_mocks",
        ":openai_responses_test_utils",
        ":ai_gateway_stub_responder",
        "//server/memory/src:memory_store_mock",
        "@abseil-cpp//absl/status",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "ai_gateway_websocket_session_test",
    srcs = ["ai_gateway_websocket_session_test.cpp"],
    deps = [
        "//server/include/isla/server:server_headers",
        ":ai_gateway_test_mocks",
        ":ai_gateway_websocket_session",
        "@boost.uuid//:boost.uuid",
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
