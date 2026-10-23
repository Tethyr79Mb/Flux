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
// Deux tailles de groupe, choisies au démarrage selon ce que préfère le pilote (maxPreferredMeshWorkGroupInvocations) :
// 128 threads, un groupe par paquet ; 64 threads (PlanetMeshSmall.slang, FLUX_MESH_SMALL), chaque paquet en deux
// moitiés de 8 x 4 mailles, testées ensemble avec la boîte du paquet.

#include "MeshCulling.slang"

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint TileQuarterIndices = 6 * (TileQuads / 2) * (TileQuads / 2) + 2 * 6 * (TileQuads / 2);
static const uint MeshletQuads = 8; // TileMeshletQuads
static const uint MeshletSide = TileQuads / MeshletQuads;
static const uint MeshletCount = MeshletSide * MeshletSide;
#ifdef FLUX_MESH_SMALL
static const uint PieceRows = 2; // morceaux dessinés par paquet, l'un au-dessus de l'autre
static const uint MeshGroupSize = 64;
#else
static const uint PieceRows = 1;
static const uint MeshGroupSize = 128;
#endif
static const uint PieceQuadsX = MeshletQuads;
static const uint PieceQuadsY = MeshletQuads / PieceRows;
static const uint PieceSideX = PieceQuadsX + 1;
static const uint PieceSideY = PieceQuadsY + 1;
static const uint PieceVertices = PieceSideX * PieceSideY;
static const uint PieceTriangles = 2 * PieceQuadsX * PieceQuadsY;
// Un coin : un bout de jupe en largeur et un en hauteur.
static const uint MaxVertices = PieceVertices + PieceSideX + PieceSideY;
static const uint MaxTriangles = PieceTriangles + 2 * PieceQuadsX + 2 * PieceQuadsY;

// Doit correspondre à Planetology::PlanetTileCompute::Meshlet.
struct Meshlet
{
    float4 low;
    float4 high;
};

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
    uint pieces[MeshletCount * PieceRows]; // morceaux à dessiner : paquet * PieceRows + rangée
};

groupshared TilePayload payload;
groupshared uint visibleCount;

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
        InterlockedAdd(visibleCount, PieceRows, slot);
        for (uint row = 0; row < PieceRows; ++row) {
            payload.pieces[slot + row] = thread * PieceRows + row;
        }
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

// Le morceau (origine en mailles dans la tuile) touche ce bord de la tuile, dans l'ordre des jupes : bas, droite, haut,
// gauche.
bool OnEdge(uint edge, uint2 origin)
{
    return edge == 0 ? origin.y == 0 : edge == 1 ? origin.x + PieceQuadsX == TileQuads
         : edge == 2 ? origin.y + PieceQuadsY == TileQuads : origin.x == 0;
}

// Mailles du morceau le long de ce bord.
uint EdgeQuads(uint edge)
{
    return (edge & 1u) == 0 ? PieceQuadsX : PieceQuadsY;
}

// Sommet k (0 à EdgeQuads) du bout de jupe du morceau sur ce bord : son indice dans la tuile. Les jupes suivent le bord
// dans le sens trigonométrique (bas : s croissant, droite : t croissant, haut : s décroissant, gauche : t décroissant),
// comme BuildTile.
uint SkirtVertex(uint edge, uint2 origin, uint k)
{
    const uint along = edge == 0 ? origin.x : edge == 1 ? origin.y
                     : edge == 2 ? TileQuads - origin.x - PieceQuadsX : TileQuads - origin.y - PieceQuadsY;
    return TileSide * TileSide + edge * TileSide + along + k;
}

// Le sommet de la grille du morceau sous le sommet k de sa jupe sur ce bord (indice dans le morceau).
uint SkirtTop(uint edge, uint k)
{
    return edge == 0 ? k : edge == 1 ? k * PieceSideX + PieceQuadsX
         : edge == 2 ? PieceQuadsY * PieceSideX + (PieceQuadsX - k) : (PieceQuadsY - k) * PieceSideX;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MeshGroupSize, 1, 1)]
void meshPlanet(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload TilePayload tile,
                OutputVertices<PlanetVertexOutput, MaxVertices> vertices, OutputIndices<uint3, MaxTriangles> triangles)
{
    const uint piece = tile.pieces[group.x];
    const uint meshlet = piece / PieceRows;
    const uint2 origin = uint2((meshlet % MeshletSide) * MeshletQuads,
                               (meshlet / MeshletSide) * MeshletQuads + (piece % PieceRows) * PieceQuadsY);
    // Bouts de jupe du morceau, rangés après sa grille dans l'ordre des bords (au plus deux : un coin).
    uint skirtEdges[2] = { 0, 0 };
    uint skirtCount = 0;
    for (uint edge = 0; edge < 4; ++edge) {
        if (OnEdge(edge, origin)) {
            skirtEdges[skirtCount++] = edge;
        }
    }
    const uint firstQuads = skirtCount > 0 ? EdgeQuads(skirtEdges[0]) : 0;
    const uint secondQuads = skirtCount > 1 ? EdgeQuads(skirtEdges[1]) : 0;
    const uint secondSkirt = PieceVertices + (skirtCount > 0 ? firstQuads + 1 : 0);
    const uint vertexCount = secondSkirt + (skirtCount > 1 ? secondQuads + 1 : 0);
    const uint triangleCount = PieceTriangles + 2 * (firstQuads + secondQuads);
    SetMeshOutputCounts(vertexCount, triangleCount);

    for (uint v = thread; v < vertexCount; v += MeshGroupSize) {
        uint index;
        if (v < PieceVertices) {
            index = (origin.y + v / PieceSideX) * TileSide + origin.x + v % PieceSideX;
        } else if (v < secondSkirt) {
            index = SkirtVertex(skirtEdges[0], origin, v - PieceVertices);
        } else {
            index = SkirtVertex(skirtEdges[1], origin, v - secondSkirt);
        }
        vertices[v] = PlanetVertex(tile.draw, ReadTileVertex(tile.firstVertex, index));
    }
    // Comme BuildTileIndices : (a, b, c) puis (a, c, d) par maille ; par bout de jupe, (bord, jupe, bord suivant) puis
    // (bord suivant, jupe, jupe suivante).
    for (uint t = thread; t < triangleCount; t += MeshGroupSize) {
        if (t < PieceTriangles) {
            const uint quad = t / 2;
            const uint a = (quad / PieceQuadsX) * PieceSideX + quad % PieceQuadsX;
            triangles[t] = (t & 1u) == 0 ? uint3(a, a + 1, a + PieceSideX + 1) : uint3(a, a + PieceSideX + 1, a + PieceSideX);
        } else {
            uint skirt = t - PieceTriangles;
            const uint s = skirt < 2 * firstQuads ? 0u : 1u;
            skirt -= s * 2 * firstQuads;
            const uint k = skirt / 2;
            const uint edge = skirtEdges[s];
            const uint low = (s == 0 ? PieceVertices : secondSkirt) + k;
            triangles[t] = (skirt & 1u) == 0 ? uint3(SkirtTop(edge, k), low, SkirtTop(edge, k + 1))
                                             : uint3(SkirtTop(edge, k + 1), low, low + 1);
        }
    }
}
