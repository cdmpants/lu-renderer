$input a_position, a_normal, a_texcoord0, a_color0
$output v_normal, v_texcoord0, v_color0, v_worldPos

#include <bgfx_shader.sh>

void main()
{
    vec4 localPos = vec4(a_position, 1.0);
    vec4 worldPos = mul(u_model[0], localPos);
    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = a_normal;
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_worldPos = worldPos;
}
