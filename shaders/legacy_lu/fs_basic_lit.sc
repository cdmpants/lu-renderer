$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);

#include "shadow_common.sh"

uniform vec4 u_materialDiffuse;
uniform vec4 u_lightDirAmbient;
uniform vec4 u_lightColor;
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
    vec3 normal = normalize(v_normal);
    vec3 lightDir = normalize(-u_lightDirAmbient.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);
    vec3 lit = vec3_splat(u_lightDirAmbient.w) + u_lightColor.rgb * ndotl;

    vec4 texColor = texture2D(s_diffuse, v_texcoord0);
    vec4 shaderColor = (v_color0 * u_luShaderFlags.y) +
                       (u_materialDiffuse * u_luShaderFlags.z) +
                       (vec4(1.0, 1.0, 1.0, 1.0) * (1.0 - max(u_luShaderFlags.y, u_luShaderFlags.z)));
    vec4 textured = texColor * shaderColor;
    vec4 untextured = shaderColor;
    vec4 color = (textured * u_luShaderFlags.x) + (untextured * (1.0 - u_luShaderFlags.x));

    if (u_luShaderFlags.w >= 0.0 && color.a < u_luShaderFlags.w) {
        discard;
    }

    vec3 rgb = color.rgb * lit * shadowVisibilityWithNormal(v_worldPos.xyz, normal);
    gl_FragColor = vec4(applyLuFog(rgb, v_worldPos.xyz), color.a);
}
