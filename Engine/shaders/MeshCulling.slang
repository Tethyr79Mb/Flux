// Tests des mesh shaders (sol des planètes, PlanetMesh.slang ; modèles, ModelMesh.slang) pour écarter un paquet
// avant de le dessiner : hors du champ, ou caché derrière ce que voyait l'image précédente (pyramide de profondeur
// d'OcclusionCulling, liée au groupe de l'image quand elle sert).

#include "DepthPyramid.slang"
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

// La sphère (relative à la caméra de l'image précédente) est derrière tout ce que l'image précédente voyait à sa
// place (DepthPyramid.slang). Sans pyramide (pyramidSize.w à 0), jamais.
bool Occluded(float3 center, float radius)
{
    if (frame[0].pyramidSize.w == 0) {
        return false;
    }
    return OccludedInPyramid(depthPyramid, frame[0].previousViewProjection, frame[0].pyramidSize, frame[0].pyramidOffsets,
                             center, radius);
}
