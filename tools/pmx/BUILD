load("@rules_python//python:defs.bzl", "py_test")

py_test(
    name = "validate_converted_gltf_test",
    srcs = [
        "validate_converted_gltf.py",
        "validate_converted_gltf_test.py",
    ],
    data = [
        "testdata/fail_cubicspline.gltf",
        "testdata/fail_cubicspline.physics.json",
        "testdata/pass_minimal.gltf",
        "testdata/pass_minimal.physics.json",
    ],
)
