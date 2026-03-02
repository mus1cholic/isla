load("@rules_cc//cc:defs.bzl", "cc_library")

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
)


