load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "math_core",
    srcs = [
        "mat4.cpp",
        "quat.cpp",
        "vector_math.cpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//engine/include/isla/engine/math"],
)

cc_library(
    name = "math",
    visibility = ["//visibility:public"],
    deps = [
        ":math_core",
        "//engine/include/isla/engine/math",
    ],
)

