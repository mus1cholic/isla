#pragma once

#include "isla/engine/math/math_types.hpp"

namespace isla::client {

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    Vec2 uv_a;
    Vec2 uv_b;
    Vec2 uv_c;
    Vec3 normal_a{};
    Vec3 normal_b{};
    Vec3 normal_c{};
    bool has_vertex_normals = false;
};

struct FrameInput {
    bool yaw_left = false;
    bool yaw_right = false;
    bool pitch_up = false;
    bool pitch_down = false;
    bool zoom_out = false;
    bool zoom_in = false;
    bool move_forward = false;
    bool move_backward = false;
    bool move_left = false;
    bool move_right = false;
    float look_yaw_delta = 0.0F;
    float look_pitch_delta = 0.0F;
};

} // namespace isla::client
