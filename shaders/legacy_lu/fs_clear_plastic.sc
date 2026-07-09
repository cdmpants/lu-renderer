$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular, v_vertPos, v_diffuseExtra

#include <bgfx_shader.sh>

SAMPLERCUBE(s_luEnv, 1);

#include "shadow_common.sh"

uniform vec4 u_luLightDirFade;
uniform vec4 u_luCameraPos;
uniform vec4 u_luFogColor;
uniform vec4 u_luFogParams;

vec3 applyLuFog(vec3 rgb, vec3 worldPos)
{
    float denom = max(0.0001, u_luFogParams.y - u_luFogParams.x);
    float fogAmount = clamp((distance(u_luCameraPos.xyz, worldPos) - u_luFogParams.x) / denom, 0.0, 1.0) * u_luFogParams.z;
    return mix(rgb, u_luFogColor.rgb, fogAmount);
}

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 viewVector = normalize(u_luCameraPos.xyz - v_worldPos.xyz);
    float ldn = dot(u_luLightDirFade.xyz, normal);
    vec3 diffuse = vec3_splat(max(0.0, ldn)) + vec3(0.7, 0.71, 0.75);

    vec3 halfVector = normalize(viewVector + u_luLightDirFade.xyz);
    float hdn = pow(max(0.0, dot(halfVector, normal)), 120.0) * 4.19;
    vec3 specular = vec3_splat(max(0.0, ldn * hdn));

    vec4 reflColor = textureCube(s_luEnv, vec3(v_reflectVector.x, -v_reflectVector.y, v_reflectVector.z));
    reflColor.a = u_luLightDirFade.w;

    float fres = (0.1 + 0.9 * pow(max(0.0, 1.0 - abs(v_vdn)), 3.5)) * 0.5;
    reflColor.rgb += ((fres * diffuse * 2.5) + specular) * shadowVisibility(v_worldPos.xyz);
    reflColor.a *= (1.0 - v_vdn) * 0.5;

    reflColor.rgb = applyLuFog(reflColor.rgb, v_worldPos.xyz);
    gl_FragColor = clamp(reflColor, 0.0, 1.0);
}
