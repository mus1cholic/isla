load("@rules_cc//cc:defs.bzl", "cc_binary")

alias(
    name = "bgfx",
    actual = "@bgfx_upstream//:bgfx",
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "shaderc_dx11",
    # TODO(tower_rpg): Replace the non-Windows stub path with a dedicated
    # Windows-only shader compilation workflow/target graph so Linux CI never
    # needs to build or invoke DX11/HLSL shaderc.
    srcs = select({
        "@bazel_tools//src/conditions:windows": [
            "shaderc_src/3rdparty/fcpp/cpp.h",
            "shaderc_src/3rdparty/fcpp/cppadd.h",
            "shaderc_src/3rdparty/fcpp/cppdef.h",
            "shaderc_src/3rdparty/fcpp/cpp1.c",
            "shaderc_src/3rdparty/fcpp/cpp2.c",
            "shaderc_src/3rdparty/fcpp/cpp3.c",
            "shaderc_src/3rdparty/fcpp/cpp4.c",
            "shaderc_src/3rdparty/fcpp/cpp5.c",
            "shaderc_src/3rdparty/fcpp/cpp6.c",
            "shaderc_src/3rdparty/fcpp/fpp.h",
            "shaderc_src/tools/shaderc/shaderc.cpp",
            "shaderc_src/tools/shaderc/shaderc_hlsl.cpp",
            "shaderc_src/tools/shaderc/shaderc.h",
            "shaderc_src/tools/shaderc/shaderc_stubs.cpp",
            "shaderc_src/src/vertexlayout.h",
        ],
        "//conditions:default": ["shaderc_stub_main.cpp"],
    }),
    copts = select({
        "@bazel_tools//src/conditions:windows": [
            "/wd4055",
            "/wd4244",
            "/wd4701",
            "/wd4706",
        ],
        "//conditions:default": [],
    }),
    defines = select({
        "@bazel_tools//src/conditions:windows": [
            "NINCLUDE=64",
            "NWORK=65536",
            "NBUFF=65536",
            "OLD_PREPROCESSOR=0",
            "SHADERC_CONFIG_HLSL=1",
        ],
        "//conditions:default": [],
    }),
    includes = select({
        "@bazel_tools//src/conditions:windows": [
            "shaderc_src/3rdparty/fcpp",
            "shaderc_src/src",
            "shaderc_src/tools/shaderc",
        ],
        "//conditions:default": [],
    }),
    linkopts = select({
        "@bazel_tools//src/conditions:windows": ["psapi.lib"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = select({
        "@bazel_tools//src/conditions:windows": [
            ":bgfx",
            "@bx_upstream//:bx",
        ],
        "//conditions:default": [],
    }),
)
