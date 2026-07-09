$input v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);
SAMPLERCUBE(s_luEnv, 1);

uniform vec4 u_luLightDirFade;
uniform vec4 u_luLightColorShadow;
uniform vec4 u_luSpecular;
uniform vec4 u_luCameraPos;
uniform vec4 u_luFogColor;
uniform vec4 u_luFogParams;
uniform vec4 u_luShaderFlags;
uniform vec4 u_materialEmissive;

vec3 applyLuFog(vec3 rgb, vec3 worldPos)
{
    float denom = max(0.0001, u_luFogParams.y - u_luFogParams.x);
    float fogAmount = clamp((distance(u_luCameraPos.xyz, worldPos) - u_luFogParams.x) / denom, 0.0, 1.0) * u_luFogParams.z;
    return mix(rgb, u_luFogColor.rgb, fogAmount);
}

float legoppCalculateFresnel(float vdn)
{
    return (0.1 + 0.9 * pow(max(0.0, 1.0 - min(1.0, abs(vdn))), 3.5)) * 0.8;
}

vec4 calculateSpecular(vec3 viewVector, float ldn, vec3 normal)
{
    vec3 halfVector = normalize(viewVector + u_luLightDirFade.xyz);
    float hdn = pow(max(0.0, dot(halfVector, normal)), 320.0);
    return vec4(ldn * hdn * u_luSpecular.rgb, 1.0);
}

void main()
{
    vec4 texColor = texture2D(s_diffuse, v_texcoord0);
    vec4 colorTextured = vec4(texColor.rgb, texColor.a);
    vec4 colorVertTextured = vec4(texColor.rgb * v_color0.rgb, texColor.a);
    vec4 colorWithTexture = (colorTextured * (1.0 - u_luShaderFlags.y)) + (colorVertTextured * u_luShaderFlags.y);
    vec4 color = (v_color0 * (1.0 - u_luShaderFlags.x)) + (colorWithTexture * u_luShaderFlags.x);

    vec3 normal = normalize(v_normal);
    vec3 viewVector = normalize(u_luCameraPos.xyz - v_worldPos.xyz);
    vec4 reflColor = v_vdn * textureCube(s_luEnv, vec3(v_reflectVector.x, -v_reflectVector.y, v_reflectVector.z)) * 0.05;
    float fres = legoppCalculateFresnel(v_vdn);
    vec4 specular = calculateSpecular(viewVector, v_diffuse.a, normal);

    vec3 baseColor = reflColor.rgb + specular.rgb + v_diffuse.rgb + (fres * color.rgb);
    vec3 texturedBase = reflColor.rgb + specular.rgb + ((v_diffuse.rgb + fres) * color.rgb);
    vec3 litColor = (baseColor * (1.0 - u_luShaderFlags.x)) + (texturedBase * u_luShaderFlags.x);
    litColor *= u_luLightColorShadow.a;

    float emissiveAmount = clamp(u_materialEmissive.r, 0.0, 1.0);
    vec3 rgb = mix(litColor, color.rgb, emissiveAmount);
    float alpha = color.a * u_luLightDirFade.w;

    if (u_luShaderFlags.w >= 0.0 && alpha < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(rgb, v_worldPos.xyz), alpha);
}
