// Test commun des sphères contre la pyramide de profondeur d'OcclusionCulling (celle de l'image précédente) : entités
// (Culling.slang), paquets du sol et des modèles (MeshCulling.slang), arbres des planètes (Vegetation.slang). Chaque
// texel garde la plus lointaine des profondeurs qu'il couvre (reverse-Z : la plus petite).

// Taille du niveau `level` : la source divisée par 2^(level + 1), arrondie au-dessus.
uint2 PyramidLevelSize(uint2 source, uint level)
{
    return (source + ((2u << level) - 1u)) >> (level + 1u);
}

// La sphère (relative à la caméra de l'image précédente) est derrière tout ce que l'image précédente voyait à sa place.
// viewProjection : de l'image précédente, par colonnes ; size : x, y taille de la profondeur source, z niveaux ;
// offsets : premier texel de chaque niveau dans `pyramid`.
bool OccludedInPyramid(StructuredBuffer<float> pyramid, float4 viewProjection[4], uint4 size, uint4 offsets[4],
                       float3 center, float radius)
{
    // Les 8 coins de la boîte autour de la sphère, dans l'image précédente : rectangle à l'écran et point le plus
    // proche. Un coin devant le plan proche (caméra dans la boîte) : visible.
    float2 low = float2(1.0e30, 1.0e30);
    float2 high = float2(-1.0e30, -1.0e30);
    float nearest = 0.0;
    for (uint corner = 0; corner < 8; ++corner) {
        const float3 p = center + float3((corner & 1u) != 0 ? radius : -radius, (corner & 2u) != 0 ? radius : -radius,
                                         (corner & 4u) != 0 ? radius : -radius);
        const float4 clip = viewProjection[0] * p.x + viewProjection[1] * p.y + viewProjection[2] * p.z + viewProjection[3];
        if (clip.w <= 0.0 || clip.z >= clip.w) {
            return false;
        }
        const float2 ndc = clip.xy / clip.w;
        low = min(low, ndc);
        high = max(high, ndc);
        nearest = max(nearest, clip.z / clip.w);
    }
    // Hors de l'image précédente, même en partie : rien ne dit ce qui la cachait là.
    const float2 uvLow = low * 0.5 + 0.5;
    const float2 uvHigh = high * 0.5 + 0.5;
    if (any(uvLow < 0.0) || any(uvHigh > 1.0)) {
        return false;
    }
    // Pixels couverts, élargis d'un pixel (l'image précédente était décalée d'un sous-pixel par l'anticrénelage).
    const uint2 source = size.xy;
    const uint2 pixelLow = uint2(max(int2(uvLow * float2(source)) - 1, int2(0, 0)));
    const uint2 pixelHigh = min(uint2(uvHigh * float2(source)) + 1u, source - 1u);
    // Le niveau le plus fin où le rectangle tient dans 2 x 2 texels : le texel t du niveau k couvre les pixels
    // t x 2^(k+1) à (t + 1) x 2^(k+1) - 1.
    uint level = 0;
    while (level < size.z) {
        const uint2 a = pixelLow >> (level + 1u);
        const uint2 b = pixelHigh >> (level + 1u);
        if (b.x - a.x <= 1u && b.y - a.y <= 1u) {
            break;
        }
        ++level;
    }
    if (level >= size.z) {
        return false;
    }
    const uint2 levelSize = PyramidLevelSize(source, level);
    const uint2 a = min(pixelLow >> (level + 1u), levelSize - 1u);
    const uint2 b = min(pixelHigh >> (level + 1u), levelSize - 1u);
    const uint offset = offsets[level / 4][level % 4];
    const float farthest = min(min(pyramid[offset + a.y * levelSize.x + a.x], pyramid[offset + a.y * levelSize.x + b.x]),
                               min(pyramid[offset + b.y * levelSize.x + a.x], pyramid[offset + b.y * levelSize.x + b.x]));
    return nearest < farthest;
}
