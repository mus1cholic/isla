filegroup(
    name = "bgfx_shader_bins",
    srcs = [
        ":compile_fs_mesh_dx11",
        ":compile_vs_mesh_dx11",
        ":compile_vs_mesh_instanced_dx11",
    ],
    visibility = ["//visibility:public"],
)

genrule(
    name = "compile_vs_mesh_dx11",
    srcs = [
        "include/bgfx_shader.sh",
        "varying.def.sc",
        "vs_mesh.sc",
    ],
    outs = ["dx11/vs_mesh.bin"],
    cmd_bat = "$(location //third_party/bgfx:shaderc_dx11) " +
              "--type vertex " +
              "--platform windows " +
              "-p s_5_0 " +
              "-O 3 " +
              "--varyingdef \"$(location varying.def.sc)\" " +
              "-i engine/src/render/shaders/include " +
              "-f \"$(location vs_mesh.sc)\" " +
              "-o \"$@\"",
    cmd_bash = "printf 'isla-dx11-shader-stub\\n' > \"$@\"",
    tools = ["//third_party/bgfx:shaderc_dx11"],
)

genrule(
    name = "compile_fs_mesh_dx11",
    srcs = [
        "include/bgfx_shader.sh",
        "varying.def.sc",
        "fs_mesh.sc",
    ],
    outs = ["dx11/fs_mesh.bin"],
    cmd_bat = "$(location //third_party/bgfx:shaderc_dx11) " +
              "--type fragment " +
              "--platform windows " +
              "-p s_5_0 " +
              "-O 3 " +
              "--varyingdef \"$(location varying.def.sc)\" " +
              "-i engine/src/render/shaders/include " +
              "-f \"$(location fs_mesh.sc)\" " +
              "-o \"$@\"",
    cmd_bash = "printf 'isla-dx11-shader-stub\\n' > \"$@\"",
    tools = ["//third_party/bgfx:shaderc_dx11"],
)

genrule(
    name = "compile_vs_mesh_instanced_dx11",
    srcs = [
        "include/bgfx_shader.sh",
        "varying_instanced.def.sc",
        "vs_mesh_instanced.sc",
    ],
    outs = ["dx11/vs_mesh_instanced.bin"],
    cmd_bat = "$(location //third_party/bgfx:shaderc_dx11) " +
              "--type vertex " +
              "--platform windows " +
              "-p s_5_0 " +
              "-O 3 " +
              "--varyingdef \"$(location varying_instanced.def.sc)\" " +
              "-i engine/src/render/shaders/include " +
              "-f \"$(location vs_mesh_instanced.sc)\" " +
              "-o \"$@\"",
    cmd_bash = "printf 'isla-dx11-shader-stub\\n' > \"$@\"",
    tools = ["//third_party/bgfx:shaderc_dx11"],
)

exports_files([
    "fs_mesh.sc",
    "include/bgfx_shader.sh",
    "varying.def.sc",
    "varying_instanced.def.sc",
    "vs_mesh.sc",
    "vs_mesh_instanced.sc",
])
