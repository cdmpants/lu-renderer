$input a_position, a_normal, a_texcoord0, a_texcoord1, a_color0
$output v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular, v_vertPos, v_diffuseExtra

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
uniform vec4 u_luVariantFlags;
uniform vec4 u_luEffectTime;
uniform vec4 u_luUvMotion1;
uniform vec4 u_luBbbLightDir1;
uniform vec4 u_luBbbLightDir2;
uniform vec4 u_luBbbLightColor1;
uniform vec4 u_luBbbLightColor2;

#define LEGOPP_VARIANT_FRONTEND 16.0
#define LEGOPP_VARIANT_THREELIGHT 24.0
#define LEGOPP_VARIANT_DARKLING 8.0
#define LEGOPP_VARIANT_DARKLING_STRUCTURE 10.0
#define LEGOPP_VARIANT_ITEM 14.0
#define LEGOPP_VARIANT_ITEM_GLOW 15.0

float isVariant(float variant)
{
    return 1.0 - step(0.5, abs(u_luVariantFlags.x - variant));
}

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
    float itemVariant = max(isVariant(LEGOPP_VARIANT_ITEM), isVariant(LEGOPP_VARIANT_ITEM_GLOW));
    float itemVertexColor = itemVariant * u_luShaderFlags.y * (1.0 - u_luShaderFlags.x);
    vec4 itemColor = mix(u_materialDiffuse, u_materialDiffuse * a_color0, itemVertexColor);
    meshColor = mix(meshColor, itemColor, itemVariant);

    float ldn = dot(u_luLightDirFade.xyz, normal);
    vec3 hemiLight = calculateHemiLightInfluence(normal);
    vec3 diffuse = max(0.0, ldn) * hemiLight * u_luLightColorShadow.rgb + u_luAmbient.rgb;
    diffuse *= (vec3_splat(1.0) * (1.0 - u_luShaderFlags.z)) + (meshColor.rgb * u_luShaderFlags.z);
    vec4 specular = calculateSpecular(viewVector, ldn, normal, 120.0, 4.19);

    float frontEnd = isVariant(LEGOPP_VARIANT_FRONTEND);
    float threeLight = isVariant(LEGOPP_VARIANT_THREELIGHT);
    float simpleDarkling = max(isVariant(LEGOPP_VARIANT_DARKLING), isVariant(LEGOPP_VARIANT_DARKLING_STRUCTURE));
    float frontEndLdn = max(0.3, ldn);
    diffuse = mix(diffuse, max(0.0, frontEndLdn) * hemiLight * u_luLightColorShadow.rgb, frontEnd);
    ldn = mix(ldn, frontEndLdn, frontEnd);
    specular = mix(specular, calculateSpecular(viewVector, frontEndLdn, normal, 320.0, 1.0), frontEnd);
    vec3 threeLightDiffuse = (max(0.0, ldn) * meshColor.rgb * u_luLightColorShadow.rgb) +
                             (u_luAmbient.rgb * meshColor.rgb);
    diffuse = mix(diffuse, threeLightDiffuse, threeLight);
    vec3 simpleDarklingDiffuse = max(vec3_splat(0.0), u_luLightColorShadow.rgb * ldn) + u_luAmbient.rgb;
    diffuse = mix(diffuse, simpleDarklingDiffuse, simpleDarkling);
    ldn = mix(ldn, u_luLightDirFade.w, simpleDarkling);
    float ldn1 = dot(u_luBbbLightDir1.xyz, normal);
    float ldn2 = dot(u_luBbbLightDir2.xyz, normal);
    vec3 diffuseExtra = (max(0.0, ldn1) * meshColor.rgb * u_luBbbLightColor1.rgb) +
                        (max(0.0, ldn2) * meshColor.rgb * u_luBbbLightColor2.rgb);

    gl_Position = mul(u_modelViewProj, localPos);
    v_normal = normal;
    v_texcoord0 = a_texcoord0 + (u_luUvMotion1.xy * u_luEffectTime.x * u_luEffectTime.y);
    v_texcoord1 = a_texcoord1;
    v_color0 = meshColor;
    v_worldPos = worldPos;
    v_reflectVector = reflect(viewVector, normal);
    v_vdn = dot(viewVector, normal);
    v_diffuse = vec4(diffuse, ldn);
    v_specular = specular;
    v_vertPos = a_position.y;
    v_diffuseExtra = diffuseExtra * threeLight;
}
