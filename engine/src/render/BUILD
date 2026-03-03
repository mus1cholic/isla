load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "render",
    srcs = [
        "animation_playback_controller.cpp",
        "animated_gltf.cpp",
        "include/bgfx_limits.hpp",
        "include/bgfx_mesh_manager.hpp",
        "include/bgfx_shader_manager.hpp",
        "include/bgfx_texture_manager.hpp",
        "include/animation_playback_controller.hpp",
        "include/animated_gltf.hpp",
        "include/mesh_asset_loader.hpp",
        "include/shader_path_resolver.hpp",
        "include/texture_loader_limits.hpp",
        "mesh_asset_loader.cpp",
        "model_renderer.cpp",
        "render_world.cpp",
        "shader_path_resolver.cpp",
    ] + select({
        "@platforms//os:windows": [
            "bgfx_mesh_manager.cpp",
            "bgfx_shader_manager.cpp",
            "bgfx_texture_manager.cpp",
        ],
        "//conditions:default": [],
    }),
    defines = select({
        "@platforms//os:windows": ["ISLA_HAS_BGFX"],
        "//conditions:default": [],
    }),
    data = select({
        "@platforms//os:windows": ["//engine/src/render/shaders:bgfx_shader_bins"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        "//engine/src/math",
        "@abseil-cpp//absl/log:log",
        "@abseil-cpp//absl/strings",
        "@cgltf_upstream//:cgltf",
        "//engine/include/isla/engine/math",
        "//engine/include/isla/engine/render",
        "//third_party/sdl3:sdl3",
    ] + select({
        "@platforms//os:windows": ["//third_party/bgfx:bgfx"],
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

exports_files([
    "testdata/textured_quad.obj",
    "testdata/triangle_embedded.gltf",
])

cc_test(
    name = "mesh_asset_loader_tests",
    srcs = ["mesh_asset_loader_test.cpp"],
    data = [
        "testdata/textured_quad.obj",
        "testdata/triangle_embedded.gltf",
    ],
    deps = [
        ":render",
        "//shared/src:test_runfiles",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shader_path_resolver_tests",
    srcs = ["shader_path_resolver_test.cpp"],
    deps = [
        ":render",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shader_contract_tests",
    srcs = ["shader_contract_test.cpp"],
    data = [
        "//engine/src/render/shaders:fs_mesh.sc",
        "//engine/src/render/shaders:vs_mesh.sc",
        "//engine/src/render/shaders:vs_mesh_instanced.sc",
        "//engine/src/render/shaders:varying.def.sc",
        "//engine/src/render/shaders:varying_instanced.def.sc",
    ],
    deps = [
        "@abseil-cpp//absl/strings",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shader_binary_runtime_tests",
    srcs = ["shader_binary_runtime_test.cpp"],
    data = ["//engine/src/render/shaders:bgfx_shader_bins"],
    deps = [
        ":render",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "texture_loader_limits_tests",
    srcs = ["texture_loader_limits_test.cpp"],
    deps = [
        ":render",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "animation_playback_controller_tests",
    srcs = ["animation_playback_controller_test.cpp"],
    deps = [
        ":render",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "animated_gltf_tests",
    srcs = ["animated_gltf_test.cpp"],
    data = [
        "testdata/triangle_embedded.gltf",
    ],
    deps = [
        ":render",
        "//shared/src:test_runfiles",
        "@googletest//:gtest_main",
    ],
)
