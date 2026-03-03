$input a_position, a_texcoord0
$input a_normal
$input a_indices, a_weight
$output v_color0, v_normal, v_texcoord0, v_world_pos

#include <bgfx_shader.sh>

uniform vec4 u_time;
uniform vec4 u_object_color;
uniform mat4 u_joint_palette[64];

void main()
{
    int i0 = int(a_indices.x);
    int i1 = int(a_indices.y);
    int i2 = int(a_indices.z);
    int i3 = int(a_indices.w);

    mat4 skin =
        (u_joint_palette[i0] * a_weight.x) + (u_joint_palette[i1] * a_weight.y) +
        (u_joint_palette[i2] * a_weight.z) + (u_joint_palette[i3] * a_weight.w);

    vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));
    vec3 unpackedNormal = normalize((a_normal.xyz * 2.0) - 1.0);
    vec3 skinnedNormal = normalize(mul(skin, vec4(unpackedNormal, 0.0)).xyz);

    vec4 worldPos = mul(u_model[0], skinnedPos);
    gl_Position = mul(u_viewProj, worldPos);
    v_world_pos = worldPos.xyz;

    float pulse = sin(u_time.x) * 0.1 + 0.9;
    v_color0 = vec4(u_object_color.rgb * pulse, u_object_color.a);
    v_normal = normalize(mul(u_model[0], vec4(skinnedNormal, 0.0)).xyz);
    v_texcoord0 = a_texcoord0;
}
