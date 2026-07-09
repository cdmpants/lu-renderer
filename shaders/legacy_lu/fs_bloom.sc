$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_sceneColor, 0);
SAMPLER2D(s_bloomMask, 1);

uniform vec4 u_bloomParams;

vec3 sampleScene(vec2 uv)
{
    return texture2D(s_sceneColor, clamp(uv, vec2_splat(0.0), vec2_splat(1.0))).rgb;
}

vec3 sampleTent(vec2 uv, vec2 texel)
{
    vec3 color = sampleScene(uv) * 4.0;
    color += sampleScene(uv + texel * vec2(-1.0,  0.0)) * 2.0;
    color += sampleScene(uv + texel * vec2( 1.0,  0.0)) * 2.0;
    color += sampleScene(uv + texel * vec2( 0.0, -1.0)) * 2.0;
    color += sampleScene(uv + texel * vec2( 0.0,  1.0)) * 2.0;
    color += sampleScene(uv + texel * vec2(-1.0, -1.0));
    color += sampleScene(uv + texel * vec2( 1.0, -1.0));
    color += sampleScene(uv + texel * vec2(-1.0,  1.0));
    color += sampleScene(uv + texel * vec2( 1.0,  1.0));
    return color * (1.0 / 16.0);
}

vec3 extractBloom(vec2 uv, vec2 texel)
{
    vec3 color = sampleTent(uv, texel);
    float authoredBloom = texture2D(s_bloomMask, clamp(uv, vec2_splat(0.0), vec2_splat(1.0))).x;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float threshold = u_bloomParams.y;
    float soft = max(threshold * 0.25, 0.02);
    float bright = saturate((luma + authoredBloom * 0.35 - threshold + soft) / max(soft, 0.0001));
    return color * authoredBloom * bright;
}

void main()
{
    vec2 uv = clamp(v_texcoord0, vec2_splat(0.0), vec2_splat(1.0));
    vec2 texel = max(u_bloomParams.zw, vec2_splat(0.000001));
    float mode = u_bloomParams.x;

    vec3 color = mode < 0.5
        ? extractBloom(uv, texel)
        : sampleTent(uv, texel);

    gl_FragColor = vec4(max(color, vec3_splat(0.0)), 1.0);
}
