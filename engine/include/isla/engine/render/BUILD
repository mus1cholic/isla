load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "render",
    hdrs = [
        "animated_gltf.hpp",
        "animation_playback_controller.hpp",
        "mesh_asset_loader.hpp",
        "model_renderer.hpp",
        "model_renderer_skinning_utils.hpp",
        "overlay_transparency.hpp",
        "pmx_native_runtime.hpp",
        "pmx_physics_sidecar.hpp",
        "pmx_texture_remap_sidecar.hpp",
        "render_types.hpp",
        "render_world.hpp",
        "renderer_backend.hpp",
    ],
    strip_include_prefix = "/engine/include",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "render_runtime",
    visibility = ["//visibility:public"],
    deps = [
        ":render",
        "//engine/src/render:render",
        "//engine/src/render:pmx_native_runtime",
    ],
)
