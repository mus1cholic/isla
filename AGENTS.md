# Project Isla Agent Guide

Concise but detailed reference for contributors working across the `isla` C++ desktop-mate and project-isla inspired framework repository. Improve code when you touch it; avoid one-off patterns.

## Tech Stack (by surface)
- **Framework Core**: C++20, Bazel, GoogleTest, Abseil.
- **Client (Desktop)**: SDL3 backend, Win32 API (`win32_layered_overlay`), C++.
- **Engine (Rendering & Math)**: bgfx, bimg, bx, glm, cgltf, saba (PMX parsing).
- **Server (Memory & AI Gateway)**: `nlohmann/json`, Boost.Beast, AI Gateway integration.
- **Build & Tools**: Bazel (via `MODULE.bazel`), Python, MinGW cross-compilation (`tools/llvm_mingw`).

## Structure & Responsibilities
- **Apps**
  - `client/`: The standalone desktop-mate application entry point. Contains `main.cpp`, `client_app`, and the Windows-specific transparent overlay logic.
- **Engine Internals** (`engine/`)
  - **Render**: `engine/src/render` and `engine/include/isla/engine/render`. Manages the graphics pipeline via bgfx. Crucially uses a Left-Handed coordinate system; assets (like glTF) are converted from Right-Handed dynamically. Relies on counter-clockwise face culling.
  - **Math**: `engine/src/math` and `engine/include/isla/engine/math`. Core mathematics utilities relying on `glm`.
- **Server Internals** (`server/`)
  - **Memory System**: `server/memory`. Implements a tri-layered biomimetic memory architecture: Working Memory (L1 persistent cache), Mid-Term Memory (fast-write staging area with Tier 1/2/3 compactions), and Long-Term Memory (a dual-database Knowledge Graph + Episodic Vector DB). Key features include Evidence Accumulation for KG conflicts and cross-encoder re-ranking.
  - **AI Gateway**: `server/src`. Provides the logic for communicating with AI models, acting as a bridge via `ai_gateway_stub_responder` and integrating with the shared AI gateway protocol.
- **Shared** (`shared/`)
  - Shared IPC and protocol logic (`ai_gateway_protocol`). Used across client/server surfaces for robust communication.
- **Dependency Management & Build**
  - Uses Bazel (Bzlmod via `MODULE.bazel`). Dependencies include `sdl3_source`, `bgfx_upstream`, `bx_upstream`, `bimg_upstream`, etc.
  - Build and linting facilitated by `.github/workflows` and `.clang-format`/`.clang-tidy`. `compile_commands.json` generated for IDE support.

## Key Path Index (what lives where)
- `client/src`: Desktop entry points and SDL runtime integration.
  - `win32_layered_overlay.cpp`: Crucial for transparent desktop buddy rendering.
- `engine/include/isla/engine`: Core engine headers (Render, Math, Bootstrap).
- `engine/src/render`: Implementation of Left-Handed rendering and conversion from Right-Handed coordinates.
- `server/memory`: Tri-layered memory system (Working, Mid-Term, Long-Term), Persistent Cache, Evidence Accumulation (KG conflict resolution), and Episodic Vector DB.
- `server/src`: Gateway server implementation (e.g., `ai_gateway_stub_responder`).
- `shared/src`: Shared protocol models (`ai_gateway_protocol`).
- `tools/pmx`: Scripting and tools for MMD (PMX) asset formats.
- `docs/`: System documentation (e.g., `COORDINATE_SYSTEMS.md`, `memory-planning.md`, PMX to GLTF plans).
- `MODULE.bazel`: The definitive guide for external dependencies and toolchains.

## Commands (Bazel)
> Use Bazel commands from the repository root. Examples below are common workflows.

- **Build / Compile**
  - Build the client: `bazel build //client/src:isla_client`
  - Build everything: `bazel build //...`
- **Unit tests (GoogleTest)**
  - Run all tests: `bazel test //...`
  - Run tests for a specific module: `bazel test //client/src:all` or `bazel test //server/memory/...`
- **Run the Application**
  - `bazel run //client/src:isla_client`
- **Linting & Formatting**
  - Use `.clang-format` and `.clang-tidy` rules for style conformance.

## Development Practices
- **Memory & Architectures**: When touching the `server/memory`, adhere to the documented psychological architectures (e.g., Working/Mid-Term/Long-Term layers, dynamic evidence accumulation over continuous math for KG conflicts, and spreading activation retrieval filtered by a local cross-encoder re-ranker). Review `docs/memory/memory-planning.md` thoroughly before making changes.
- **Rendering & Math**: Be explicitly aware of the Left-Handed vs Right-Handed coordinate shift when updating `model_renderer.cpp` or loading models. Document changes heavily and update `COORDINATE_SYSTEMS.md` if fundamental rules change.
- **Serialization**: Use `nlohmann::json`. Rely on native support (like `std::optional` built-in support) rather than maintaining custom `adl_serializer` specializations unless strictly necessary.
- **C++ Idioms**: Favor modern C++ functional patterns and RAII.
- **Refactoring**: If refactoring is required, keep changes scoped. Improve legacy code when touching nearby lines. Update the associated `docs/` files alongside the changes.
- **Cross-Compilation**: Ensure new dependencies do not break the MinGW cross-build pipeline defined in `tools/llvm_mingw`.
- **Search Scope**: When searching or exploring the repository, ALWAYS ignore folders beginning with `bazel-` (e.g., `bazel-bin`, `bazel-out`, `bazel-isla`, etc.) to prevent exceeding context token limits.

## Conventions
- File names: `snake_case` (e.g., `client_app.cpp`, `memory_timestamp_utils.hpp`).
- Structural patterns: Use `namespace isla::[component]` where appropriate.
- Add clear, concise comments for math, OS-interaction (especially Win32 API calls), algorithm, shared, and architectural functions.
- When fixing an issue via workaround, add a `// NOTICE:` comment explaining why, the root cause, and source context (e.g., if a library bug forces a specific pattern).

## PR / Workflow Tips
- Summarize changes, how the changes were tested (e.g., which Bazel targets), and follow-ups.
- Keep changes scoped.
- Ensure all relevant GoogleTests pass.
- Update `docs/` documentation for architectural changes.
