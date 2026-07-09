$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_diffuse, 0);

uniform vec4 u_materialDiffuse;
uniform vec4 u_luShaderFlags;

void main()
{
    vec4 tex = texture2D(s_diffuse, v_texcoord0);
    float vertexAlpha = mix(1.0, v_color0.a, u_luShaderFlags.y);
    float alpha = mix(1.0, tex.a, u_luShaderFlags.x) * u_materialDiffuse.a * vertexAlpha;
    if (u_luShaderFlags.w >= 0.0 && alpha < u_luShaderFlags.w) {
        discard;
    }
}
