load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "client_sdl_runtime",
    srcs = ["client_sdl_runtime.cpp"],
    hdrs = ["client_sdl_runtime.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//third_party/sdl3:sdl3"],
)

cc_library(
    name = "win32_layered_overlay",
    srcs = ["win32_layered_overlay.cpp"],
    hdrs = ["win32_layered_overlay.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//third_party/sdl3:sdl3"],
)

cc_library(
    name = "client_app",
    srcs = ["client_app.cpp"],
    hdrs = ["client_app.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/log:log",
        ":client_sdl_runtime",
        ":win32_layered_overlay",
        "//engine/src:engine",
        "//engine/src/render:render",
        "//engine/include/isla/engine/render",
        "//third_party/sdl3:sdl3",
    ],
)

cc_binary(
    name = "isla_client",
    srcs = ["main.cpp"],
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/log:log",
        ":client_app",
        "//engine/src:engine",
    ],
)

