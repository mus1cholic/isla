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
        "testdata/fail_no_skin.gltf",
        "testdata/fail_naive_timestamp.physics.json",
        "testdata/fail_layer_out_of_range.physics.json",
        "testdata/fail_schema_version.physics.json",
        "testdata/fail_top_level_array.physics.json",
        "testdata/pass_minimal.gltf",
        "testdata/pass_minimal.physics.json",
        "testdata/warn_malformed_joints_min_metadata.gltf",
    ],
)

py_test(
    name = "validate_motion_clips_test",
    srcs = [
        "validate_motion_clips.py",
        "validate_motion_clips_test.py",
    ],
    data = [
        "testdata/motion/pass_motion.gltf",
        "testdata/motion/pass_motion.motion.json",
        "testdata/motion/pass_contract_named.motion.gltf",
        "testdata/motion/pass_contract_named.motion.json",
        "testdata/motion/fail_motion_cubicspline.gltf",
        "testdata/motion/fail_motion_root_translation.gltf",
        "testdata/motion/fail_motion_missing_action.gltf",
        "testdata/motion/fail_motion_schema_version.motion.json",
        "testdata/motion/fail_motion_sidecar_missing_fields.motion.json",
        "testdata/motion/fail_motion_sidecar_malformed_clip.motion.json",
        "testdata/motion/fail_motion_sidecar_bad_timestamp.motion.json",
        "testdata/motion/fail_motion_sidecar_unknown_clip.motion.json",
        "testdata/motion/fail_motion_duplicate_clip_names.gltf",
        "testdata/motion/fail_motion_empty_clip_name.gltf",
        "testdata/motion/fail_motion_action_root_translation.gltf",
        "testdata/motion/pass_motion_action_allow_override.motion.json",
        "testdata/motion/warn_motion_root_no_minmax.gltf",
    ],
)

py_test(
    name = "validate_motion_clips_smoke_test",
    srcs = [
        "validate_motion_clips.py",
        "validate_motion_clips_smoke_test.py",
    ],
    data = [
        "testdata/motion/pass_motion.gltf",
        "testdata/motion/pass_motion.motion.json",
    ],
)
