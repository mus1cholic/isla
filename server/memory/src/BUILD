load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

genrule(
    name = "embedded_prompts_header",
    srcs = ["//server/memory/include/prompts:prompt_assets"],
    outs = ["embedded_prompts.inc"],
    cmd_bash = "$(location //server/memory/include/prompts:embed_prompts_tool) \"$@\" $(locations //server/memory/include/prompts:prompt_assets)",
    cmd_bat = "call $(location //server/memory/include/prompts:embed_prompts_tool) \"$@\" $(locations //server/memory/include/prompts:prompt_assets)",
    tools = ["//server/memory/include/prompts:embed_prompts_tool"],
)

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

cc_library(
    name = "memory_store",
    srcs = ["memory_store.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_types",
        "//server/memory/include/isla/server/memory:memory_headers",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "mid_term_memory",
    srcs = ["mid_term_memory.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_store",
        ":memory_types",
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "supabase_memory_store",
    srcs = ["supabase_memory_store.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_store",
        ":memory_types",
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/src:http_json_client",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
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

cc_library(
    name = "prompt_loader",
    srcs = [
        ":embedded_prompts_header",
        "prompt_loader.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "working_memory",
    srcs = [
        "system_prompt.cpp",
        "working_memory.cpp",
        "working_memory_utils.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":conversation",
        ":memory_types",
        ":prompt_loader",
        "//server/memory/include/isla/server/memory:memory_headers",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "mid_term_flush_decider",
    srcs = ["mid_term_flush_decider.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_types",
        ":prompt_loader",
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/include/isla/server:server_headers",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "mid_term_compactor",
    srcs = ["mid_term_compactor.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":memory_types",
        ":prompt_loader",
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/include/isla/server:server_headers",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_library(
    name = "memory_orchestrator",
    srcs = ["memory_orchestrator.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":working_memory",
        ":memory_store",
        "//server/memory/include/isla/server/memory:memory_headers",
        "//server/src:ai_gateway_logging_utils",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)

cc_library(
    name = "memory_store_mock",
    testonly = True,
    hdrs = ["memory_store_mock.hpp"],
    visibility = [
        "//server/memory/src:__pkg__",
        "//server/src:__pkg__",
    ],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "mid_term_compactor_mock",
    testonly = True,
    hdrs = ["mid_term_compactor_mock.hpp"],
    visibility = [
        "//server/memory/src:__pkg__",
        "//server/src:__pkg__",
    ],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "mid_term_flush_decider_mock",
    testonly = True,
    hdrs = ["mid_term_flush_decider_mock.hpp"],
    visibility = [
        "//server/memory/src:__pkg__",
        "//server/src:__pkg__",
    ],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "@googletest//:gtest",
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
    name = "prompt_loader_test",
    srcs = ["prompt_loader_test.cpp"],
    deps = [
        ":prompt_loader",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "system_prompt_test",
    srcs = ["system_prompt_test.cpp"],
    deps = [
        ":working_memory",
        "@googletest//:gtest_main",
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

cc_test(
    name = "memory_orchestrator_test",
    srcs = ["memory_orchestrator_test.cpp"],
    deps = [
        ":memory_store_mock",
        ":memory_orchestrator",
        ":mid_term_compactor_mock",
        ":mid_term_flush_decider_mock",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "memory_store_test",
    srcs = ["memory_store_test.cpp"],
    deps = [
        ":memory_store",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "mid_term_memory_test",
    srcs = ["mid_term_memory_test.cpp"],
    deps = [
        ":memory_store_mock",
        ":mid_term_memory",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "supabase_memory_store_test",
    srcs = ["supabase_memory_store_test.cpp"],
    deps = [
        "//server/src:ai_gateway_log_test_utils",
        ":supabase_memory_store",
        "@abseil-cpp//absl/log:log_sink_registry",
        "@boost.beast//:boost.beast",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "mid_term_compactor_test",
    srcs = ["mid_term_compactor_test.cpp"],
    deps = [
        "//server/src:llm_client_mock",
        ":mid_term_compactor",
        ":prompt_loader",
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)

cc_test(
    name = "mid_term_flush_decider_test",
    srcs = ["mid_term_flush_decider_test.cpp"],
    deps = [
        "//server/src:llm_client_mock",
        ":mid_term_flush_decider",
        ":prompt_loader",
        "//server/include/isla/server:server_headers",
        "@googletest//:gtest_main",
        "@nlohmann_json_upstream//:nlohmann_json",
    ],
)
