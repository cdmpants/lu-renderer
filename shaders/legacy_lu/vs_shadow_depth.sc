$input a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform vec4 u_luEffectTime;
uniform vec4 u_luUvMotion1;

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0 + (u_luUvMotion1.xy * u_luEffectTime.x * u_luEffectTime.y);
    v_color0 = a_color0;
}
