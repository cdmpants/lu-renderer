$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular, v_vertPos, v_diffuseExtra

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);
SAMPLERCUBE(s_luEnv, 1);
SAMPLER2D(s_dark, 2);

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
uniform vec4 u_materialDiffuse;
uniform vec4 u_materialEmissive;
uniform vec4 u_luEffectParams;
uniform vec4 u_luGlowColor;
uniform vec4 u_luShinyGlint;
uniform vec4 u_luShinyGlintColor;

#define LEGOPP_VARIANT_SUPEREMISSIVE 4.0
#define LEGOPP_VARIANT_GLOW 5.0
#define LEGOPP_VARIANT_GLOW_IGNORE_VERT_ALPHA 6.0
#define LEGOPP_VARIANT_GRAYSCALE 7.0
#define LEGOPP_VARIANT_DARKLING 8.0
#define LEGOPP_VARIANT_DARKLING_SPECULAR 9.0
#define LEGOPP_VARIANT_DARKLING_STRUCTURE 10.0
#define LEGOPP_VARIANT_DARKLING_SHINY_GLINT 11.0
#define LEGOPP_VARIANT_DARKLING_SPECULAR_SHINY_GLINT 12.0
#define LEGOPP_VARIANT_DARKLING_STRUCTURE_SHINY_GLINT 13.0
#define LEGOPP_VARIANT_SHINY_GLINT 25.0
#define LEGOPP_VARIANT_ITEM 14.0
#define LEGOPP_VARIANT_ITEM_GLOW 15.0
#define LEGOPP_VARIANT_FRONTEND 16.0
#define LEGOPP_VARIANT_MASKED_NONDECAL 17.0
#define LEGOPP_VARIANT_REVEAL 18.0
#define LEGOPP_VARIANT_FADE_UP 19.0
#define LEGOPP_VARIANT_NO_LIGHT 21.0
#define LEGOPP_VARIANT_FACE_CREATE 22.0

vec3 applyLuFog(vec3 rgb, vec3 worldPos)
{
    float denom = max(0.0001, u_luFogParams.y - u_luFogParams.x);
    float fogAmount = clamp((distance(u_luCameraPos.xyz, worldPos) - u_luFogParams.x) / denom, 0.0, 1.0) * u_luFogParams.z;
    return mix(rgb, u_luFogColor.rgb, fogAmount);
}

float isVariant(float variant)
{
    return 1.0 - step(0.5, abs(u_luVariantFlags.x - variant));
}

float isDarklingVariant()
{
    float code = u_luVariantFlags.x;
    return step(LEGOPP_VARIANT_DARKLING - 0.5, code) *
           (1.0 - step(LEGOPP_VARIANT_DARKLING_STRUCTURE + 0.5, code));
}

