load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "engine",
    srcs = ["engine_bootstrap.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//engine/include/isla/engine:engine_headers",
        "//shared/src:shared",
    ],
)

