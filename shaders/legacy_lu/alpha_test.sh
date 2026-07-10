// x: enabled, y: reference [0,1], z: NiAlphaProperty TestFunction
uniform vec4 u_luAlphaTest;

bool luAlphaTestPass(float alpha)
{
    if (u_luAlphaTest.x < 0.5) return true;

    float reference = u_luAlphaTest.y;
    float comparison = u_luAlphaTest.z;
    if (comparison < 0.5) return true;
    if (comparison < 1.5) return alpha < reference;
    if (comparison < 2.5) return alpha == reference;
    if (comparison < 3.5) return alpha <= reference;
    if (comparison < 4.5) return alpha > reference;
    if (comparison < 5.5) return alpha != reference;
    if (comparison < 6.5) return alpha >= reference;
    return false;
}
