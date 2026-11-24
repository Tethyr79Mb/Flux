// Herbe des planètes en mesh shaders (voir Grass.slang), quand le GPU les a : une tâche (taskGrass) par tuile proche
// teste ses 64 carrés de 4 x 4 mailles : hors du champ, caché derrière ce que voyait l'image précédente, sans herbe
// (aux coins et au centre), ou trop loin pour garder un brin, il est écarté. Chaque carré retenu envoie ses brins à
// meshGrass par groupes de 32 (un thread par brin), en 3 segments de près, 2 puis 1 au loin, avec son climat calculé une
// fois (GrassClimate).
// Des carrés de 4 x 4 et non les paquets de 8 x 8 du sol : un paquet à cheval sur le bord de l'image ou derrière un
// arbre fabriquait tous ses brins, jusqu'à quatre fois trop (regard vers le ciel : l'herbe des pieds coûtait encore la
// moitié de son prix). Les bornes d'un carré viennent de ses 25 sommets (le sol est plan entre eux).
// Compilé à part de Grass.slang (qu'il inclut) : un module qui déclare les mesh shaders ne se charge pas sur un GPU qui
// ne les a pas.

#include "Grass.slang"

static const uint GrassGroupBlades = 32;
static const uint GrassMaxSegments = 3;
// Sorties d'un groupe : ses brins, plus la place des corolles de près (GrassCorollaVertex) au-delà des brins qu'elles
// remplacent ; 256 au plus (limite garantie des mesh shaders, et taille préférée des GPU AMD). Une fleur qui ne tient plus
// (taches très fleuries) garde sa tête en losange.
static const uint GrassMaxVertices = 256;
static const uint GrassMaxTriangles = 256;
static const uint GrassPatchQuads = 4;
static const uint GrassPatchSide = GrassTileQuads / GrassPatchQuads;
static const uint GrassPatchCount = GrassPatchSide * GrassPatchSide; // un thread de la tâche par carré
// Bornes d'un carré : sur une tuile fine (1,5 m entre sommets) il couvre ~36 m², soit ~7 000 brins à la densité par
// défaut ; sur la plus grande qui en porte (MeshRenderer : GrassMaxTileEdge, 320 m), 1 600 m², mais au loin, où il n'en
// garde qu'une part. Une aire plus grande ou non finie (sommets pas encore calculés) l'écarte, et ses brins restent
// bornés : sans elles, des milliards de brins partaient au pilote (délai dépassé, GPU perdu).
static const float GrassMaxPatchArea = 2500.0;
static const uint GrassMaxPatchBlades = 16384; // 512 groupes : 64 carrés restent sous les 65 535 d'une tâche

// Commandes des tuiles de l'image (5 uint chacune : indexCount, instanceCount, firstIndex, vertexOffset,
// firstInstance) ; params.draw : première du lot. Pour l'herbe, firstIndex dit le quart dessiné (1 à 4, 0 : la tuile
// entière) : une tuile dont une partie est remplacée par ses filles ne garde que les carrés de ses autres quarts.
[[vk::binding(11, 1)]] StructuredBuffer<uint> tileCommands;

struct GrassPayload
{
    uint draw;        // DrawData de la tuile
    uint firstVertex; // dans la page
    uint seed;        // GrassSeed
    uint count;       // carrés retenus
    // Par carré retenu : x, carré + segments x 256 ; y, son premier groupe ; z, brins gardés ; w, brins à pleine
    // densité (bits d'un float).
    uint4 patches[GrassPatchCount];
    float3 climates[GrassPatchCount]; // GrassClimate au centre du carré
};

groupshared GrassPayload grass;
groupshared uint grassCount;
groupshared uint grassGroups;

