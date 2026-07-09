$input v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_objectPos

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);
SAMPLERCUBE(s_luEnv, 1);
SAMPLER2D(s_dark, 2);

#include "shadow_common.sh"

uniform vec4 u_luLightDirFade;
uniform vec4 u_luSpecular;
uniform vec4 u_luCameraPos;
uniform vec4 u_luFogColor;
uniform vec4 u_luFogParams;
uniform vec4 u_luShaderFlags;

vec3 applyLuFog(vec3 rgb, vec3 worldPos)
{
    float denom = max(0.0001, u_luFogParams.y - u_luFogParams.x);
    float fogAmount = clamp((distance(u_luCameraPos.xyz, worldPos) - u_luFogParams.x) / denom, 0.0, 1.0) * u_luFogParams.z;
    return mix(rgb, u_luFogColor.rgb, fogAmount);
}

void main()
{
    vec4 texColor = texture2D(s_diffuse, v_texcoord0);
    vec4 reflColor = textureCube(s_luEnv, v_reflectVector);
    vec3 normalizedPos = normalize(v_objectPos);
    vec2 noiseCoord = vec2(normalizedPos.x * (normalizedPos.x - normalizedPos.z), v_objectPos.y / 5.0);
    vec4 noiseColor = texture2D(s_dark, noiseCoord);

    vec3 viewVec = normalize(u_luCameraPos.xyz - v_worldPos.xyz);
    vec3 halfVec = normalize(viewVec + u_luLightDirFade.xyz);
    float specIntensity = pow(max(0.0, dot(halfVec, v_normal)), 100.0 * noiseColor.a) * 0.5;
    vec3 specColor = u_luSpecular.rgb * specIntensity * v_vdn;
    float reflIntensity = 1.0 - ((v_vdn / 2.0) + 0.5);

    vec4 noTextureColor = v_diffuse;
    vec4 textureColor = v_diffuse * texColor;
    vec4 vertColorTextureColor = v_diffuse;
    vec4 outColor = mix(noTextureColor, mix(textureColor, vertColorTextureColor, u_luShaderFlags.y), u_luShaderFlags.x);
    vec3 reflectionTint = mix(v_color0.rgb, mix(texColor.rgb, v_color0.rgb * texColor.rgb, u_luShaderFlags.y), u_luShaderFlags.x);
    vec3 coloredRefl = reflColor.rgb * noiseColor.rgb * (reflColor.a - reflIntensity) * reflectionTint * 2.0;
    outColor.rgb = (outColor.rgb * shadowVisibilityWithNormal(v_worldPos.xyz, normalize(v_normal))) + coloredRefl + specColor;
    gl_FragColor = vec4(applyLuFog(outColor.rgb, v_worldPos.xyz), outColor.a);
}
