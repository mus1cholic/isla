$input a_position, a_texcoord0
$input a_normal
$input i_data0, i_data1, i_data2, i_data3, i_data4
$output v_color0, v_normal, v_texcoord0, v_world_pos

#include <bgfx_shader.sh>

uniform vec4 u_time;

void main()
{
    // Reconstruct 4x4 model matrix from per-instance data.
    mat4 model;
    model[0] = i_data0;
    model[1] = i_data1;
    model[2] = i_data2;
    model[3] = i_data3;

    vec4 worldPos = mul(model, vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, worldPos);
    v_world_pos = worldPos.xyz;

    // u_time.x is fixed-step simulation time, u_time.y is renderer wall-clock elapsed time.
    float pulse = sin(u_time.x) * 0.1 + 0.9;
    // Per-instance color from i_data4 (packed into instance buffer by CPU).
    v_color0 = vec4(i_data4.rgb * pulse, i_data4.a);

    vec3 unpackedNormal = normalize((a_normal.xyz * 2.0) - 1.0);
    v_normal = normalize(mul(model, vec4(unpackedNormal, 0.0)).xyz);
    v_texcoord0 = a_texcoord0;
}
