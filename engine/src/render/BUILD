load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

config_setting(
    name = "windows_gnu",
    values = {"define": "isla_windows_gnu=true"},
)

config_setting(
    name = "windows_msvc",
    values = {"define": "isla_windows_gnu=false"},
)

cc_library(
    name = "render",
    srcs = [
        "mesh_asset_loader.cpp",
        "model_renderer.cpp",
        "render_world.cpp",
        "include/mesh_asset_loader.hpp",
        "include/texture_loader_limits.hpp"
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//engine/src/math",
        "@abseil-cpp//absl/log:log",
        "@cgltf_upstream//:cgltf",
        "//engine/include/isla/engine/render",
        "//engine/include/isla/engine/math",
        "//third_party/sdl3:sdl3",
    ],
    linkopts = select({
        ":windows_gnu": [
            "-ld3d11",
            "-ldxgi",
        ],
        ":windows_msvc": [
            "d3d11.lib",
            "dxgi.lib",
        ],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "overlay_transparency_contract_test",
    srcs = ["overlay_transparency_contract_test.cpp"],
    deps = [
        "//engine/include/isla/engine/render",
        "@googletest//:gtest_main",
    ],
)


