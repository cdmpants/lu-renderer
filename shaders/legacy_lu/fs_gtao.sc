$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_sceneDepth, 0);
SAMPLER2D(s_sceneNormal, 1);

uniform vec4 u_screenParams;
uniform vec4 u_screenSpaceParams;
uniform vec4 u_depthParams;
uniform vec4 u_temporalParams;

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float sampleDepth(vec2 uv)
{
    return texture2DLod(s_sceneDepth, clamp(uv, vec2_splat(0.0), vec2_splat(1.0)), 0.0).x;
}

float linearizeDepth(float depth)
{
    return u_depthParams.x / max(u_depthParams.y - depth, 0.00001);
}

vec3 reconstructViewPos(vec2 uv, float viewDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * u_depthParams.z * viewDepth, -ndc.y * u_depthParams.w * viewDepth, viewDepth);
}

vec3 inferViewNormal(vec2 uv, vec3 centerPos)
{
    vec2 texel = u_screenParams.zw;
    float leftZ = linearizeDepth(sampleDepth(uv - vec2(texel.x, 0.0)));
    float rightZ = linearizeDepth(sampleDepth(uv + vec2(texel.x, 0.0)));
    float topZ = linearizeDepth(sampleDepth(uv - vec2(0.0, texel.y)));
    float bottomZ = linearizeDepth(sampleDepth(uv + vec2(0.0, texel.y)));

    vec3 leftPos = reconstructViewPos(uv - vec2(texel.x, 0.0), leftZ);
    vec3 rightPos = reconstructViewPos(uv + vec2(texel.x, 0.0), rightZ);
    vec3 topPos = reconstructViewPos(uv - vec2(0.0, texel.y), topZ);
    vec3 bottomPos = reconstructViewPos(uv + vec2(0.0, texel.y), bottomZ);

    vec3 dx = abs(leftZ - centerPos.z) < abs(rightZ - centerPos.z) ? centerPos - leftPos : rightPos - centerPos;
    vec3 dy = abs(topZ - centerPos.z) < abs(bottomZ - centerPos.z) ? centerPos - topPos : bottomPos - centerPos;
    vec3 normal = normalize(cross(dy, dx));
    return normal.z > 0.0 ? -normal : normal;
}

vec3 sampleViewNormal(vec2 uv, vec3 centerPos)
{
    vec3 encoded = texture2D(s_sceneNormal, clamp(uv, vec2_splat(0.0), vec2_splat(1.0))).rgb;
    vec3 normal = encoded * 2.0 - 1.0;
    float lenSq = dot(normal, normal);
    vec3 inferred = inferViewNormal(uv, centerPos);
    normal = normalize(mix(inferred, normal, step(0.05, lenSq)));
    return normal.z > 0.0 ? -normal : normal;
}

float gtaoHorizonTap(vec2 uv, vec3 centerPos, vec3 normal, vec2 dir, float pixelRadius, float viewRadius, float stepScale, float jitter, float bias)
{
    float sampleRadius = pixelRadius * clamp(stepScale + jitter * 0.045, 0.02, 1.0);
    vec2 sampleUv = uv + dir * sampleRadius * u_screenParams.zw;
    float sampleDepthRaw = sampleDepth(sampleUv);
    if (sampleDepthRaw >= 0.9999) return 0.0;

    vec3 samplePos = reconstructViewPos(sampleUv, linearizeDepth(sampleDepthRaw));
    vec3 delta = samplePos - centerPos;
    float dist = max(length(delta), 0.000001);
    float depthDelta = samplePos.z - centerPos.z;
    float distanceFade = saturate(1.0 - dist / max(viewRadius, 0.0001));
    float depthFade = saturate(1.0 - abs(depthDelta) / max(viewRadius * 1.65, 0.0001));
    float frontFade = saturate((depthDelta + viewRadius * 0.28) / max(viewRadius * 0.28, 0.0001));
    float horizon = max(dot(normal, delta / dist) - bias, 0.0);
    return horizon * distanceFade * distanceFade * depthFade * frontFade;
}

