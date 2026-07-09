SAMPLER2D(s_shadowMap, 3);

uniform mat4 u_shadowMatrix;
uniform vec4 u_shadowParams;

float shadowDepthAt(vec2 uv)
{
    return texture2D(s_shadowMap, clamp(uv, vec2_splat(0.0), vec2_splat(1.0))).x;
}

float shadowVisibility(vec3 worldPos)
{
    if (u_shadowParams.x <= 0.0) return 1.0;

    vec4 shadowPos = mul(u_shadowMatrix, vec4(worldPos, 1.0));
    shadowPos.xyz /= max(shadowPos.w, 0.0001);
    vec2 uv = shadowPos.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    float receiverDepth = shadowPos.z - u_shadowParams.z;
    if (uv.x <= 0.001 || uv.y <= 0.001 || uv.x >= 0.999 || uv.y >= 0.999 || receiverDepth <= 0.0 || receiverDepth >= 1.0) {
        return 1.0;
    }

    float texel = u_shadowParams.w;
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    float searchRadius = max(u_shadowParams.y * 16.0, 1.0);
    vec2 searchStep = vec2_splat(texel * searchRadius);

    float d = shadowDepthAt(uv + searchStep * vec2(-0.75, -0.75));
    float b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * vec2( 0.75, -0.75));
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * vec2(-0.75,  0.75));
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * vec2( 0.75,  0.75));
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;

    if (blockerCount <= 0.0) return 1.0;

    float avgBlocker = blockerSum / blockerCount;
    float penumbra = saturate((receiverDepth - avgBlocker) / max(avgBlocker, 0.0001)) * u_shadowParams.y * 96.0;
    float radius = max(penumbra, 1.0) * texel;
    float visibility = 0.0;
    visibility += step(receiverDepth, shadowDepthAt(uv));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 1.0,  0.0)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-1.0,  0.0)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.0,  1.0)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.0, -1.0)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.7,  0.7)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.7,  0.7)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.7, -0.7)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.7, -0.7)));

    return mix(0.35, 1.0, visibility / 9.0);
}
