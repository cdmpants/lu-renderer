$input v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);

uniform vec4 u_materialEmissive;
uniform vec4 u_luEffectTime;
uniform vec4 u_luUvMotion1;
uniform vec4 u_luUvMotion2;
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
    vec2 layer1Uv = (v_texcoord0 * 0.75) + (u_luUvMotion1.xy * u_luEffectTime.x * u_luEffectTime.y);
    vec2 layer2Uv = v_texcoord0 + (u_luUvMotion2.xy * u_luEffectTime.x * u_luEffectTime.y);

    vec4 layer1Color = texture2D(s_diffuse, layer1Uv);
    vec2 tempUv = layer2Uv;
    tempUv.x += layer1Color.r * 0.2 - 0.5;
    tempUv.y += layer1Color.g * 0.2 - 0.5;

    vec4 layer2Color = texture2D(s_diffuse, tempUv);
    vec4 warpedColor = (layer1Color * 0.5) + (layer2Color * 0.5);
    vec4 color = mix(layer1Color, warpedColor, warpedColor.a);

    float centerDis = abs(u_materialEmissive.r - v_diffuse.a);
    float adjustedDis = min(centerDis, 1.0 - centerDis);
    float widthMod = u_materialEmissive.g * 0.5;
    float blurMod = u_materialEmissive.b * 100.0;
    float alphaMod = 1.0 - clamp((adjustedDis - widthMod) * blurMod, 0.0, 1.0);

    color *= v_diffuse;
    color.a *= alphaMod;

    if (u_luShaderFlags.w >= 0.0 && color.a < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(color.rgb, v_worldPos.xyz), color.a);
}
