$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular, v_vertPos, v_diffuseExtra

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);
SAMPLERCUBE(s_luEnv, 1);

#include "shadow_common.sh"

uniform vec4 u_luLightDirFade;
uniform vec4 u_luLightColorShadow;
uniform vec4 u_luSpecular;
uniform vec4 u_luCameraPos;
uniform vec4 u_luFogColor;
uniform vec4 u_luFogParams;
uniform vec4 u_luShaderFlags;
uniform vec4 u_luVariantFlags;
uniform vec4 u_luPbrParams;
uniform vec4 u_luReflectionParams;
uniform vec4 u_materialEmissive;

#define LEGOPP_VARIANT_SUPEREMISSIVE 4.0

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

float pbrDistributionGgx(float ndoth, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (ndoth * ndoth) * (a2 - 1.0) + 1.0;
    return a2 / max(0.0001, 3.14159265 * denom * denom);
}

float pbrGeometrySchlickGgx(float ndotx, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return ndotx / max(0.0001, ndotx * (1.0 - k) + k);
}

vec3 pbrFresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (vec3_splat(1.0) - f0) * pow(1.0 - cosTheta, 5.0);
}

vec3 calculatePbrColor(vec3 baseColor, vec3 normal, vec3 viewVector, vec3 reflectVector, vec3 diffuseLight, vec3 worldPos, float reflectionScale)
{
    float roughness = clamp(u_luPbrParams.y, 0.04, 1.0);
    float metallic = clamp(u_luPbrParams.z, 0.0, 1.0);
    float specularIntensity = max(0.0, u_luPbrParams.w);
    vec3 lightVector = normalize(u_luLightDirFade.xyz);
    vec3 halfVector = normalize(viewVector + lightVector);
    float ndotv = max(0.001, dot(normal, viewVector));
    float ndotl = max(0.0, dot(normal, lightVector));
    float ndoth = max(0.0, dot(normal, halfVector));
    float hdotv = max(0.0, dot(halfVector, viewVector));

    vec3 f0 = mix(vec3_splat(0.04 * specularIntensity), baseColor, metallic);
    vec3 fresnel = pbrFresnelSchlick(hdotv, f0);
    float distribution = pbrDistributionGgx(ndoth, roughness);
    float geometry = pbrGeometrySchlickGgx(ndotv, roughness) * pbrGeometrySchlickGgx(ndotl, roughness);
    vec3 specular = (distribution * geometry * fresnel) / max(0.001, 4.0 * ndotv * ndotl);
    vec3 diffuse = (vec3_splat(1.0) - fresnel) * (1.0 - metallic) * baseColor * 0.31830989;
    vec3 direct = (diffuse + specular) * u_luLightColorShadow.rgb * ndotl;

    vec3 reflectDir = vec3(reflectVector.x, -reflectVector.y, reflectVector.z);
    vec3 envColor = textureCube(s_luEnv, reflectDir).rgb;
    vec3 envFresnel = pbrFresnelSchlick(ndotv, f0);
    vec3 envSpecular = envColor * envFresnel * reflectionScale * u_luReflectionParams.y * specularIntensity * mix(1.0, 0.35, roughness);
    vec3 ambient = baseColor * diffuseLight * (1.0 - metallic) * 0.25;
    return (direct + ambient + envSpecular) * u_luLightColorShadow.a * shadowVisibilityWithNormal(worldPos, normal);
}

float isVariant(float variant)
{
    return 1.0 - step(0.5, abs(u_luVariantFlags.x - variant));
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
    float lowSource = u_luVariantFlags.y;
    float specularEnabled = u_luVariantFlags.z;
    float reflectionEnabled = u_luVariantFlags.w;
    vec4 reflColor = v_vdn * textureCube(s_luEnv, vec3(v_reflectVector.x, -v_reflectVector.y, v_reflectVector.z)) * 0.05 * reflectionEnabled * u_luReflectionParams.y;
    float fres = mix(legoppCalculateFresnel(v_vdn), 0.08, lowSource);
    vec4 specular = calculateSpecular(viewVector, v_diffuse.a, normal) * specularEnabled;

    vec3 baseColor = reflColor.rgb + specular.rgb + v_diffuse.rgb + (fres * color.rgb);
    vec3 texturedBase = reflColor.rgb + specular.rgb + ((v_diffuse.rgb + fres) * color.rgb);
    vec3 litColor = (baseColor * (1.0 - u_luShaderFlags.x)) + (texturedBase * u_luShaderFlags.x);
    litColor *= u_luLightColorShadow.a * shadowVisibilityWithNormal(v_worldPos.xyz, normal);
    litColor = mix(litColor, calculatePbrColor(color.rgb, normal, viewVector, v_reflectVector, v_diffuse.rgb, v_worldPos.xyz, reflectionEnabled), u_luPbrParams.x);

    float emissiveAmount = u_materialEmissive.r * mix(1.0, v_color0.a, u_luShaderFlags.y);
    float superEmissive = isVariant(LEGOPP_VARIANT_SUPEREMISSIVE);
    vec3 emissiveTarget = color.rgb * mix(1.0, 10.0, superEmissive);
    vec3 rgb = mix(litColor, emissiveTarget, emissiveAmount);
    float alpha = mix(1.0, texColor.a, u_luShaderFlags.x) * u_luLightDirFade.w;

    if (u_luShaderFlags.w >= 0.0 && alpha < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(rgb, v_worldPos.xyz), alpha);
}
