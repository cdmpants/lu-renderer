$input a_position, a_normal, a_texcoord0, a_color0
$output v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular

#include <bgfx_shader.sh>

uniform vec4 u_materialDiffuse;
uniform vec4 u_luLightDirFade;
uniform vec4 u_luLightColorShadow;
uniform vec4 u_luAmbient;
uniform vec4 u_luUpperHemi;
uniform vec4 u_luLowerHemi;
uniform vec4 u_luSpecular;
uniform vec4 u_luCameraPos;
uniform vec4 u_luShaderFlags;

vec3 calculateHemiLightInfluence(vec3 normal)
{
    float shiftedY = normal.y + 1.0;
    return ((u_luUpperHemi.rgb * shiftedY) + (u_luLowerHemi.rgb * (2.0 - shiftedY))) * 0.5;
}

vec4 calculateSpecular(vec3 viewVector, float ldn, vec3 normal, float power, float scale)
{
    vec3 halfVector = normalize(viewVector + u_luLightDirFade.xyz);
    float hdn = pow(max(0.0, dot(halfVector, normal)), power);
    return vec4(ldn * hdn * u_luSpecular.rgb * scale, 1.0);
}

void main()
{
    vec4 localPos = vec4(a_position, 1.0);
    vec4 worldPos = mul(u_model[0], localPos);
    vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    vec3 viewVector = normalize(u_luCameraPos.xyz - worldPos.xyz);
    vec4 meshColor = (u_materialDiffuse * (1.0 - u_luShaderFlags.y)) + (a_color0 * u_luShaderFlags.y);

    float ldn = dot(u_luLightDirFade.xyz, normal);
    vec3 hemiLight = calculateHemiLightInfluence(normal);
    vec3 diffuse = max(0.0, ldn) * hemiLight * u_luLightColorShadow.rgb + vec3_splat(1.0);
    diffuse *= (vec3_splat(1.0) * (1.0 - u_luShaderFlags.z)) + (meshColor.rgb * u_luShaderFlags.z);

    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = normal;
    v_texcoord0 = a_texcoord0;
    v_color0 = meshColor;
    v_worldPos = worldPos;
    v_reflectVector = reflect(viewVector, normal);
    v_vdn = dot(viewVector, normal);
    v_diffuse = vec4(diffuse, ldn);
    v_specular = calculateSpecular(viewVector, ldn, normal, 120.0, 4.19);
}
