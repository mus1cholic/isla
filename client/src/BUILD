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
    linkopts = select({
        ":windows_gnu": ["-luser32"],
        ":windows_msvc": ["user32.lib"],
        "//conditions:default": [],
    }),
    deps = [
        "@abseil-cpp//absl/log:log",
        "//engine/include/isla/engine/render",
        "//third_party/sdl3:sdl3",
    ],
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

cc_test(
    name = "win32_layered_overlay_test",
    srcs = ["win32_layered_overlay_test.cpp"],
    linkopts = select({
        ":windows_gnu": [
            "-luser32",
            "-ladvapi32",
        ],
        ":windows_msvc": [
            "user32.lib",
            "advapi32.lib",
        ],
        "//conditions:default": [],
    }),
    deps = [
        ":win32_layered_overlay",
        "@googletest//:gtest_main",
    ],
)
