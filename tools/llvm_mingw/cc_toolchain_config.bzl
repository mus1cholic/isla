"""Experimental cc_toolchain config for Linux-hosted llvm-mingw Windows cross-builds."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "artifact_name_pattern",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")


def _impl(ctx):
    tool_paths = [
        tool_path(name = "ar", path = "bin/llvm-ar-wrapper"),
        tool_path(name = "cpp", path = "bin/clang-wrapper"),
        tool_path(name = "dwp", path = "bin/llvm-dwp-wrapper"),
        tool_path(name = "gcc", path = "bin/clangxx-wrapper"),
        tool_path(name = "gcov", path = "/bin/false"),
        tool_path(name = "ld", path = "bin/clangxx-wrapper"),
        tool_path(name = "nm", path = "bin/llvm-nm-wrapper"),
        tool_path(name = "objcopy", path = "bin/llvm-objcopy-wrapper"),
        tool_path(name = "objdump", path = "bin/llvm-objdump-wrapper"),
        tool_path(name = "strip", path = "bin/llvm-strip-wrapper"),
    ]

    all_compile_actions = [
        ACTION_NAMES.assemble,
        ACTION_NAMES.preprocess_assemble,
        ACTION_NAMES.linkstamp_compile,
        ACTION_NAMES.c_compile,
        ACTION_NAMES.cpp_compile,
        ACTION_NAMES.cpp_header_parsing,
        ACTION_NAMES.cpp_module_compile,
        ACTION_NAMES.cpp_module_codegen,
        ACTION_NAMES.clif_match,
        ACTION_NAMES.lto_backend,
        ACTION_NAMES.cpp_module_deps_scanning,
    ]

    cxx_compile_actions = [
        ACTION_NAMES.cpp_compile,
        ACTION_NAMES.cpp_header_parsing,
        ACTION_NAMES.cpp_module_compile,
        ACTION_NAMES.cpp_module_codegen,
        ACTION_NAMES.clif_match,
        ACTION_NAMES.lto_backend,
        ACTION_NAMES.cpp_module_deps_scanning,
        ACTION_NAMES.linkstamp_compile,
    ]

    link_actions = [
        ACTION_NAMES.cpp_link_executable,
        ACTION_NAMES.cpp_link_dynamic_library,
        ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ]

    features = [
        feature(
            name = "default_compile_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-DWIN32",
                                "-D_WINDOWS",
                            ],
                        ),
                    ],
                ),
                flag_set(
                    actions = [ACTION_NAMES.c_compile],
                    flag_groups = [
                        flag_group(
                            # Keep gcc=clang++ for C++ link behavior, but force C
                            # language mode for C compilation actions.
                            flags = [
                                "-x",
                                "c",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "default_cxx_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = cxx_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-std=c++20",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "default_link_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = link_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-fuse-ld=lld",
                                "-ldbghelp",
                            ],
                        ),
                    ],
                ),
            ],
        ),
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "llvm_mingw_x86_64_windows",
        host_system_name = "local",
        target_system_name = "x86_64-w64-windows-gnu",
        target_cpu = "x64_windows",
        target_libc = "ucrt",
        compiler = "clang",
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        cxx_builtin_include_directories = ctx.attr.cxx_builtin_include_directories,
        features = features,
        artifact_name_patterns = [
            artifact_name_pattern(
                category_name = "static_library",
                prefix = "",
                extension = ".lib",
            ),
            artifact_name_pattern(
                category_name = "dynamic_library",
                prefix = "dynamic_",
                extension = ".dll",
            ),
            artifact_name_pattern(
                category_name = "executable",
                prefix = "",
                extension = ".exe",
            ),
        ],
    )


llvm_mingw_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "cxx_builtin_include_directories": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
