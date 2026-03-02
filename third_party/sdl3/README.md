# SDL3 for Bazel

SDL3 is wired via Bzlmod in `MODULE.bazel` as a pinned source dependency:

- Release: `SDL3-3.4.0.tar.gz`
- Source: `https://github.com/libsdl-org/SDL/releases/tag/release-3.4.0`
- SHA-256 pinned in `MODULE.bazel`

`//third_party/sdl3:sdl3` is an alias to a `rules_foreign_cc` CMake build target:

- Source repo: `@sdl3_source//:all_srcs`
- Build rule: `//third_party/sdl3:sdl3_build`

Build tool/dependency expectations:

- Windows: CMake + Ninja + Git Bash available in the build environment. `.bazelrc` configures Bazel to use `C:/Program Files/Git/usr/bin/bash.exe`.
- Ubuntu/Linux: CMake + Ninja + SDL3 native build dependencies (X11/Wayland/audio dev packages).

Current project status:

- `//client/src:isla_client` is marked `manual` and not built by `//...` yet.
- Server and unit tests do not require SDL3.

