$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_sceneColor, 0);
SAMPLER2D(s_sceneDepth, 1);
SAMPLER2D(s_reflectionMask, 2);
SAMPLER2D(s_colorLut, 3);
SAMPLER2D(s_bloomMask, 4);
SAMPLER2D(s_sceneNormal, 5);
SAMPLER2D(s_historyColor, 6);

uniform vec4 u_postParams;
uniform vec4 u_bloomParams;
uniform vec4 u_dofParams;
uniform vec4 u_colorLutParams;
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

vec3 sampleColorLutSlice(vec3 color, float slice)
{
    float size = max(u_colorLutParams.y, 2.0);
    float horizontal = u_colorLutParams.z;
    float texel = 1.0 / size;
    float halfTexel = 0.5 / size;
    vec2 uvHorizontal = vec2(
        (color.r + slice * size + 0.5) / (size * size),
        color.g * (1.0 - texel) + halfTexel);
    vec2 uvVertical = vec2(
        color.r * (1.0 - texel) + halfTexel,
        (color.g + slice * size + 0.5) / (size * size));
    return texture2D(s_colorLut, mix(uvVertical, uvHorizontal, horizontal)).rgb;
}

vec3 applyColorLut(vec3 color)
{
    float intensity = u_colorLutParams.x;
    if (intensity <= 0.0) return color;

    vec3 clampedColor = clamp(color, vec3_splat(0.0), vec3_splat(1.0));
    float size = max(u_colorLutParams.y, 2.0);
    float blue = clampedColor.b * (size - 1.0);
    float slice0 = floor(blue);
    float slice1 = min(slice0 + 1.0, size - 1.0);
    vec3 graded = mix(
        sampleColorLutSlice(clampedColor, slice0),
        sampleColorLutSlice(clampedColor, slice1),
        fract(blue));
    return mix(color, graded, saturate(intensity));
}

float sampleDepth(vec2 uv)
{
    return texture2DLod(s_sceneDepth, clamp(uv, vec2_splat(0.0), vec2_splat(1.0)), 0.0).x;
}

float linearizeDepth(float depth)
{
    return u_depthParams.x / max(u_depthParams.y - depth, 0.00001);
}

float dofSignedCoc(float depthRaw)
{
    float aperture = u_dofParams.x;
    if (aperture <= 0.0 || depthRaw >= 0.9999) return 0.0;

    float viewDepth = linearizeDepth(depthRaw);
    float focusDepth = max(u_dofParams.y, 0.01);
    float maxRadius = max(u_dofParams.z, 0.0);
    float focusRange = max(focusDepth * 0.45, 1.0);
    float coc = (viewDepth - focusDepth) / focusRange * aperture * 5.0;
    return clamp(coc, -1.0, 1.0) * maxRadius;
}

void dofBokehTap(vec2 uv, float centerCoc, vec2 diskOffset, inout vec3 color, inout float totalWeight)
{
    float centerRadius = abs(centerCoc);
    float sampleDistance = length(diskOffset);
    vec2 sampleUv = uv + diskOffset * max(centerRadius, 1.0) * u_screenParams.zw;
    float sampleDepthRaw = sampleDepth(sampleUv);
    float sampleCoc = dofSignedCoc(sampleDepthRaw);
    float sampleRadius = abs(sampleCoc);
    float sourceCoverage = saturate((sampleRadius - sampleDistance + 1.0) * 0.5);
    float centerCoverage = saturate((centerRadius - sampleDistance + 1.0) * 0.5);
    float foreground = sourceCoverage * step(sampleCoc, -0.5);
    float background = max(centerCoverage, sourceCoverage * step(0.5, sampleCoc));
    float sideMatch = centerCoc * sampleCoc >= 0.0 ? 1.0 : 0.35;
    float weight = max(foreground, background * sideMatch);
    weight *= 0.55 + 0.45 * saturate(1.0 - sampleDistance / max(centerRadius, 1.0));
    totalWeight += weight;
    color += texture2D(s_sceneColor, clamp(sampleUv, vec2_splat(0.0), vec2_splat(1.0))).rgb * weight;
}

