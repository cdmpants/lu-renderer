$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_gtao, 0);
SAMPLER2D(s_sceneDepth, 1);

uniform vec4 u_screenParams;
uniform vec4 u_depthParams;

float sampleDepth(vec2 uv)
{
    return texture2DLod(s_sceneDepth, clamp(uv, vec2_splat(0.0), vec2_splat(1.0)), 0.0).x;
}

float linearizeDepth(float depth)
{
    return u_depthParams.x / max(u_depthParams.y - depth, 0.00001);
}

void denoiseTap(vec2 uv, vec2 offset, float centerDepth, inout float visibilitySum, inout float weightSum)
{
    vec2 sampleUv = clamp(uv + offset * u_screenParams.zw, vec2_splat(0.0), vec2_splat(1.0));
    float sampleDepthRaw = sampleDepth(sampleUv);
    float sampleDepth = linearizeDepth(sampleDepthRaw);
    float depthDelta = abs(sampleDepth - centerDepth);
    float spatial = exp2(-0.45 * dot(offset, offset));
    float depthWeight = exp2(-depthDelta * 18.0 / max(centerDepth, 0.001));
    float weight = spatial * depthWeight * (sampleDepthRaw < 0.9999 ? 1.0 : 0.0);
    visibilitySum += texture2DLod(s_gtao, sampleUv, 0.0).x * weight;
    weightSum += weight;
}

void main()
{
    vec2 uv = clamp(v_texcoord0, vec2_splat(0.0), vec2_splat(1.0));
    float centerDepthRaw = sampleDepth(uv);
    if (centerDepthRaw >= 0.9999) {
        gl_FragColor = vec4_splat(1.0);
        return;
    }

    float centerDepth = linearizeDepth(centerDepthRaw);
    float visibilitySum = 0.0;
    float weightSum = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            denoiseTap(uv, vec2(float(x), float(y)), centerDepth, visibilitySum, weightSum);
        }
    }

    float visibility = visibilitySum / max(weightSum, 0.0001);
    gl_FragColor = vec4_splat(clamp(visibility, 0.0, 1.0));
}
