$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);
SAMPLER2D(s_dark, 2);

uniform vec4 u_materialDiffuse;
uniform vec4 u_luCameraPos;
uniform vec4 u_luFogColor;
uniform vec4 u_luFogParams;
uniform vec4 u_luShaderFlags;
uniform vec4 u_luEffectTime;
uniform vec4 u_luUvMotion1;
uniform vec4 u_luUvMotion2;

vec3 applyLuFog(vec3 rgb, vec3 worldPos)
{
    float denom = max(0.0001, u_luFogParams.y - u_luFogParams.x);
    float fogAmount = clamp((distance(u_luCameraPos.xyz, worldPos) - u_luFogParams.x) / denom, 0.0, 1.0) * u_luFogParams.z;
    return mix(rgb, u_luFogColor.rgb, fogAmount);
}

void main()
{
    vec2 layer1Uv = v_texcoord0 + (u_luUvMotion1.xy * u_luEffectTime.x * u_luEffectTime.y);
    vec2 layer2Uv = v_texcoord1 + (u_luUvMotion2.xy * u_luEffectTime.x * u_luEffectTime.y);

    vec4 texColor = texture2D(s_diffuse, layer1Uv) * u_materialDiffuse.r;
    texColor += texture2D(s_dark, layer2Uv) * u_materialDiffuse.g;

    vec4 color = texColor * v_color0;
    if (u_luShaderFlags.w >= 0.0 && color.a < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(color.rgb, v_worldPos.xyz), color.a);
}
