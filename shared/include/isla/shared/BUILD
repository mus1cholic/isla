load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "shared_headers",
    hdrs = ["build_info.hpp"],
    strip_include_prefix = "/shared/include",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ai_gateway_headers",
    hdrs = [
        "ai_gateway_protocol.hpp",
        "ai_gateway_session.hpp",
    ],
    strip_include_prefix = "/shared/include",
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
    ],
)