vec3 dofApprox(vec2 uv, float centerDepthRaw, vec3 baseColor)
{
    float centerCoc = dofSignedCoc(centerDepthRaw);
    float radius = abs(centerCoc);
    if (radius <= 0.35) return baseColor;

    float totalWeight = 1.0;
    vec3 color = baseColor;
    dofBokehTap(uv, centerCoc, vec2( 0.000,  0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.433,  0.250), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.433, -0.250), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.000, -0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.433, -0.250), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.433,  0.250), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.000,  1.000), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.500,  0.866), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.866,  0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 1.000,  0.000), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.866, -0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.500, -0.866), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.000, -1.000), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.500, -0.866), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.866, -0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-1.000,  0.000), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.866,  0.500), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.500,  0.866), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.306,  0.739), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.739,  0.306), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.739, -0.306), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2( 0.306, -0.739), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.306, -0.739), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.739, -0.306), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.739,  0.306), color, totalWeight);
    dofBokehTap(uv, centerCoc, vec2(-0.306,  0.739), color, totalWeight);
    return color / max(totalWeight, 0.0001);
}

vec3 reconstructViewPos(vec2 uv, float viewDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * u_depthParams.z * viewDepth, -ndc.y * u_depthParams.w * viewDepth, viewDepth);
}

vec2 projectViewPos(vec3 viewPos)
{
    vec2 ndc = vec2(viewPos.x / max(viewPos.z * u_depthParams.z, 0.0001),
                    -viewPos.y / max(viewPos.z * u_depthParams.w, 0.0001));
    return ndc * 0.5 + 0.5;
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
    return normal.z < 0.0 ? -normal : normal;
}

vec3 sampleViewNormal(vec2 uv, vec3 centerPos)
{
    vec3 encoded = texture2D(s_sceneNormal, clamp(uv, vec2_splat(0.0), vec2_splat(1.0))).rgb;
    vec3 normal = encoded * 2.0 - 1.0;
    float lenSq = dot(normal, normal);
    vec3 inferred = inferViewNormal(uv, centerPos);
    normal = normalize(mix(inferred, normal, step(0.05, lenSq)));
    return normal.z < 0.0 ? -normal : normal;
}

float aoTap(vec2 sampleUv, vec3 centerPos, vec3 normal, float radius, float bias)
{
    float sampleDepthRaw = sampleDepth(sampleUv);
    if (sampleDepthRaw >= 0.9999) return 0.0;

    vec3 samplePos = reconstructViewPos(sampleUv, linearizeDepth(sampleDepthRaw));
    vec3 delta = samplePos - centerPos;
    float dist = max(length(delta), 0.000001);
    float normalTerm = max(dot(normal, delta / dist) - bias, 0.0);
    float distanceFade = saturate(1.0 - dist / max(radius, 0.0001));
    float depthFade = saturate(1.0 - abs(samplePos.z - centerPos.z) / max(radius * 2.0, 0.0001));
    return normalTerm * distanceFade * distanceFade * depthFade;
}

float gtaoDirection(vec2 uv, vec3 centerPos, vec3 normal, vec2 dir, float pixelRadius, float viewRadius, float bias)
{
    float occ = 0.0;
    float weight = 0.0;

    float stepWeight = 1.0;
    vec2 sampleUv = uv + dir * (pixelRadius * 0.25) * u_screenParams.zw;
    occ += aoTap(sampleUv, centerPos, normal, viewRadius, bias) * stepWeight;
    weight += stepWeight;

    stepWeight = 0.85;
    sampleUv = uv + dir * (pixelRadius * 0.50) * u_screenParams.zw;
    occ += aoTap(sampleUv, centerPos, normal, viewRadius, bias) * stepWeight;
    weight += stepWeight;

    stepWeight = 0.65;
    sampleUv = uv + dir * (pixelRadius * 0.75) * u_screenParams.zw;
    occ += aoTap(sampleUv, centerPos, normal, viewRadius, bias) * stepWeight;
    weight += stepWeight;

    stepWeight = 0.45;
    sampleUv = uv + dir * pixelRadius * u_screenParams.zw;
    occ += aoTap(sampleUv, centerPos, normal, viewRadius, bias) * stepWeight;
    weight += stepWeight;

    return occ / max(weight, 0.0001);
}

float gtaoApprox(vec2 uv, float centerDepthRaw, vec2 pixel)
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
    float bias = 0.08;
    float angle = hash12(pixel) * 6.2831853;
    vec2 rot = vec2(cos(angle), sin(angle));
    vec2 d0 = rot;
    vec2 d1 = vec2(-rot.y, rot.x);
    vec2 d2 = normalize(d0 + d1);
    vec2 d3 = normalize(d0 - d1);

    float occ = 0.0;
    occ += gtaoDirection(uv, centerPos, normal,  d0, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal, -d0, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal,  d1, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal, -d1, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal,  d2, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal, -d2, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal,  d3, pixelRadius, viewRadius, bias);
    occ += gtaoDirection(uv, centerPos, normal, -d3, pixelRadius, viewRadius, bias);

    occ = saturate(occ * 0.85);
    float visibility = pow(saturate(1.0 - occ), max(intensity, 0.01));
    return clamp(visibility, 0.0, 1.0);
}

