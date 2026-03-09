load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "server_headers",
    hdrs = [
        "ai_gateway_logging_utils.hpp",
        "ai_gateway_server.hpp",
        "ai_gateway_session_handler.hpp",
        "ai_gateway_stub_responder.hpp",
        "ai_gateway_stub_responder_utils.hpp",
        "ai_gateway_websocket_session.hpp",
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
