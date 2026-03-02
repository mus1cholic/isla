load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "shared",
    srcs = ["build_info.cpp"],
    visibility = ["//visibility:public"],
    deps = ["//shared/include/isla/shared:shared_headers"],
)

cc_library(
    name = "test_runfiles",
    testonly = True,
    srcs = ["test_runfiles.cpp"],
    hdrs = ["test_runfiles.hpp"],
    visibility = ["//visibility:public"],
    deps = ["@rules_cc//cc/runfiles"],
)

