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
// Trois tailles de groupe, choisies au démarrage selon ce que préfère le pilote (maxPreferredMeshWorkGroupInvocations) :
// 128 threads, un groupe par paquet ; 64 threads (PlanetMeshSmall.slang, FLUX_MESH_SMALL), chaque paquet en deux
// moitiés de 8 x 4 mailles, testées ensemble avec la boîte du paquet ; 256 threads (PlanetMeshLarge.slang,
// FLUX_MESH_LARGE), deux paquets l'un au-dessus de l'autre par groupe (8 x 16 mailles, 256 triangles) et les jupes
// dessinées à part : avec elles, un tel morceau dépasserait les 256 triangles qu'un groupe peut sortir.

#include "MeshCulling.slang"

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint TileQuarterIndices = 6 * (TileQuads / 2) * (TileQuads / 2) + 2 * 6 * (TileQuads / 2);
static const uint MeshletQuads = 8; // TileMeshletQuads
static const uint MeshletSide = TileQuads / MeshletQuads;
static const uint MeshletCount = MeshletSide * MeshletSide;
#if defined(FLUX_MESH_SMALL)
static const uint PieceRows = 2; // morceaux dessinés par paquet, l'un au-dessus de l'autre
static const uint MeshGroupSize = 64;
#elif defined(FLUX_MESH_LARGE)
static const uint PieceRows = 1;
static const uint MeshGroupSize = 256;
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

// La commande de la tuile que dessine la tâche `group` du lot.
struct TileCommand
{
    uint quarter; // quart dessiné (TileKey::Child), si la tuile n'est pas entière
    bool whole;
    uint firstVertex; // dans la page
    uint draw;        // DrawData de la tuile
};

TileCommand ReadTileCommand(uint group)
{
    const uint command = (params.draw + group) * 5;
    TileCommand tile;
    tile.whole = tileCommands[command] > TileQuarterIndices;
    tile.quarter = tileCommands[command + 2] / TileQuarterIndices; // tuile entière : 0, et indexCount le dit
    tile.firstVertex = tileCommands[command + 3];
    tile.draw = tileCommands[command + 4];
    return tile;
}