float isShinyGlintVariant()
{
    float code = u_luVariantFlags.x;
    float darklingGlint =
        step(LEGOPP_VARIANT_DARKLING_SHINY_GLINT - 0.5, code) *
        (1.0 - step(LEGOPP_VARIANT_DARKLING_STRUCTURE_SHINY_GLINT + 0.5, code));
    return max(darklingGlint, isVariant(LEGOPP_VARIANT_SHINY_GLINT));
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

vec4 calculateSpecularMasked(vec3 viewVector, float ldn, vec3 normal)
{
    vec3 halfVector = normalize(viewVector + u_luLightDirFade.xyz);
    float hdn = pow(max(0.0, dot(halfVector, normal)), 150.0);
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
    return (direct + ambient + envSpecular) * u_luLightColorShadow.a * shadowVisibility(worldPos);
}

vec4 applyGrayscale(vec4 result)
{
    vec3 gray = vec3_splat(0.7 * ((0.3 * result.r) + (0.7 * result.g) + (0.1 * result.b) + u_luEffectParams.y));
    result.rgb = mix(result.rgb, gray, u_luEffectParams.x);
    return result;
}

vec4 applyGlow(vec4 result)
{
    vec3 glow = u_luGlowColor.rgb * result.a;
    result.rgb = mix(result.rgb, glow, u_luEffectParams.z);
    return result;
}

vec4 applyShinyGlint(vec4 result, float vertPos)
{
    float specAddVal = pow(saturate(1.0 - abs(u_luShinyGlint.x - vertPos)), u_luShinyGlint.y) * u_luShinyGlintColor.a;
    result.rgb += u_luShinyGlintColor.rgb * specAddVal;
    return result;
}

void main()
{
    vec4 texColor = texture2D(s_diffuse, v_texcoord0);
    vec4 colorTextured = vec4(texColor.rgb, 1.0);
    vec4 colorVertTextured = vec4(mix(v_color0.rgb, texColor.rgb, texColor.a), v_color0.a);
    vec4 colorWithTexture = (colorTextured * (1.0 - u_luShaderFlags.y)) + (colorVertTextured * u_luShaderFlags.y);
    vec4 color = (v_color0 * (1.0 - u_luShaderFlags.x)) + (colorWithTexture * u_luShaderFlags.x);
    float itemVariant = max(isVariant(LEGOPP_VARIANT_ITEM), isVariant(LEGOPP_VARIANT_ITEM_GLOW));
    if (itemVariant > 0.5) {
        vec4 itemTextured = vec4(texColor.rgb, 1.0);
        vec4 itemVertTextured = itemTextured * v_color0;
        vec4 itemColorWithTexture = mix(itemTextured, itemVertTextured, u_luShaderFlags.y);
        color = mix(v_color0, itemColorWithTexture, u_luShaderFlags.x);
    }

    float fadeUp = isVariant(LEGOPP_VARIANT_FADE_UP);
    float fadeAmt = 1.0 - saturate(u_luEffectParams.w - v_vertPos);
    color.rgb = mix(color.rgb, color.rgb + vec3_splat(10.0 * fadeAmt), fadeUp);
    color.a = mix(color.a, 1.0 - fadeAmt, fadeUp);

    vec3 normal = normalize(v_normal);
    vec3 viewVector = normalize(u_luCameraPos.xyz - v_worldPos.xyz);
    float specularEnabled = u_luVariantFlags.z;
    float reflectionEnabled = u_luVariantFlags.w;
    float lowSource = u_luVariantFlags.y;
    vec4 reflColor = v_vdn * textureCube(s_luEnv, vec3(v_reflectVector.x, -v_reflectVector.y, v_reflectVector.z)) * 0.05 * reflectionEnabled * u_luReflectionParams.y;
    float fres = mix(legoppCalculateFresnel(v_vdn), 0.08, lowSource);
    vec4 specular = calculateSpecular(viewVector, v_diffuse.a, normal) * specularEnabled;

    vec3 builtInColor = reflColor.rgb + specular.rgb + v_diffuse.rgb + v_diffuseExtra + (fres * color.rgb);
    vec3 texturedColor = reflColor.rgb + specular.rgb + v_diffuseExtra + ((v_diffuse.rgb + fres) * color.rgb);
    vec3 rgb = (builtInColor * (1.0 - u_luShaderFlags.x)) + (texturedColor * u_luShaderFlags.x);
    rgb *= u_luLightColorShadow.a * shadowVisibility(v_worldPos.xyz);
    rgb = mix(rgb, calculatePbrColor(color.rgb, normal, viewVector, v_reflectVector, v_diffuse.rgb, v_worldPos.xyz, reflectionEnabled), u_luPbrParams.x);

    float ignoreVertAlpha = isVariant(LEGOPP_VARIANT_GLOW_IGNORE_VERT_ALPHA);
    color.a = mix(color.a, 1.0, ignoreVertAlpha);

    float alpha = color.a * u_luLightDirFade.w;
    vec4 result = vec4(rgb, alpha);

    float frontEnd = isVariant(LEGOPP_VARIANT_FRONTEND);
    if (frontEnd > 0.5) {
        vec4 frontEndTexColor = vec4(texColor.rgb, 1.0);
        vec4 frontEndTexturedColor = mix(frontEndTexColor, frontEndTexColor * v_color0, u_luShaderFlags.y);
        vec4 frontEndColor = mix(v_color0, frontEndTexturedColor, u_luShaderFlags.x);
        vec3 frontEndBuiltIn = vec3_splat(0.1) + reflColor.rgb + (v_specular.rgb * specularEnabled) + v_diffuse.rgb + (fres * frontEndColor.rgb);
        vec3 frontEndTextured = vec3_splat(0.1) + reflColor.rgb + (v_specular.rgb * specularEnabled) + ((v_diffuse.rgb + fres) * frontEndColor.rgb);
        float washDist = clamp(((u_luCameraPos.z - v_worldPos.z) - 50.0) / 5.0, 0.0, 1.0);
        vec3 dullValue = vec3_splat(washDist * 0.5);
        result.rgb = clamp(mix(frontEndBuiltIn, frontEndTextured, u_luShaderFlags.x) * (vec3_splat(1.0) - dullValue) + dullValue,
                           vec3_splat(0.1),
                           vec3_splat(0.9));
        result.a = frontEndColor.a * u_luLightDirFade.w;
    }

    float noLight = isVariant(LEGOPP_VARIANT_NO_LIGHT);
    if (noLight > 0.5) {
        vec4 unlitColor = mix(v_color0, texColor, u_luShaderFlags.x);
        unlitColor.a *= u_luLightDirFade.w * u_materialDiffuse.a;
        result = unlitColor;
    }

    float grayscale = isVariant(LEGOPP_VARIANT_GRAYSCALE);
    result = mix(result, applyGrayscale(result), grayscale);

    float glow = max(isVariant(LEGOPP_VARIANT_GLOW), isVariant(LEGOPP_VARIANT_ITEM_GLOW));
    glow = max(glow, ignoreVertAlpha);
    result = mix(result, applyGlow(result), glow);

    float masked = isVariant(LEGOPP_VARIANT_MASKED_NONDECAL);
    if (masked > 0.5) {
        vec4 maskColor = texture2D(s_dark, v_texcoord0);
        vec4 maskedReflColor = v_vdn * textureCube(s_luEnv, vec3(v_reflectVector.x, -v_reflectVector.y, v_reflectVector.z)) * maskColor.a * reflectionEnabled * u_luReflectionParams.y;
        vec4 maskedSpecular = calculateSpecularMasked(viewVector, v_diffuse.a, normal) * specularEnabled;
        maskedSpecular.rgb *= maskColor.rgb;
        vec4 maskedTextureColor = mix(texColor, texColor * v_color0, u_luShaderFlags.y);
        vec4 maskedColor = mix(v_color0, maskedTextureColor, u_luShaderFlags.x);
        vec3 maskedBuiltIn = maskedReflColor.rgb + maskedSpecular.rgb + v_diffuse.rgb + (fres * maskedColor.rgb);
        vec3 maskedTextured = maskedReflColor.rgb + maskedSpecular.rgb + ((v_diffuse.rgb + fres) * maskedColor.rgb);
        result.rgb = ((maskedBuiltIn * (1.0 - u_luShaderFlags.x)) + (maskedTextured * u_luShaderFlags.x)) * u_luLightColorShadow.a * shadowVisibility(v_worldPos.xyz);
        result.rgb = mix(result.rgb, calculatePbrColor(maskedColor.rgb, normal, viewVector, v_reflectVector, v_diffuse.rgb, v_worldPos.xyz, maskColor.a * reflectionEnabled), u_luPbrParams.x);
        result.a = maskedColor.a * u_luLightDirFade.w;

        vec4 noEnvNonDecalColor = texColor * v_color0;
        vec3 noEnvNonDecalRgb = (specular.rgb * specularEnabled) + ((v_diffuse.rgb + fres) * noEnvNonDecalColor.rgb);
        float noEnvNonDecal = 1.0 - reflectionEnabled;
        result.rgb = mix(result.rgb, noEnvNonDecalRgb, noEnvNonDecal);
        result.a = mix(result.a, noEnvNonDecalColor.a * u_luLightDirFade.w, noEnvNonDecal);
    }

    float faceCreate = isVariant(LEGOPP_VARIANT_FACE_CREATE);
    if (faceCreate > 0.5) {
        vec4 faceColor = texColor * v_color0;
        result.rgb = (reflColor.rgb + specular.rgb + ((v_diffuse.rgb + fres) * faceColor.rgb)) * u_luLightColorShadow.a * shadowVisibility(v_worldPos.xyz);
        result.rgb = mix(result.rgb, calculatePbrColor(faceColor.rgb, normal, viewVector, v_reflectVector, v_diffuse.rgb, v_worldPos.xyz, reflectionEnabled), u_luPbrParams.x);
        result.a = faceColor.a * u_luLightDirFade.w;
    }

    float reveal = isVariant(LEGOPP_VARIANT_REVEAL);
    if (reveal > 0.5) {
        float maskValue = texture2D(s_dark, v_texcoord0).a;
        float revealAlpha = clamp((maskValue - u_materialDiffuse.a + 0.1) * 10.0, 0.0, 1.0) * u_luLightDirFade.w;
        float vertexColorOnly = u_luShaderFlags.y * (1.0 - u_luShaderFlags.x);
        result.a = mix(revealAlpha, result.a * revealAlpha, vertexColorOnly);
    }

    float darkling = isDarklingVariant();
    if (darkling > 0.5) {
        float centerDis = abs(u_materialEmissive.r - v_color0.a);
        float adjustedDis = min(centerDis, 1.0 - centerDis);
        float widthMod = u_materialEmissive.g * 0.5;
        float blurMod = u_materialEmissive.b * 100.0;
        float darklingLerp = 1.0 - clamp((adjustedDis - widthMod) * blurMod, 0.0, 1.0);
        vec4 darklingColor = texture2D(s_dark, v_texcoord1);
        vec3 combinedDarkling = mix(v_color0.rgb, texColor.rgb, texColor.a) * v_diffuse.rgb;
        float specularDarkling = max(isVariant(LEGOPP_VARIANT_DARKLING_SPECULAR),
                                     isVariant(LEGOPP_VARIANT_DARKLING_SPECULAR_SHINY_GLINT));
        float structure = max(isVariant(LEGOPP_VARIANT_DARKLING_STRUCTURE),
                              isVariant(LEGOPP_VARIANT_DARKLING_STRUCTURE_SHINY_GLINT));
        vec3 nonDecalColor = texColor.rgb * v_color0.rgb * v_diffuse.rgb;
        vec3 darklingBase = mix(combinedDarkling, result.rgb, specularDarkling);
        darklingBase = mix(darklingBase, nonDecalColor, structure);
        float darklingControl = mix(darklingLerp, v_color0.a, structure);
        result.rgb = mix(darklingBase, darklingColor.rgb, darklingColor.a * darklingControl);
        result.a = mix(clamp(v_diffuse.a, 0.0, 1.0), u_luLightDirFade.w, specularDarkling);
    }

    float shinyGlint = isShinyGlintVariant();
    result = mix(result, applyShinyGlint(result, v_vertPos), shinyGlint);

    if (u_luShaderFlags.w >= 0.0 && result.a < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(result.rgb, v_worldPos.xyz), result.a);
}
