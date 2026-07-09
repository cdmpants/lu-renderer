$input a_position, a_normal, a_texcoord0, a_color0
$output v_normal, v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform vec4 u_luEffectTime;
uniform vec4 u_luUvMotion1;

void main()
{
    vec4 localPos = vec4(a_position, 1.0);
    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = normalize(mul(u_modelView, vec4(a_normal, 0.0)).xyz);
    v_texcoord0 = a_texcoord0 + (u_luUvMotion1.xy * u_luEffectTime.x * u_luEffectTime.y);
    v_color0 = a_color0;
}
