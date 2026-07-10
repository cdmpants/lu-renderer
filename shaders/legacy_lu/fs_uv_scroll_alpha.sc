$input v_normal, v_texcoord0, v_color0, v_worldPos

#include <bgfx_shader.sh>
#include "alpha_test.sh"

SAMPLER2D(s_diffuse, 0);

uniform vec4 u_materialDiffuse;
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
    vec4 diffuse = vec4(v_color0.rgb, v_color0.a * u_materialDiffuse.a);
    vec4 textured = texColor * diffuse;
    vec4 color = (textured * u_luShaderFlags.x) + (diffuse * (1.0 - u_luShaderFlags.x));

    if (!luAlphaTestPass(color.a)) {
        discard;
    }

    gl_FragColor = vec4(applyLuFog(color.rgb, v_worldPos.xyz), color.a);
}
