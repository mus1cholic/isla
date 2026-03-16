load("@rules_cc//cc:defs.bzl", "cc_binary")

filegroup(
    name = "prompt_assets",
    srcs = glob(["*.txt"]),
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "embed_prompts_tool",
    srcs = ["embed_prompts_tool.cpp"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "system_prompt_asset",
    srcs = ["system_prompt.txt"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "mid_term_compactor_system_prompt_asset",
    srcs = ["mid_term_compactor_system_prompt.txt"],
    visibility = ["//visibility:public"],
)
