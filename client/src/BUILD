load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

exports_files(["win32_layered_overlay.cpp"])

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
        ":windows_gnu": [
            "-luser32",
            "-ldwmapi",
        ],
        ":windows_msvc": [
            "user32.lib",
            "dwmapi.lib",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "@abseil-cpp//absl/log:log",
        "//engine/include/isla/engine/render",
        "//third_party/sdl3:sdl3",
    ],
)

cc_library(
    name = "animated_mesh_skinning",
    srcs = ["animated_mesh_skinning.cpp"],
    hdrs = ["animated_mesh_skinning.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//engine/src/render:render",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
    ],
)

cc_library(
    name = "client_app",
    srcs = ["client_app.cpp"],
    hdrs = ["client_app.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/log:log",
        ":animated_mesh_skinning",
        ":client_sdl_runtime",
        ":win32_layered_overlay",
        "//engine/src:engine",
        "//engine/src/render:render",
        "//engine/include/isla/engine/render",
        "//third_party/sdl3:sdl3",
    ],
)

cc_test(
    name = "animated_mesh_skinning_test",
    srcs = ["animated_mesh_skinning_test.cpp"],
    deps = [
        ":animated_mesh_skinning",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "client_app_animation_test",
    srcs = [
        "client_app_animation_test.cpp",
        "client_app_test_hooks.hpp",
    ],
    deps = [
        ":client_app",
        "//engine/src/render:render",
        "@googletest//:gtest_main",
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
            "-ldwmapi",
        ],
        ":windows_msvc": [
            "user32.lib",
            "advapi32.lib",
            "dwmapi.lib",
        ],
        "//conditions:default": [],
    }),
    deps = [
        ":win32_layered_overlay",
        "@googletest//:gtest_main",
    ],
)