float gtaoSliceOcclusion(vec2 uv, vec3 centerPos, vec3 normal, vec2 dir, float pixelRadius, float viewRadius, float jitter, float bias)
{
    float side0 = 0.0;
    side0 = max(side0, gtaoHorizonTap(uv, centerPos, normal,  dir, pixelRadius, viewRadius, 0.06, jitter, bias));
    side0 = max(side0, gtaoHorizonTap(uv, centerPos, normal,  dir, pixelRadius, viewRadius, 0.20, jitter, bias));
    side0 = max(side0, gtaoHorizonTap(uv, centerPos, normal,  dir, pixelRadius, viewRadius, 0.48, jitter, bias));
    side0 = max(side0, gtaoHorizonTap(uv, centerPos, normal,  dir, pixelRadius, viewRadius, 0.90, jitter, bias));

    float side1 = 0.0;
    side1 = max(side1, gtaoHorizonTap(uv, centerPos, normal, -dir, pixelRadius, viewRadius, 0.06, jitter, bias));
    side1 = max(side1, gtaoHorizonTap(uv, centerPos, normal, -dir, pixelRadius, viewRadius, 0.20, jitter, bias));
    side1 = max(side1, gtaoHorizonTap(uv, centerPos, normal, -dir, pixelRadius, viewRadius, 0.48, jitter, bias));
    side1 = max(side1, gtaoHorizonTap(uv, centerPos, normal, -dir, pixelRadius, viewRadius, 0.90, jitter, bias));

    return (side0 + side1) * 0.5;
}

float gtaoVisibility(vec2 uv, float centerDepthRaw, vec2 pixel)
{
    float intensity = u_screenSpaceParams.x;
    if (intensity <= 0.0 || centerDepthRaw >= 0.9999) return 1.0;

    float centerDepth = linearizeDepth(centerDepthRaw);
    vec3 centerPos = reconstructViewPos(uv, centerDepth);
    vec3 normal = sampleViewNormal(uv, centerPos);
    float viewRadius = max(u_screenSpaceParams.y, 0.05);
    float projectedRadiusX = viewRadius / max(centerDepth * u_depthParams.z, 0.0001) * u_screenParams.x * 0.5;
    float projectedRadiusY = viewRadius / max(centerDepth * u_depthParams.w, 0.0001) * u_screenParams.y * 0.5;
    float pixelRadius = clamp(min(projectedRadiusX, projectedRadiusY), 2.0, 96.0);
    float bias = 0.10;
    float noise = hash12(pixel + vec2_splat(u_temporalParams.w * 0.37));
    float angle = noise * 6.2831853;
    vec2 rot = vec2(cos(angle), sin(angle));
    vec2 d0 = rot;
    vec2 d1 = vec2(-rot.y, rot.x);
    vec2 d2 = normalize(d0 + d1);
    vec2 d3 = normalize(d0 - d1);

    float occ = 0.0;
    occ += gtaoSliceOcclusion(uv, centerPos, normal, d0, pixelRadius, viewRadius, noise, bias);
    occ += gtaoSliceOcclusion(uv, centerPos, normal, d1, pixelRadius, viewRadius, noise, bias);
    occ += gtaoSliceOcclusion(uv, centerPos, normal, d2, pixelRadius, viewRadius, noise, bias);
    occ += gtaoSliceOcclusion(uv, centerPos, normal, d3, pixelRadius, viewRadius, noise, bias);

    occ = saturate(occ * 0.55);
    return pow(saturate(1.0 - occ), max(intensity, 0.01));
}

void main()
{
    vec2 uv = clamp(v_texcoord0, vec2_splat(0.0), vec2_splat(1.0));
    float visibility = gtaoVisibility(uv, sampleDepth(uv), uv * u_screenParams.xy);
    gl_FragColor = vec4_splat(clamp(visibility, 0.0, 1.0));
}
