$input a_position, a_texcoord0
$input a_normal
$output v_color0, v_normal, v_texcoord0, v_world_pos

#include <bgfx_shader.sh>

uniform vec4 u_time;
uniform vec4 u_object_color;

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    // u_time.x is fixed-step simulation time, u_time.y is renderer wall-clock elapsed time.
    float pulse = sin(u_time.x) * 0.1 + 0.9;
    v_color0 = vec4(u_object_color.rgb * pulse, u_object_color.a);
    v_world_pos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    vec3 unpackedNormal = normalize((a_normal.xyz * 2.0) - 1.0);
    v_normal = normalize(mul(u_model[0], vec4(unpackedNormal, 0.0)).xyz);
    v_texcoord0 = a_texcoord0;
}
