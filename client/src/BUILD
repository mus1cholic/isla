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
    name = "ai_gateway_client_session",
    srcs = ["ai_gateway_client_session.cpp"],
    hdrs = ["ai_gateway_client_session.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//shared/src:ai_gateway_protocol",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@boost.beast//:boost.beast",
    ],
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
        "@abseil-cpp//absl/log:log",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
        "//engine/src/math",
    ],
)

cc_library(
    name = "model_intake",
    srcs = ["model_intake.cpp"],
    hdrs = ["model_intake.hpp"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "client_app_internal_types",
    srcs = ["client_app_internal_types.cpp"],
    hdrs = ["client_app_internal_types.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        ":animated_mesh_skinning",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
    ],
)

cc_library(
    name = "client_app_geometry_utils",
    srcs = ["client_app_geometry_utils.cpp"],
    hdrs = ["client_app_geometry_utils.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/log:log",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/render:render_runtime",
        "//engine/src/math",
    ],
)

cc_library(
    name = "client_app_texture_remap",
    srcs = ["client_app_texture_remap.cpp"],
    hdrs = ["client_app_texture_remap.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/strings",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/render:render_runtime",
    ],
)

cc_library(
    name = "client_app_animation_world",
    srcs = ["client_app_animation_world.cpp"],
    hdrs = ["client_app_animation_world.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/log:log",
        ":animated_mesh_skinning",
        ":client_app_geometry_utils",
        ":client_app_internal_types",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/render:render_runtime",
    ],
)

cc_library(
    name = "client_app_physics_proxies",
    srcs = ["client_app_physics_proxies.cpp"],
    hdrs = ["client_app_physics_proxies.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/log:log",
        ":client_app_geometry_utils",
        ":client_app_internal_types",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/render:render_runtime",
    ],
)

cc_library(
    name = "client_app_startup_loader",
    srcs = ["client_app_startup_loader.cpp"],
    hdrs = ["client_app_startup_loader.hpp"],
    visibility = ["//visibility:private"],
    deps = [
        "@abseil-cpp//absl/log:log",
        ":client_app_geometry_utils",
        ":client_app_internal_types",
        ":client_app_texture_remap",
        ":model_intake",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/render:render_runtime",
    ],
)

cc_library(
    name = "client_app",
    srcs = ["client_app.cpp"],
    hdrs = ["client_app.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/log:log",
        ":ai_gateway_client_session",
        ":client_app_animation_world",
        ":client_app_geometry_utils",
        ":client_app_internal_types",
        ":client_app_physics_proxies",
        ":client_app_startup_loader",
        ":client_sdl_runtime",
        ":win32_layered_overlay",
        "//engine/include/isla/engine/render:render_runtime",
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
    name = "model_intake_test",
    srcs = ["model_intake_test.cpp"],
    deps = [
        ":model_intake",
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
        "//engine/src/render:model_renderer_test_hooks",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "client_app_gateway_test",
    srcs = [
        "client_app_gateway_test.cpp",
        "client_app_test_hooks.hpp",
    ],
    deps = [
        ":client_app",
        "//engine/src/render:model_renderer_test_hooks",
        "//server/include/isla/server:server_headers",
        "//server/src:ai_gateway_server",
        "//server/src:ai_gateway_stub_responder",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "isla_ai_gateway_smoke_client",
    srcs = ["ai_gateway_smoke_client_main.cpp"],
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/log:log",
        ":ai_gateway_client_session",
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
    name = "ai_gateway_client_session_test",
    srcs = ["ai_gateway_client_session_test.cpp"],
    deps = [
        ":ai_gateway_client_session",
        "//server/include/isla/server:server_headers",
        "//server/src:ai_gateway_server",
        "//server/src:ai_gateway_stub_responder",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "win32_layered_overlay_test",
    srcs = ["win32_layered_overlay_test.cpp"],
    data = ["win32_layered_overlay.cpp"],
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
        "//shared/src:test_runfiles",
        "@abseil-cpp//absl/strings",
        "@googletest//:gtest_main",
    ],
)
