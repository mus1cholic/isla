load("@rules_cc//cc:defs.bzl", "cc_library")

exports_files([
    "openai_responses_curl_transport.hpp",
    "openai_responses_http_utils.hpp",
    "openai_responses_inprocess_transport.hpp",
    "openai_responses_json_utils.hpp",
    "openai_responses_sse_parser.hpp",
    "openai_responses_transport_utils.hpp",
])

cc_library(
    name = "server_headers",
    hdrs = [
        "ai_gateway_telemetry.hpp",
        "ai_gateway_executor.hpp",
        "ai_gateway_logging_utils.hpp",
        "ai_gateway_planner.hpp",
        "ai_gateway_step_registry.hpp",
        "openai_reasoning_effort.hpp",
        "openai_llms.hpp",
        "openai_responses_client.hpp",
        "ai_gateway_server.hpp",
        "ai_gateway_session_handler.hpp",
        "ai_gateway_stub_responder.hpp",
        "ai_gateway_stub_responder_utils.hpp",
        "ai_gateway_websocket_session.hpp",
        "openai_responses_curl_transport.hpp",
        "openai_responses_http_utils.hpp",
        "openai_responses_inprocess_transport.hpp",
        "openai_responses_json_utils.hpp",
        "openai_responses_sse_parser.hpp",
        "openai_responses_transport_utils.hpp",
    ],
    strip_include_prefix = "/server/include",
    visibility = ["//visibility:public"],
    deps = [
        "//server/memory/include/isla/server/memory:memory_headers",
        "//shared/src:ai_gateway_protocol",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
