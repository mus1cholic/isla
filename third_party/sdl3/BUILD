load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

config_setting(
    name = "windows_gnu",
    values = {"define": "isla_windows_gnu=true"},
)

config_setting(
    name = "windows_msvc",
    values = {"define": "isla_windows_gnu=false"},
)

cmake(
    name = "sdl3_build",
    lib_source = "@sdl3_source//:all_srcs",
    # Force Ninja so Linux-hosted cross builds do not default to Unix Makefiles
    # and pull in rules_foreign_cc's Windows make.exe tool path.
    generate_args = ["-GNinja"],
    cache_entries = select({
        ":windows_gnu": {
            "SDL_SHARED": "OFF",
            "SDL_STATIC": "ON",
            "SDL_TESTS": "OFF",
            "SDL_TRAY": "OFF",
            "CMAKE_SYSTEM_NAME": "Windows",
            "CMAKE_SYSTEM_PROCESSOR": "x86_64",
            "CMAKE_C_COMPILER": "tools/llvm_mingw/bin/clang-wrapper",
            "CMAKE_CXX_COMPILER": "tools/llvm_mingw/bin/clangxx-wrapper",
            # Linux-hosted Windows cross builds should not require Linux desktop
            # window system development packages.
            "SDL_UNIX_CONSOLE_BUILD": "ON",
            "SDL_X11": "OFF",
            "SDL_WAYLAND": "OFF",
            # Avoid host pkg-config probing in this lane; rules_foreign_cc's
            # pkg-config wrapper path can be non-executable in sandboxed CI.
            "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "ON",
        },
        "//conditions:default": {
            "SDL_SHARED": "OFF",
            "SDL_STATIC": "ON",
            "SDL_TESTS": "OFF",
        },
    }),
    out_include_dir = "include",
    out_static_libs = select({
        ":windows_gnu": ["libSDL3.a"],
        ":windows_msvc": ["SDL3-static.lib"],
        "@bazel_tools//src/conditions:linux_x86_64": ["libSDL3.a"],
        "//conditions:default": ["libSDL3.a"],
    }),
    linkopts = select({
        ":windows_gnu": [
            "-luser32",
            "-lgdi32",
            "-lwinmm",
            "-lole32",
            "-loleaut32",
            "-lshell32",
            "-lsetupapi",
            "-lcfgmgr32",
            "-limm32",
            "-lversion",
            "-luuid",
        ],
        ":windows_msvc": [
            "user32.lib",
            "gdi32.lib",
            "winmm.lib",
            "ole32.lib",
            "oleaut32.lib",
            "shell32.lib",
            "setupapi.lib",
            "cfgmgr32.lib",
            "imm32.lib",
            "version.lib",
            "uuid.lib",
        ],
        "//conditions:default": [],
    }),
)

alias(
    name = "sdl3",
    actual = ":sdl3_build",
    visibility = ["//visibility:public"],
)

