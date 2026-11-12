// Herbe des planètes en mesh shaders (voir Grass.slang), quand le GPU les a : une tâche (taskGrass) par tuile proche
// teste ses 16 paquets de 8 x 8 mailles : hors du champ, caché derrière ce que voyait l'image précédente, sans herbe
// (aux coins et au centre), ou trop loin pour garder un brin, il est écarté. Chaque paquet retenu envoie ses brins à
// meshGrass par groupes de 32 (un thread par brin), en 3 segments de près, 2 puis 1 au loin.
// Compilé à part de Grass.slang (qu'il inclut) : un module qui déclare les mesh shaders ne se charge pas sur un GPU qui
// ne les a pas.

#include "Grass.slang"

static const uint GrassGroupBlades = 32;
static const uint GrassMaxSegments = 3;
static const uint GrassMaxVertices = GrassGroupBlades * (2 * GrassMaxSegments + 1);
static const uint GrassMaxTriangles = GrassGroupBlades * (2 * GrassMaxSegments - 1);
// Bornes d'un paquet : sur une tuile fine (1,5 m entre sommets) il couvre ~150 m², soit ~29 000 brins à la densité par
// défaut. Une aire plus grande ou non finie (sommets pas encore calculés) l'écarte : sans elles, des milliards de brins
// partaient au pilote (délai dépassé, GPU perdu).
static const float GrassMaxMeshletArea = 1000.0;
static const uint GrassMaxMeshletBlades = 65536; // 2 048 groupes : 16 paquets restent sous les 65 535 d'une tâche

// Commandes des tuiles de l'image (5 uint chacune : indexCount, instanceCount, firstIndex, vertexOffset,
// firstInstance) ; params.draw : première du lot.
[[vk::binding(11, 1)]] StructuredBuffer<uint> tileCommands;

struct GrassPayload
{
    uint draw;        // DrawData de la tuile
    uint firstVertex; // dans la page
    uint seed;        // GrassSeed
    uint count;       // paquets retenus
    // Par paquet retenu : x, paquet + segments x 256 ; y, son premier groupe ; z, brins gardés ; w, brins à pleine
    // densité (bits d'un float).
    uint4 meshlets[GrassMeshletCount];
    float2 climates[GrassMeshletCount]; // PlanetClimate au centre du paquet
};

groupshared GrassPayload grass;
groupshared uint grassGroups;
groupshared uint grassCount;

