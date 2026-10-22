// Sol des planètes en mesh shaders (VK_EXT_mesh_shader), à la place de vertexPlanet quand le GPU les a (RTX 20,
// RX 6000, Arc et plus récentes) : même image, mais le GPU écarte lui-même les morceaux de tuile qu'il ne verra pas.
// Chaque tuile se découpe en 4 x 4 paquets de 8 x 8 mailles (128 triangles, plus leurs bouts de jupe) dont la boîte
// est calculée avec la tuile (PlanetTile.slang). Une tâche (taskPlanet) par tuile dessinée teste ses paquets : hors
// du champ, ou cachés derrière ce que voyait l'image précédente (pyramide de profondeur d'OcclusionCulling, comme
// shaders/Culling.slang) ; les paquets restants sont dessinés par meshPlanet, avec les mêmes sommets et le même sol
// que vertexPlanet (PlanetVertex, fragmentPlanet).
// Pas de test des faces de dos : le sol regarde vers le haut, vers la caméra, un paquet entier de dos est rare.
// Limite : ce qu'une montagne découvre d'une image à l'autre apparaît une image plus tard ; la sphère testée grandit du
// déplacement de la caméra pour que cela reste rare.
// Compilé à part de Mesh.slang (qu'il inclut) : un module qui déclare les mesh shaders ne se charge pas sur un GPU qui
// ne les a pas.

#include "Mesh.slang"

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint TileQuarterIndices = 6 * (TileQuads / 2) * (TileQuads / 2) + 2 * 6 * (TileQuads / 2);
static const uint MeshletQuads = 8; // TileMeshletQuads
static const uint MeshletSide = TileQuads / MeshletQuads;
static const uint MeshletCount = MeshletSide * MeshletSide;
static const uint MeshletVertices = (MeshletQuads + 1) * (MeshletQuads + 1);
static const uint MaxVertices = MeshletVertices + 2 * (MeshletQuads + 1);          // un coin : deux bouts de jupe
static const uint MaxTriangles = 2 * MeshletQuads * MeshletQuads + 2 * 2 * MeshletQuads;
static const uint MeshGroupSize = 128;

// Doit correspondre à Planetology::PlanetTileCompute::Meshlet.
struct Meshlet
{
    float4 low;
    float4 high;
};

[[vk::binding(10, 1)]] StructuredBuffer<float> tilePyramid; // OcclusionCulling, celle de l'image précédente
// Commandes des tuiles de l'image (RHI::DrawIndexedCommand, 5 uint : indexCount, instanceCount, firstIndex,
// vertexOffset, firstInstance), les mêmes que pour vertexPlanet ; params.draw : première du lot.
[[vk::binding(11, 1)]] StructuredBuffer<uint> tileCommands;
// Page de tuiles (PlanetTileCompute) : ModelVertex (8 floats), MorphVertex (6 floats), boîtes des paquets.
[[vk::binding(0, 3)]] StructuredBuffer<float> pageVertices;
[[vk::binding(1, 3)]] StructuredBuffer<float> pageMorph;
[[vk::binding(2, 3)]] StructuredBuffer<Meshlet> pageMeshlets;

struct TilePayload
{
    uint draw;        // DrawData de la tuile
    uint firstVertex; // dans la page
    uint meshlets[MeshletCount]; // paquets à dessiner (0 à MeshletCount - 1)
};

groupshared TilePayload payload;
groupshared uint visibleCount;

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
// place : comme Occluded dans shaders/Culling.slang.
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
    const float farthest = min(min(tilePyramid[offset + a.y * size.x + a.x], tilePyramid[offset + a.y * size.x + b.x]),
                               min(tilePyramid[offset + b.y * size.x + a.x], tilePyramid[offset + b.y * size.x + b.x]));
    return nearest < farthest;
}

[shader("amplification")]
[numthreads(MeshletCount, 1, 1)]
void taskPlanet(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const uint command = (params.draw + group.x) * 5;
    const uint indexCount = tileCommands[command];
    const uint quarter = tileCommands[command + 2] / TileQuarterIndices; // tuile entière : 0, et indexCount le dit
    const bool whole = indexCount > TileQuarterIndices;
    const uint firstVertex = tileCommands[command + 3];
    const uint drawIndex = tileCommands[command + 4];
    if (thread == 0) {
        visibleCount = 0;
        payload.draw = drawIndex;
        payload.firstVertex = firstVertex;
    }
    GroupMemoryBarrierWithGroupSync();

    // Paquet `thread`, s'il est dans la part dessinée de la tuile (un quart : ceux de l'enfant qui manque).
    const uint mx = thread % MeshletSide;
    const uint my = thread / MeshletSide;
    const uint inQuarter = (mx >= MeshletSide / 2 ? 1u : 0u) + (my >= MeshletSide / 2 ? 2u : 0u);
    bool visible = whole || inQuarter == quarter;
    if (visible) {
        const Meshlet meshlet = pageMeshlets[firstVertex / (TileSide * TileSide + 4 * TileSide) * MeshletCount + thread];
        const float3 local = (meshlet.low.xyz + meshlet.high.xyz) * 0.5;
        const float radius = length(meshlet.high.xyz - meshlet.low.xyz) * 0.5;
        const float3 center = Transform(draws[drawIndex].world, local).xyz; // sans échelle : le rayon ne change pas
        visible = InView(center, radius)
               && !Occluded(center + frame[0].cameraDelta.xyz, radius + length(frame[0].cameraDelta.xyz));
    }
    if (visible) {
        uint slot;
        InterlockedAdd(visibleCount, 1u, slot);
        payload.meshlets[slot] = thread;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payload);
}