[shader("amplification")]
[numthreads(GrassPatchCount, 1, 1)]
void taskGrass(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const uint command = (params.draw + group.x) * 5;
    const uint tileInPage = tileCommands[command + 3] >> GrassVertexShift;
    const uint drawIndex = tileCommands[command + 4] >> GrassInstanceShift;
    const uint firstVertex = tileInPage * GrassTileVertices;
    const uint quarter = tileCommands[command + 2];
    if (thread == 0) {
        grass.draw = drawIndex;
        grass.firstVertex = firstVertex;
        grass.seed = GrassSeed(drawIndex);
        grassCount = 0;
        grassGroups = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Carré `thread` : la boîte de ses sommets, grandie de la hauteur des brins les plus hauts et penchés.
    const uint2 origin = uint2(thread % GrassPatchSide, thread / GrassPatchSide) * GrassPatchQuads;
    float3 low = float3(1.0e30);
    float3 high = float3(-1.0e30);
    for (uint y = 0; y <= GrassPatchQuads; ++y) {
        for (uint x = 0; x <= GrassPatchQuads; ++x) {
            const float3 p = ReadGrassPosition(drawIndex, firstVertex, (origin.y + y) * GrassTileSide + origin.x + x);
            low = min(low, p);
            high = max(high, p);
        }
    }
    const float3 local = (low + high) * 0.5;
    const float radius = length(high - low) * 0.5 + 1.5 * GrassHeight();
    const DrawData d = draws[drawIndex];
    const float3 center = Transform(d.world, local).xyz; // sans échelle : le rayon ne change pas
    const float nearest = max(length(center) - radius, 0.0);
    const float keep = GrassKeep(nearest);
    // Quart q : moitié droite si q & 1, haute si q & 2 (TileKey::Child, BuildTileIndices).
    const uint half = GrassTileQuads / 2;
    const bool inQuarter = quarter == 0 || (uint(origin.x >= half) + 2 * uint(origin.y >= half) == quarter - 1);
    bool kept = inQuarter && keep > 0.0 && InView(center, radius)
             && !Occluded(center + frame[0].cameraDelta.xyz, radius + length(frame[0].cameraDelta.xyz));
    float3 climate = float3(0.0);
    if (kept) {
        climate = GrassClimate(drawIndex, firstVertex, (origin.y + GrassPatchQuads / 2) * GrassTileSide + origin.x + GrassPatchQuads / 2, local);
        // De l'herbe aux coins ou au centre (ses taches font des dizaines de mètres : un carré en a peu ou prou).
        float share = 0.0;
        for (uint corner = 0; corner < 5; ++corner) {
            const uint2 at = corner == 4 ? origin + GrassPatchQuads / 2
                           : origin + uint2(corner & 1u, corner >> 1) * GrassPatchQuads;
            const uint index = at.y * GrassTileSide + at.x;
            share = max(share, GrassShareAt(drawIndex, ReadGrassPosition(drawIndex, firstVertex, index),
                                            normalize(ReadGrassNormal(drawIndex, firstVertex, index)), climate));
        }
        kept = share > 0.02;
    }
    if (kept) {
        const float area = GrassArea(firstVertex, origin, GrassPatchQuads);
        const float total = area <= GrassMaxPatchArea ? ceil(area * GrassDensity()) : 0.0; // faux si NaN
        const uint blades = uint(min(ceil(total * GrassKept(keep)), float(GrassMaxPatchBlades)));
        const float range = GrassRange();
        // 3 segments seulement tout près (l'arc s'y voit), 2 ensuite : chaque segment coûte deux triangles par brin.
        const uint segments = nearest < 8.0 ? 3u : nearest < 0.35 * range ? 2u : 1u;
        const uint groups = (blades + GrassGroupBlades - 1) / GrassGroupBlades;
        if (groups > 0) {
            uint slot;
            InterlockedAdd(grassCount, 1, slot);
            grass.patches[slot] = uint4(thread + segments * 256, groups, blades, asuint(total));
            grass.climates[slot] = climate;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Premier groupe de chaque carré (y gardait leur nombre).
    if (thread == 0) {
        grass.count = grassCount;
        uint first = 0;
        for (uint i = 0; i < grassCount; ++i) {
            const uint groups = grass.patches[i].y;
            grass.patches[i].y = first;
            first += groups;
        }
        grassGroups = first;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(grassGroups, 1, 1, grass);
}

groupshared uint aliveBlades;
groupshared uint corollaCount;
groupshared uint corollaNeeds[GrassGroupBlades]; // sommets en plus de chaque fleur candidate, dans l'ordre d'arrivée
groupshared uint plainBlades;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(GrassGroupBlades, 1, 1)]
void meshGrass(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload GrassPayload tile,
               OutputVertices<GrassVertexOutput, GrassMaxVertices> vertices, OutputIndices<uint3, GrassMaxTriangles> triangles)
{
    // Le carré de ce groupe : le dernier dont le premier groupe ne le dépasse pas (recherche dichotomique : jusqu'à 64).
    uint entry = 0;
    uint last = tile.count - 1;
    while (entry < last) {
        const uint middle = (entry + last + 1) / 2;
        if (tile.patches[middle].y <= group.x) {
            entry = middle;
        } else {
            last = middle - 1;
        }
    }
    const uint4 patch = tile.patches[entry];
    const uint index = (group.x - patch.y) * GrassGroupBlades + thread;
    const uint segments = patch.x >> 8;
    const uint m = patch.x & 255u;
    const float2 origin = float2(uint2(m % GrassPatchSide, m / GrassPatchSide) * GrassPatchQuads);
    // Une graine par carré : avec celle de la tuile seule, tous ses carrés tiraient les mêmes places (même motif répété).
    const uint seed = GroundHash(tile.seed ^ (m * 0x9E3779B9u + 0x7F4A7C15u));
    GrassBlade blade = NoGrassBlade();
    if (index < patch.z) {
        blade = MakeGrassBlade(tile.draw, tile.firstVertex, seed, index, origin, float(GrassPatchQuads),
                               asfloat(patch.w), tile.climates[entry]);
    }
    // Les brins vivants à la suite, sans trou ; les fleurs de près candidates à une corolle comptées à part.
    const uint vertexCount = 2 * segments + 1;
    const uint triangleCount = 2 * segments - 1;
    const bool candidate = blade.alive && blade.petals.a > 0.0 && segments == GrassMaxSegments;
    const uint petals = candidate ? GrassCorollaPetals(blade) : 0u;
    if (thread == 0) {
        aliveBlades = 0;
        corollaCount = 0;
        plainBlades = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint order = 0;
    if (blade.alive) {
        InterlockedAdd(aliveBlades, 1);
    }
    if (candidate) {
        InterlockedAdd(corollaCount, 1, order);
        // Sommets en plus du brin qu'elle remplace (3 par pétale, moins 3) ; un triangle de moins que de sommets.
        corollaNeeds[order] = GrassCorollaVertices(petals) - vertexCount;
    }
    GroupMemoryBarrierWithGroupSync();
    // Place restante une fois tous les brins comptés : les candidates la prennent dans leur ordre d'arrivée tant
    // qu'elle suffit. rank : retenues avant celle-ci ; extra : leur supplément de sommets (de triangles : extra - rank).
    const uint spare = min(GrassMaxVertices - aliveBlades * vertexCount, GrassMaxTriangles - aliveBlades * triangleCount);
    bool corolla = false;
    uint rank = 0;
    uint extra = 0;
    uint accepted = 0;
    for (uint c = 0; c < corollaCount; ++c) {
        const bool fits = accepted + corollaNeeds[c] <= spare;
        if (candidate && c == order) {
            corolla = fits;
            extra = accepted;
        }
        if (fits) {
            accepted += corollaNeeds[c];
            rank += candidate && c < order ? 1u : 0u;
        }
    }
    // Brins simples d'abord, à la suite ; les corolles après eux, dans leur ordre.
    uint plainSlot = 0;
    if (blade.alive && !corolla) {
        InterlockedAdd(plainBlades, 1, plainSlot);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint acceptedCount = aliveBlades - plainBlades;
    SetMeshOutputCounts(aliveBlades * vertexCount + accepted, aliveBlades * triangleCount + accepted - acceptedCount);
    if (corolla) {
        const uint firstVertex = (plainBlades + rank) * vertexCount + extra;
        const uint firstTriangle = (plainBlades + rank) * triangleCount + extra - rank;
        for (uint k = 0; k < GrassCorollaVertices(petals); ++k) {
            vertices[firstVertex + k] = GrassCorollaVertex(blade, k);
        }
        for (uint j = 0; j < GrassCorollaTriangles(petals); ++j) {
            triangles[firstTriangle + j] = GrassCorollaTriangle(j, petals) + firstVertex;
        }
    } else if (blade.alive) {
        for (uint k = 0; k < vertexCount; ++k) {
            vertices[plainSlot * vertexCount + k] = GrassVertex(blade, k, segments);
        }
        for (uint j = 0; j < triangleCount; ++j) {
            triangles[plainSlot * triangleCount + j] = GrassTriangle(j, segments) + plainSlot * vertexCount;
        }
    }
}