// Paquet `meshlet` à dessiner : dans la part dessinée de la tuile (un quart : ceux de l'enfant qui manque), dans le
// champ et pas caché.
bool MeshletVisible(TileCommand tile, uint meshlet)
{
    const uint mx = meshlet % MeshletSide;
    const uint my = meshlet / MeshletSide;
    const uint inQuarter = (mx >= MeshletSide / 2 ? 1u : 0u) + (my >= MeshletSide / 2 ? 2u : 0u);
    if (!tile.whole && inQuarter != tile.quarter) {
        return false;
    }
    const Meshlet box = pageMeshlets[tile.firstVertex / (TileSide * TileSide + 4 * TileSide) * MeshletCount + meshlet];
    const float3 local = (box.low.xyz + box.high.xyz) * 0.5;
    const float radius = length(box.high.xyz - box.low.xyz) * 0.5;
    const float3 center = Transform(draws[tile.draw].world, local).xyz; // sans échelle : le rayon ne change pas
    return InView(center, radius)
        && !Occluded(center + frame[0].cameraDelta.xyz, radius + length(frame[0].cameraDelta.xyz));
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

#ifdef FLUX_MESH_LARGE

// Morceaux d'une tuile : 8 de grille (une colonne de paquets, deux rangées), puis les jupes, par moitiés de bord
// (16 mailles, le côté d'un quart : un quart dessiné en garde deux), au plus SkirtsPerPiece par morceau.
static const uint GridPieces = MeshletCount / 2;
static const uint GridQuadsY = 2 * MeshletQuads;
static const uint GridSideY = GridQuadsY + 1;
static const uint GridVertices = PieceSideX * GridSideY;
static const uint GridTriangles = 2 * MeshletQuads * GridQuadsY;
static const uint SkirtCount = 8;
static const uint SkirtQuads = TileQuads / 2;
static const uint SkirtVertices = 2 * (SkirtQuads + 1); // le bord, puis sa jupe
static const uint SkirtTriangles = 2 * SkirtQuads;
static const uint SkirtsPerPiece = 7; // 238 sommets, 224 triangles
static const uint LargeMaxVertices = SkirtsPerPiece * SkirtVertices; // plus que GridVertices (153)
static const uint LargeMaxTriangles = GridTriangles;                 // plus que SkirtsPerPiece * SkirtTriangles

struct TilePayload
{
    uint draw;
    uint firstVertex;
    uint gridCount;                // morceaux de grille dessinés, en tête de pieces ; les suivants sont des jupes
    uint skirtCount;               // moitiés de bord dessinées, dans skirts
    uint pieces[GridPieces];       // colonne * 2 + rangée
    uint skirts[SkirtCount];       // bord * 2 + moitié
};

groupshared TilePayload payload;
groupshared uint visibleMeshlets;

// Les deux paquets du bord le long de cette moitié (bord * 2 + moitié), dans l'ordre des jupes : bas, droite, haut,
// gauche, chacun dans le sens trigonométrique (voir SkirtVertex).
uint2 SkirtMeshlets(uint skirt)
{
    const uint edge = skirt / 2;
    const uint first = 2 * (skirt % 2); // premier paquet le long du bord, dans le sens de la jupe
    const uint last = MeshletSide - 1;
    if (edge == 0) {
        return uint2(first, first + 1);
    }
    if (edge == 1) {
        return uint2(first * MeshletSide + last, (first + 1) * MeshletSide + last);
    }
    if (edge == 2) {
        return uint2(last * MeshletSide + last - first, last * MeshletSide + last - first - 1);
    }
    return uint2((last - first) * MeshletSide, (last - first - 1) * MeshletSide);
}

[shader("amplification")]
[numthreads(MeshletCount, 1, 1)]
void taskPlanet(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const TileCommand tile = ReadTileCommand(group.x);
    if (thread == 0) {
        visibleMeshlets = 0;
        payload.draw = tile.draw;
        payload.firstVertex = tile.firstVertex;
        payload.gridCount = 0;
        payload.skirtCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (MeshletVisible(tile, thread)) {
        InterlockedOr(visibleMeshlets, 1u << thread);
    }
    GroupMemoryBarrierWithGroupSync();

    // Un morceau se dessine si l'un de ses paquets est visible. Threads 0 à 7 : la grille ; 8 à 15 : les jupes.
    const uint visible = visibleMeshlets;
    uint slot;
    if (thread < GridPieces) {
        const uint column = thread / 2;
        const uint row = thread % 2;
        const uint bottom = 2 * row * MeshletSide + column;
        if ((visible & ((1u << bottom) | (1u << (bottom + MeshletSide)))) != 0) {
            InterlockedAdd(payload.gridCount, 1u, slot);
            payload.pieces[slot] = thread;
        }
    } else {
        const uint skirt = thread - GridPieces;
        const uint2 meshlets = SkirtMeshlets(skirt);
        if ((visible & ((1u << meshlets.x) | (1u << meshlets.y))) != 0) {
            InterlockedAdd(payload.skirtCount, 1u, slot);
            payload.skirts[slot] = skirt;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(payload.gridCount + (payload.skirtCount + SkirtsPerPiece - 1) / SkirtsPerPiece, 1, 1, payload);
}

// Sommet k (0 à SkirtQuads) du bord de la tuile le long de cette moitié, en haut (la grille) ou en bas (la jupe) :
// son indice dans la tuile, comme SkirtVertex et SkirtTop.
uint HalfEdgeVertex(uint skirt, uint k, bool low)
{
    const uint edge = skirt / 2;
    const uint along = (skirt % 2) * SkirtQuads + k;
    if (low) {
        return TileSide * TileSide + edge * TileSide + along;
    }
    const uint2 grid = edge == 0 ? uint2(along, 0) : edge == 1 ? uint2(TileQuads, along)
                     : edge == 2 ? uint2(TileQuads - along, TileQuads) : uint2(0, TileQuads - along);
    return grid.y * TileSide + grid.x;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MeshGroupSize, 1, 1)]
void meshPlanet(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload TilePayload tile,
                OutputVertices<PlanetVertexOutput, LargeMaxVertices> vertices,
                OutputIndices<uint3, LargeMaxTriangles> triangles)
{
    if (group.x < tile.gridCount) {
        // 8 x 16 mailles : un sommet et un triangle par thread.
        const uint piece = tile.pieces[group.x];
        const uint2 origin = uint2((piece / 2) * MeshletQuads, (piece % 2) * GridQuadsY);
        SetMeshOutputCounts(GridVertices, GridTriangles);
        if (thread < GridVertices) {
            const uint index = (origin.y + thread / PieceSideX) * TileSide + origin.x + thread % PieceSideX;
            vertices[thread] = PlanetVertex(tile.draw, ReadTileVertex(tile.firstVertex, index));
        }
        const uint quad = thread / 2;
        const uint a = (quad / MeshletQuads) * PieceSideX + quad % MeshletQuads;
        triangles[thread] = (thread & 1u) == 0 ? uint3(a, a + 1, a + PieceSideX + 1)
                                               : uint3(a, a + PieceSideX + 1, a + PieceSideX);
        return;
    }
    // Jupes : par moitié de bord, ses SkirtQuads + 1 sommets du bord puis autant de la jupe, et comme BuildTileIndices
    // (bord, jupe, bord suivant) puis (bord suivant, jupe, jupe suivante).
    const uint first = (group.x - tile.gridCount) * SkirtsPerPiece;
    const uint count = min(SkirtsPerPiece, tile.skirtCount - first);
    SetMeshOutputCounts(count * SkirtVertices, count * SkirtTriangles);
    if (thread < count * SkirtVertices) {
        const uint skirt = tile.skirts[first + thread / SkirtVertices];
        const uint v = thread % SkirtVertices;
        const uint index = HalfEdgeVertex(skirt, v % (SkirtQuads + 1), v > SkirtQuads);
        vertices[thread] = PlanetVertex(tile.draw, ReadTileVertex(tile.firstVertex, index));
    }
    if (thread < count * SkirtTriangles) {
        const uint top = (thread / SkirtTriangles) * SkirtVertices;
        const uint t = thread % SkirtTriangles;
        const uint k = t / 2;
        const uint low = top + SkirtQuads + 1;
        triangles[thread] = (t & 1u) == 0 ? uint3(top + k, low + k, top + k + 1) : uint3(top + k + 1, low + k, low + k + 1);
    }
}

#else

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
    const TileCommand tile = ReadTileCommand(group.x);
    if (thread == 0) {
        visibleCount = 0;
        payload.draw = tile.draw;
        payload.firstVertex = tile.firstVertex;
    }
    GroupMemoryBarrierWithGroupSync();

    if (MeshletVisible(tile, thread)) {
        uint slot;
        InterlockedAdd(visibleCount, PieceRows, slot);
        for (uint row = 0; row < PieceRows; ++row) {
            payload.pieces[slot + row] = thread * PieceRows + row;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payload);
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

#endif