vec4 ssrTap(vec3 origin, vec3 rayDir, float rayDistance, float thickness)
{
    vec3 rayPos = origin + rayDir * rayDistance;
    if (rayPos.z <= 0.0) return vec4_splat(0.0);

    vec2 sampleUv = projectViewPos(rayPos);
    if (sampleUv.x <= 0.001 || sampleUv.y <= 0.001 || sampleUv.x >= 0.999 || sampleUv.y >= 0.999) {
        return vec4_splat(0.0);
    }

    float sampleDepthRaw = sampleDepth(sampleUv);
    if (sampleDepthRaw >= 0.9999) return vec4_splat(0.0);

    float sampleDepth = linearizeDepth(sampleDepthRaw);
    float depthError = abs(sampleDepth - rayPos.z);
    float hit = 1.0 - step(thickness, depthError);
    float edgeFade = saturate(min(min(sampleUv.x, sampleUv.y), min(1.0 - sampleUv.x, 1.0 - sampleUv.y)) * 24.0);
    float mask = texture2D(s_reflectionMask, sampleUv).x;
    float weight = hit * edgeFade * mask;
    return vec4(texture2D(s_sceneColor, sampleUv).rgb * weight, weight);
}

vec3 ssrApprox(vec2 uv, float centerDepthRaw, vec3 baseColor)
{
    float strength = u_screenSpaceParams.w;
    float mask = texture2D(s_reflectionMask, uv).x;
    if (strength <= 0.0 || mask <= 0.01 || centerDepthRaw >= 0.9999) return baseColor;

    float centerDepth = linearizeDepth(centerDepthRaw);
    vec3 centerPos = reconstructViewPos(uv, centerDepth);
    vec3 normal = sampleViewNormal(uv, centerPos);
    vec3 viewDir = normalize(centerPos);
    vec3 rayDir = normalize(reflect(viewDir, normal));
    if (rayDir.z <= 0.02) return baseColor;

    float grazingFade = saturate(1.0 - abs(dot(normal, -viewDir)));
    float thickness = max(centerDepth * u_postParams.w, 0.02);
    float maxDistance = max(u_screenSpaceParams.z, 0.1);
    vec4 hit = vec4_splat(0.0);
    vec4 tap = ssrTap(centerPos, rayDir, maxDistance * 0.023, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.047, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.073, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.107, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.150, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.207, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.280, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.373, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.487, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.627, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance * 0.800, thickness); hit += tap * (1.0 - hit.a);
    tap = ssrTap(centerPos, rayDir, maxDistance, thickness); hit += tap * (1.0 - hit.a);

    if (hit.a <= 0.001) return baseColor;
    vec3 reflection = hit.rgb / max(hit.a, 0.0001);
    float blend = saturate(strength * mask * hit.a * grazingFade);
    return mix(baseColor, reflection, blend);
}

void main()
{
    vec2 uv = clamp(v_texcoord0, vec2_splat(0.0), vec2_splat(1.0));
    vec3 color = texture2D(s_sceneColor, uv).rgb;
    vec2 pixel = uv * u_screenParams.xy;

    float centerDepth = sampleDepth(uv);
    color = dofApprox(uv, centerDepth, color);
    color *= gtaoApprox(uv, centerDepth, pixel);
    color = ssrApprox(uv, centerDepth, color);
    color += texture2D(s_bloomMask, uv).rgb * u_bloomParams.x;
    color = applyColorLut(color);

    float dist = distance(uv, vec2_splat(0.5));
    float vignette = smoothstep(0.35, 0.78, dist);
    color *= 1.0 - (vignette * u_postParams.x);

    float grain = hash12(pixel + vec2_splat(u_postParams.z * 60.0)) - 0.5;
    color += vec3_splat(grain * u_postParams.y);

    float temporalWeight = saturate(u_temporalParams.x) * saturate(u_temporalParams.z);
    if (temporalWeight > 0.0) {
        vec3 history = texture2D(s_historyColor, uv).rgb;
        vec3 minColor = min(color, history);
        vec3 maxColor = max(color, history);
        history = clamp(history, minColor - vec3_splat(0.08), maxColor + vec3_splat(0.08));
        color = mix(color, history, temporalWeight);
    }

    gl_FragColor = vec4(max(color, vec3_splat(0.0)), 1.0);
}