// Sommet `index` de la tuile (grille, puis jupes : voir BuildTileIndices) dans sa page.
PlanetVertexInput ReadTileVertex(uint firstVertex, uint index)
{
    const uint v = (firstVertex + index) * 8;
    const uint m = (firstVertex + index) * 6;
    PlanetVertexInput input;
    input.position = float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
    input.normal = float3(pageVertices[v + 3], pageVertices[v + 4], pageVertices[v + 5]);
    input.uv = float2(pageVertices[v + 6], pageVertices[v + 7]);
    input.morphOffset = float3(pageMorph[m], pageMorph[m + 1], pageMorph[m + 2]);
    input.morphNormal = float3(pageMorph[m + 3], pageMorph[m + 4], pageMorph[m + 5]);
    return input;
}

// Bords de la tuile que touche le paquet (mx, my), dans l'ordre des jupes : bas, droite, haut, gauche.
bool OnEdge(uint edge, uint mx, uint my)
{
    return edge == 0 ? my == 0 : edge == 1 ? mx == MeshletSide - 1 : edge == 2 ? my == MeshletSide - 1 : mx == 0;
}

// Sommet k (0 à MeshletQuads) du bout de jupe du paquet sur ce bord : son indice dans la tuile. Les jupes suivent le
// bord dans le sens trigonométrique (bas : s croissant, droite : t croissant, haut : s décroissant, gauche : t
// décroissant), comme BuildTile.
uint SkirtVertex(uint edge, uint mx, uint my, uint k)
{
    const uint along = edge == 0 ? mx * MeshletQuads : edge == 1 ? my * MeshletQuads
                     : edge == 2 ? (MeshletSide - 1 - mx) * MeshletQuads : (MeshletSide - 1 - my) * MeshletQuads;
    return TileSide * TileSide + edge * TileSide + along + k;
}

// Le sommet de la grille du paquet sous le sommet k de sa jupe sur ce bord (indice dans le paquet).
uint SkirtTop(uint edge, uint k)
{
    const uint side = MeshletQuads + 1;
    return edge == 0 ? k : edge == 1 ? k * side + MeshletQuads : edge == 2 ? MeshletQuads * side + (MeshletQuads - k)
                                                                          : (MeshletQuads - k) * side;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MeshGroupSize, 1, 1)]
void meshPlanet(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload TilePayload tile,
                OutputVertices<PlanetVertexOutput, MaxVertices> vertices, OutputIndices<uint3, MaxTriangles> triangles)
{
    const uint meshlet = tile.meshlets[group.x];
    const uint mx = meshlet % MeshletSide;
    const uint my = meshlet / MeshletSide;
    const uint side = MeshletQuads + 1;
    // Bouts de jupe du paquet, rangés après sa grille dans l'ordre des bords.
    uint skirtEdges[2] = { 0, 0 };
    uint skirtCount = 0;
    for (uint edge = 0; edge < 4; ++edge) {
        if (OnEdge(edge, mx, my)) {
            skirtEdges[skirtCount++] = edge;
        }
    }
    const uint vertexCount = MeshletVertices + skirtCount * side;
    const uint triangleCount = 2 * MeshletQuads * MeshletQuads + skirtCount * 2 * MeshletQuads;
    SetMeshOutputCounts(vertexCount, triangleCount);

    for (uint v = thread; v < vertexCount; v += MeshGroupSize) {
        uint index;
        if (v < MeshletVertices) {
            index = (my * MeshletQuads + v / side) * TileSide + mx * MeshletQuads + v % side;
        } else {
            const uint s = (v - MeshletVertices) / side;
            index = SkirtVertex(skirtEdges[s], mx, my, (v - MeshletVertices) % side);
        }
        vertices[v] = PlanetVertex(tile.draw, ReadTileVertex(tile.firstVertex, index));
    }
    // Comme BuildTileIndices : (a, b, c) puis (a, c, d) par maille ; par bout de jupe, (bord, jupe, bord suivant) puis
    // (bord suivant, jupe, jupe suivante).
    for (uint t = thread; t < triangleCount; t += MeshGroupSize) {
        if (t < 2 * MeshletQuads * MeshletQuads) {
            const uint quad = t / 2;
            const uint a = (quad / MeshletQuads) * side + quad % MeshletQuads;
            triangles[t] = (t & 1u) == 0 ? uint3(a, a + 1, a + side + 1) : uint3(a, a + side + 1, a + side);
        } else {
            const uint skirt = t - 2 * MeshletQuads * MeshletQuads;
            const uint s = skirt / (2 * MeshletQuads);
            const uint k = (skirt % (2 * MeshletQuads)) / 2;
            const uint edge = skirtEdges[s];
            const uint low = MeshletVertices + s * side + k;
            triangles[t] = (skirt & 1u) == 0 ? uint3(SkirtTop(edge, k), low, SkirtTop(edge, k + 1))
                                             : uint3(SkirtTop(edge, k + 1), low, low + 1);
        }
    }
}