[shader("amplification")]
[numthreads(GrassMeshletCount, 1, 1)]
void taskGrass(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const uint command = (params.draw + group.x) * 5;
    const uint tileInPage = tileCommands[command + 3] >> GrassVertexShift;
    const uint drawIndex = tileCommands[command + 4] >> GrassInstanceShift;
    const uint firstVertex = tileInPage * GrassTileVertices;
    if (thread == 0) {
        grass.draw = drawIndex;
        grass.firstVertex = firstVertex;
        grass.seed = GrassSeed(drawIndex);
        grassCount = 0;
        grassGroups = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Paquet `thread` : sa boîte (relief compris), grandie de la hauteur des brins les plus hauts et penchés.
    const GrassMeshlet meshlet = pageMeshlets[tileInPage * GrassMeshletCount + thread];
    const float3 local = (meshlet.low.xyz + meshlet.high.xyz) * 0.5;
    const float radius = length(meshlet.high.xyz - meshlet.low.xyz) * 0.5 + 1.5 * GrassHeight();
    const DrawData d = draws[drawIndex];
    const float3 center = Transform(d.world, local).xyz; // sans échelle : le rayon ne change pas
    const float nearest = max(length(center) - radius, 0.0);
    const float keep = GrassKeep(nearest);
    bool kept = keep > 0.0 && InView(center, radius)
             && !Occluded(center + frame[0].cameraDelta.xyz, radius + length(frame[0].cameraDelta.xyz));
    const uint2 origin = uint2(thread % GrassMeshletSide, thread / GrassMeshletSide) * GrassMeshletQuads;
    float2 climate = float2(0.0);
    if (kept) {
        climate = PlanetClimate(frame[0].planets[asuint(d.rim.w)], d.rim.xyz + local);
        // De l'herbe aux coins ou au centre (ses taches font des dizaines de mètres : un paquet en a peu ou prou).
        float share = 0.0;
        for (uint corner = 0; corner < 5; ++corner) {
            const uint2 at = corner == 4 ? origin + GrassMeshletQuads / 2
                           : origin + uint2(corner & 1u, corner >> 1) * GrassMeshletQuads;
            const uint index = at.y * GrassTileSide + at.x;
            share = max(share, GrassShareAt(drawIndex, ReadGrassPosition(firstVertex, index),
                                            normalize(ReadGrassNormal(firstVertex, index)), climate));
        }
        kept = share > 0.02;
    }
    if (kept) {
        const float area = GrassArea(firstVertex, origin, GrassMeshletQuads);
        const float total = area <= GrassMaxMeshletArea ? ceil(area * GrassDensity()) : 0.0; // faux si NaN
        const uint blades = uint(min(ceil(total * GrassKept(keep)), float(GrassMaxMeshletBlades)));
        const float range = GrassRange();
        const uint segments = nearest < 0.12 * range ? 3u : nearest < 0.35 * range ? 2u : 1u;
        const uint groups = (blades + GrassGroupBlades - 1) / GrassGroupBlades;
        if (groups > 0) {
            uint slot;
            InterlockedAdd(grassCount, 1, slot);
            grass.meshlets[slot] = uint4(thread + segments * 256, groups, blades, asuint(total));
            grass.climates[slot] = climate;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Premier groupe de chaque paquet (y gardait leur nombre).
    if (thread == 0) {
        grass.count = grassCount;
        uint first = 0;
        for (uint i = 0; i < grassCount; ++i) {
            const uint groups = grass.meshlets[i].y;
            grass.meshlets[i].y = first;
            first += groups;
        }
        grassGroups = first;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(grassGroups, 1, 1, grass);
}

groupshared uint aliveBlades;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(GrassGroupBlades, 1, 1)]
void meshGrass(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload GrassPayload tile,
               OutputVertices<GrassVertexOutput, GrassMaxVertices> vertices, OutputIndices<uint3, GrassMaxTriangles> triangles)
{
    // Le paquet de ce groupe : le dernier dont le premier groupe ne le dépasse pas.
    uint entry = 0;
    while (entry + 1 < tile.count && tile.meshlets[entry + 1].y <= group.x) {
        ++entry;
    }
    const uint4 meshlet = tile.meshlets[entry];
    const uint index = (group.x - meshlet.y) * GrassGroupBlades + thread;
    const uint segments = meshlet.x >> 8;
    const uint m = meshlet.x & 255u;
    const float2 origin = float2(uint2(m % GrassMeshletSide, m / GrassMeshletSide) * GrassMeshletQuads);
    GrassBlade blade = NoGrassBlade();
    if (index < meshlet.z) {
        blade = MakeGrassBlade(tile.draw, tile.firstVertex, tile.seed, index, origin, float(GrassMeshletQuads),
                               asfloat(meshlet.w), tile.climates[entry]);
    }
    // Les brins vivants à la suite, sans trou.
    if (thread == 0) {
        aliveBlades = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint slot = 0;
    if (blade.alive) {
        InterlockedAdd(aliveBlades, 1, slot);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint vertexCount = 2 * segments + 1;
    const uint triangleCount = 2 * segments - 1;
    SetMeshOutputCounts(aliveBlades * vertexCount, aliveBlades * triangleCount);
    if (blade.alive) {
        for (uint k = 0; k < vertexCount; ++k) {
            vertices[slot * vertexCount + k] = GrassVertex(blade, k, segments);
        }
        for (uint j = 0; j < triangleCount; ++j) {
            triangles[slot * triangleCount + j] = GrassTriangle(j, segments) + slot * vertexCount;
        }
    }
}
