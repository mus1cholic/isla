load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands_editor",
    targets = [
        "//client/...",
        "//engine/...",
        "//server/...",
        "//shared/...",
    ],
    exclude_headers = "all",
    exclude_external_sources = True,
)
