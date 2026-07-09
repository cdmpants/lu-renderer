$input a_position, a_normal, a_texcoord0, a_color0
$output v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular

#include <bgfx_shader.sh>

uniform vec4 u_luLightDirFade;
uniform vec4 u_luLightColorShadow;
uniform vec4 u_luAmbient;
uniform vec4 u_luCameraPos;

void main()
{
    vec4 localPos = vec4(a_position, 1.0);
    vec4 worldPos = mul(u_model[0], localPos);
    vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    vec3 viewVector = normalize(u_luCameraPos.xyz - worldPos.xyz);

    float ldn = dot(u_luLightDirFade.xyz, normal);
    vec3 diffuse = max(vec3_splat(0.0), u_luLightColorShadow.rgb * ldn) + u_luAmbient.rgb;

    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = normal;
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_worldPos = worldPos;
    v_reflectVector = vec3(0.0, 0.0, 1.0);
    v_vdn = dot(viewVector, normal);
    v_diffuse = vec4(diffuse, ldn);
    v_specular = vec4(0.0, 0.0, 0.0, 1.0);
}
