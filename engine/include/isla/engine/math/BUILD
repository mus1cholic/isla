load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "math_types",
    hdrs = ["math_types.hpp"],
    strip_include_prefix = "/engine/include",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "math",
    hdrs = [
        "constants.hpp",
        "mat4.hpp",
        "quat.hpp",
        "render_math.hpp",
        "transform.hpp",
    ],
    strip_include_prefix = "/engine/include",
    visibility = ["//visibility:public"],
    deps = [":math_types"],
)
