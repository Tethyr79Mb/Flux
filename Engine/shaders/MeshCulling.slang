// Tests des mesh shaders (sol des planètes, PlanetMesh.slang ; modèles, ModelMesh.slang) pour écarter un paquet
// avant de le dessiner : hors du champ, ou caché derrière ce que voyait l'image précédente (pyramide de profondeur
// d'OcclusionCulling, liée au groupe de l'image quand elle sert).

#include "Mesh.slang"

[[vk::binding(10, 1)]] StructuredBuffer<float> depthPyramid; // OcclusionCulling, celle de l'image précédente

// La sphère (relative à la caméra) touche le champ : côtés et plan proche (reverse-Z, sans plan lointain).
bool InView(float3 center, float radius)
{
    const float4 columns[4] = frame[0].viewProjection;
    const float4 x = float4(columns[0].x, columns[1].x, columns[2].x, columns[3].x);
    const float4 y = float4(columns[0].y, columns[1].y, columns[2].y, columns[3].y);
    const float4 z = float4(columns[0].z, columns[1].z, columns[2].z, columns[3].z);
    const float4 w = float4(columns[0].w, columns[1].w, columns[2].w, columns[3].w);
    const float4 planes[5] = { w + x, w - x, w + y, w - y, w - z };
    for (uint p = 0; p < 5; ++p) {
        if (dot(planes[p].xyz, center) + planes[p].w < -radius * length(planes[p].xyz)) {
            return false;
        }
    }
    return true;
}

uint PyramidOffset(uint level)
{
    return frame[0].pyramidOffsets[level / 4][level % 4];
}

// Taille du niveau `level` : la source divisée par 2^(level + 1), arrondie au-dessus.
uint2 PyramidLevelSize(uint level)
{
    const uint2 source = frame[0].pyramidSize.xy;
    return (source + ((2u << level) - 1u)) >> (level + 1u);
}

// La sphère (relative à la caméra de l'image précédente) est derrière tout ce que l'image précédente voyait à sa
// place : comme Occluded dans shaders/Culling.slang. Sans pyramide (pyramidSize.w à 0), jamais.
bool Occluded(float3 center, float radius)
{
    if (frame[0].pyramidSize.w == 0) {
        return false;
    }
    float2 low = float2(1.0e30, 1.0e30);
    float2 high = float2(-1.0e30, -1.0e30);
    float nearest = 0.0;
    for (uint corner = 0; corner < 8; ++corner) {
        const float3 offset = float3((corner & 1u) != 0 ? radius : -radius, (corner & 2u) != 0 ? radius : -radius,
                                     (corner & 4u) != 0 ? radius : -radius);
        const float4 clip = Transform(frame[0].previousViewProjection, center + offset);
        if (clip.w <= 0.0 || clip.z >= clip.w) {
            return false; // la caméra est dans la boîte
        }
        const float2 ndc = clip.xy / clip.w;
        low = min(low, ndc);
        high = max(high, ndc);
        nearest = max(nearest, clip.z / clip.w);
    }
    const float2 uvLow = low * 0.5 + 0.5;
    const float2 uvHigh = high * 0.5 + 0.5;
    if (any(uvLow < 0.0) || any(uvHigh > 1.0)) {
        return false; // hors de l'image précédente : rien ne dit ce qui la cachait
    }
    const uint2 source = frame[0].pyramidSize.xy;
    const uint2 pixelLow = uint2(max(int2(uvLow * float2(source)) - 1, int2(0, 0)));
    const uint2 pixelHigh = min(uint2(uvHigh * float2(source)) + 1u, source - 1u);
    uint level = 0;
    while (level < frame[0].pyramidSize.z) {
        const uint2 a = pixelLow >> (level + 1u);
        const uint2 b = pixelHigh >> (level + 1u);
        if (b.x - a.x <= 1u && b.y - a.y <= 1u) {
            break;
        }
        ++level;
    }
    if (level >= frame[0].pyramidSize.z) {
        return false;
    }
    const uint2 size = PyramidLevelSize(level);
    const uint2 a = min(pixelLow >> (level + 1u), size - 1u);
    const uint2 b = min(pixelHigh >> (level + 1u), size - 1u);
    const uint offset = PyramidOffset(level);
    const float farthest = min(min(depthPyramid[offset + a.y * size.x + a.x], depthPyramid[offset + a.y * size.x + b.x]),
                               min(depthPyramid[offset + b.y * size.x + a.x], depthPyramid[offset + b.y * size.x + b.x]));
    return nearest < farthest;
}
