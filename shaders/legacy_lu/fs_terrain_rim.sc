$input v_normal, v_texcoord0, v_color0, v_worldPos, v_reflectVector, v_vdn, v_diffuse, v_specular

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);

#include "shadow_common.sh"

uniform vec4 u_luLightDirFade;
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
    float rimLightAmt = clamp(2.0 * pow(1.0 - clamp(2.0 * v_vdn, 0.0, 1.0), 2.0), 0.0, 1.0);
    vec4 texColor = texture2D(s_diffuse, v_texcoord0);
    vec4 color = (texColor * v_color0) * (v_diffuse + vec4(vec3_splat(rimLightAmt), 0.0));
    color.a = u_luLightDirFade.w;

    if (u_luShaderFlags.w >= 0.0 && color.a < u_luShaderFlags.w) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(color.rgb * shadowVisibility(v_worldPos.xyz), v_worldPos.xyz), color.a);
}
