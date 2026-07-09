$input a_position, a_normal, a_texcoord0, a_color0
$output v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_objectPos

#include <bgfx_shader.sh>

uniform vec4 u_materialDiffuse;
uniform vec4 u_luLightDirFade;
uniform vec4 u_luCameraPos;
uniform vec4 u_luShaderFlags;

void main()
{
    vec4 localPos = vec4(a_position, 1.0);
    vec4 worldPos = mul(u_model[0], localPos);
    vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    float ldn = saturate(dot(u_luLightDirFade.xyz, normal));
    float modLdn = pow(ldn, 4.0);
    vec3 metalDiffuse = vec3_splat(0.7);
    vec3 metalAmbient = vec3_splat(0.3);

    vec4 shaderColor = (a_color0 * u_luShaderFlags.y) +
                       (u_materialDiffuse * u_luShaderFlags.z) +
                       (vec4(1.0, 1.0, 1.0, 1.0) * (1.0 - max(u_luShaderFlags.y, u_luShaderFlags.z)));
    vec4 diffuse = vec4((max(vec3_splat(0.0), metalDiffuse * modLdn) + metalAmbient) * shaderColor.rgb,
                        shaderColor.a * u_luLightDirFade.w);
    vec3 viewVector = normalize(u_luCameraPos.xyz - worldPos.xyz);

    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = normal;
    v_texcoord0 = a_texcoord0;
    v_color0 = shaderColor;
    v_worldPos = worldPos;
    v_reflectVector = reflect(viewVector, normal);
    v_vdn = ldn;
    v_diffuse = diffuse;
    v_objectPos = a_position;
}
