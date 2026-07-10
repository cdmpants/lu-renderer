$input v_normal, v_texcoord0, v_texcoord1, v_color0, v_worldPos

#include <bgfx_shader.sh>
#include "alpha_test.sh"

SAMPLER2D(s_diffuse, 0);

uniform vec4 u_materialDiffuse;
uniform vec4 u_luShaderFlags;
uniform vec4 u_reflectionMaskValue;

void main()
{
    vec4 tex = texture2D(s_diffuse, v_texcoord0);
    float vertexAlpha = mix(1.0, v_color0.a, u_luShaderFlags.y);
    float alpha = mix(1.0, tex.a, u_luShaderFlags.x) * u_materialDiffuse.a * vertexAlpha;
    if (!luAlphaTestPass(alpha)) {
        discard;
    }

    gl_FragColor = vec4(u_reflectionMaskValue.xxx, 1.0);
}
