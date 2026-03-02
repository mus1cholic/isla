$input v_color0, v_normal, v_texcoord0, v_world_pos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_dir_light_dir;
uniform vec4 u_dir_light_color;
uniform vec4 u_ambient_color;
uniform vec4 u_camera_pos;
uniform vec4 u_spec_params;

void main()
{
    vec3 normal = -normalize(v_normal);
    vec3 light_dir = normalize(u_dir_light_dir.xyz);
    vec3 view_dir = normalize(u_camera_pos.xyz - v_world_pos);
    vec3 half_dir = normalize(light_dir + view_dir);
    float ndotl = max(dot(normal, light_dir), 0.0);
    float shininess = max(u_spec_params.y, 1.0);
    float spec_intensity = max(u_spec_params.x, 0.0);
    float spec_term =
        (ndotl > 0.0) ? pow(max(dot(normal, half_dir), 0.0), shininess) * spec_intensity : 0.0;
    vec3 ambient = u_ambient_color.rgb;
    vec3 diffuse = u_dir_light_color.rgb * ndotl;
    vec3 specular = u_dir_light_color.rgb * spec_term;

    vec4 texColor = texture2D(s_texColor, v_texcoord0);
    gl_FragColor =
        vec4(v_color0.rgb * texColor.rgb * (ambient + diffuse + specular), v_color0.a * texColor.a);
}
