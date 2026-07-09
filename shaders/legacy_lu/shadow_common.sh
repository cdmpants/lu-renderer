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
    float lightRadius = max(u_shadowParams.y, 0.0);
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    float searchRadius = max(1.0, lightRadius * 96.0);
    vec2 searchStep = vec2_splat(texel * searchRadius);

    vec2 poisson0 = vec2(-0.326, -0.406);
    vec2 poisson1 = vec2(-0.840, -0.074);
    vec2 poisson2 = vec2(-0.696,  0.457);
    vec2 poisson3 = vec2(-0.203,  0.621);
    vec2 poisson4 = vec2( 0.962, -0.195);
    vec2 poisson5 = vec2( 0.473, -0.480);
    vec2 poisson6 = vec2( 0.519,  0.767);
    vec2 poisson7 = vec2( 0.185, -0.893);

    float d = shadowDepthAt(uv + searchStep * poisson0);
    float b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson1);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson2);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson3);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson4);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson5);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson6);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;
    d = shadowDepthAt(uv + searchStep * poisson7);
    b = 1.0 - step(receiverDepth, d); blockerSum += d * b; blockerCount += b;

    if (blockerCount <= 0.0) return 1.0;

    float avgBlocker = blockerSum / blockerCount;
    float receiverGap = saturate((receiverDepth - avgBlocker) * 64.0);
    float radiusTexels = max(1.0, lightRadius * 192.0 * (0.35 + receiverGap));
    float radius = radiusTexels * texel;
    float visibility = 0.0;
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson0));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson1));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson2));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson3));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson4));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson5));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson6));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * poisson7));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.507,  0.064)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.896,  0.412)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.322, -0.933)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.792, -0.598)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.842,  0.800)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2(-0.105, -0.025)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.746, -0.667)));
    visibility += step(receiverDepth, shadowDepthAt(uv + radius * vec2( 0.390,  0.025)));

    return mix(0.25, 1.0, visibility / 16.0);
}
