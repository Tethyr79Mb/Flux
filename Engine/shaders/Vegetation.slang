// Arbres des planètes (Planetology::VegetationRenderer), à chaque image : un groupe par plaine semée en vue
// (computeCull) range ses arbres dans des listes : détaillés de près, simplifiés plus loin, imposteurs au loin, et pour
// les ombres (modèle simplifié) une liste par cascade qu'ils touchent, en comptant ceux de chaque espèce.
// computeFinalize donne à chaque espèce sa part de sa liste et l'écrit dans ses commandes (une par primitive de son
// modèle), puis computeScatter y range les arbres, espèce par espèce, pour vertexTree.
// Les arbres eux-mêmes (place, haut, taille, espèce, angle) ont été semés une fois par computeTrees
// (shaders/PlanetTile.slang).

static const uint ListCount = 7;     // VegetationRenderer::ListCount
static const uint MaxSpecies = 8;    // VegetationRenderer::MaxSpecies
static const uint MaxCommands = 128; // VegetationRenderer::MaxCommands
static const uint SpeciesLists = ListCount * MaxSpecies;
static const uint TreesPerCell = 64 * 64;
static const uint GroupSize = 256;

// Doit correspondre à VegetationRenderer::CullFrame.
struct CullFrame
{
    float4 forward;   // de la caméra ; w : tangente du demi-champ horizontal
    float4 right;     // w : tangente du demi-champ vertical
    float4 up;        // w : 1 / sqrt(1 + tangente horizontale²)
    float4 distances; // x : simplifiés au-delà ; y : imposteurs au-delà ; z : plus rien au-delà ; w : 1 / sqrt(1 + tangente verticale²)
    float4 spheres[MaxSpecies]; // sphère du modèle détaillé de chaque espèce : centre (repère du modèle), rayon
    float4 tints[MaxSpecies / 4]; // écart de teinte de chaque espèce (TreeSpecies::tint)
    float4 rotation;  // de la planète (quaternion xyzw)
    float4 cascades[16];    // comme FrameData::cascades (shaders/Mesh.slang)
    float4 cascadeLimits[4]; // x : 1 / demi-côté ; y : 1 / profondeur couverte ; z : rayon minimal (m) ; w : fin de la tranche (m)
    float4 receiverSides[4]; // bords du champ (ShadowReceivers::GetSides)
    float4 toSun;            // w : distance des obstacles vers le soleil (ShadowCasterDistance)
    uint4 firstInstances[2]; // de chaque liste
    uint4 capacities[2];
    uint4 commandLists[MaxCommands / 4]; // liste x MaxSpecies + espèce de chaque commande
    uint4 counts;            // x : commandes ; y : plaines
};

// Doit correspondre à VegetationRenderer::CullCell : une plaine semée en vue.
struct CullCell
{
    float4 origin; // point de référence de sa tuile, relatif à la caméra ; w : sa case (bits d'un entier)
};

[[vk::binding(0, 0)]] StructuredBuffer<CullFrame> cullFrame;
[[vk::binding(1, 0)]] StructuredBuffer<CullCell> cells;
[[vk::binding(2, 0)]] StructuredBuffer<float4> trees;  // computeTrees : 2 par arbre
[[vk::binding(3, 0)]] StructuredBuffer<uint> treeCounts;
// Deux float4 par arbre retenu, rangés par espèce : position relative à la caméra, taille ; rotation (quaternion de
// w positif : xyz seulement), teinte (de -0,5 à 0,5). Lus par vertexTree.
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> instances;
// Commandes de dessin (5 entiers chacune, RHI::DrawIndexedCommand), puis les compteurs : arbres de chaque liste, de
// chaque espèce de chaque liste, place suivante de chaque espèce (computeScatter).
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> commands;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> stats; // arbres de chaque liste, relus par le CPU
// Les mêmes que instances, dans l'ordre du tri ; w de la rotation : espèce x 4 + teinte + 1.
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> staged;

static const uint ListCounters = MaxCommands * 5;
static const uint SpeciesCounters = ListCounters + ListCount;
static const uint Cursors = SpeciesCounters + SpeciesLists;

#define frame cullFrame[0]

float3 Rotate(float4 q, float3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float4 Multiply(float4 a, float4 b)
{
    return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

// Rotation qui amène +y (le haut du modèle) sur `up`.
float4 Align(float3 up)
{
    if (up.y < -0.9999) {
        return float4(1.0, 0.0, 0.0, 0.0);
    }
    return normalize(float4(up.z, 0.0, -up.x, 1.0 + up.y));
}

// La sphère touche le champ de la caméra (comme ViewCone dans MeshRenderer.cpp).
bool InView(float3 center, float radius)
{
    const float depth = dot(center, frame.forward.xyz);
    if (depth < -radius) {
        return false;
    }
    return (abs(dot(center, frame.right.xyz)) - depth * frame.forward.w) * frame.up.w <= radius
        && (abs(dot(center, frame.up.xyz)) - depth * frame.right.w) * frame.distances.w <= radius;
}

// Comme MeshRenderer::CastsShadow.
bool InCascade(float3 p, float radius, uint c)
{
    const float4 limits = frame.cascadeLimits[c];
    if (radius < limits.z) {
        return false;
    }
    const float4 m0 = frame.cascades[c * 4 + 0];
    const float4 m1 = frame.cascades[c * 4 + 1];
    const float4 m2 = frame.cascades[c * 4 + 2];
    const float4 m3 = frame.cascades[c * 4 + 3];
    const float3 x = m0.xyz * p.x + m1.xyz * p.y + m2.xyz * p.z + m3.xyz;
    const float side = radius * limits.x;
    const float depth = radius * limits.y;
    return abs(x.x) <= 1.0 + side && abs(x.y) <= 1.0 + side && x.z >= -depth && x.z <= 1.0 + depth;
}

// L'ombre de la sphère, prolongée de `length` mètres à l'opposé du soleil, peut tomber dans la tranche c du champ
// (comme ShadowReceivers::Reaches).
bool ReachesView(float3 center, float radius, float length, uint c)
{
    const float3 end = center - frame.toSun.xyz * length;
    [unroll]
    for (uint s = 0; s < 4; ++s) {
        const float3 side = frame.receiverSides[s].xyz;
        if (dot(center, side) > radius && dot(end, side) > radius) {
            return false;
        }
    }
    const float start = (c == 0 ? 0.0 : frame.cascadeLimits[c - 1].w) - radius - 1.0;
    const float stop = frame.cascadeLimits[c].w + radius + 1.0;
    const float depthA = dot(center, frame.forward.xyz);
    const float depthB = dot(end, frame.forward.xyz);
    return max(depthA, depthB) >= start && min(depthA, depthB) <= stop;
}

// Nombre de 0 à 1 tiré de la place d'un arbre.
float Random(float3 p)
{
    uint x = asuint(p.x) * 0x9e3779b1u ^ asuint(p.y) * 0x85ebca6bu ^ asuint(p.z) * 0xc2b2ae35u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.0 / 16777216.0);
}

uint FirstInstance(uint list) { return frame.firstInstances[list / 4][list % 4]; }
uint Capacity(uint list) { return frame.capacities[list / 4][list % 4]; }

void Append(uint list, uint species, float tint, float3 position, float scale, float4 rotation)
{
    uint index;
    InterlockedAdd(commands[ListCounters + list], 1u, index);
    if (index < Capacity(list)) {
        const uint at = (FirstInstance(list) + index) * 2;
        staged[at + 0] = float4(position, scale);
        staged[at + 1] = float4(rotation.xyz * (rotation.w < 0.0 ? -1.0 : 1.0), float(species) * 4.0 + tint + 1.0);
        InterlockedAdd(commands[SpeciesCounters + list * MaxSpecies + species], 1u);
    }
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void computeCull(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const CullCell cell = cells[group.x];
    const uint slot = asuint(cell.origin.w);
    const uint count = treeCounts[slot];
    const float4 planet = frame.rotation;
    const float4 distances = frame.distances;
    const float shadowEnd = frame.cascadeLimits[3].w;
    for (uint t = thread; t < count; t += GroupSize) {
        const uint at = (slot * TreesPerCell + t) * 2;
        const float4 placed = trees[at + 0];
        const float4 up = trees[at + 1];
        const float3 position = cell.origin.xyz + Rotate(planet, placed.xyz);
        const uint species = min(uint(up.w / 8.0), MaxSpecies - 1);
        const float angle = 0.5 * (up.w - float(species) * 8.0);
        const float4 sphere = frame.spheres[species];
        const float tint = (Random(placed.xyz + 0.37) - 0.5) * frame.tints[species / 4][species % 4] * 2.0;
        const float3 normal = Rotate(planet, up.xyz); // haut de l'arbre
        const float4 rotation = Multiply(Align(normal), float4(0.0, sin(angle), 0.0, cos(angle)));
        const float scale = placed.w;
        const float3 center = position + Rotate(rotation, sphere.xyz * scale);
        const float radius = sphere.w * scale;
        const float distance = length(center);
        if (distance - radius > distances.z) {
            continue;
        }
        if (InView(center, radius)) {
            if (distance < distances.y) {
                Append(distance < distances.x ? 0 : 1, species, tint, position, scale, rotation);
            } else {
                // Au loin, un arbre couvre de moins en moins de pixels et il y en a de plus en plus : passé deux fois
                // la distance des imposteurs, une part d'eux seulement (inverse de la distance), toujours les mêmes
                // (hasard tiré de leur place), un peu plus grands pour couvrir autant. La couleur des forêts sur le
                // sol (fragmentPlanet) comble le reste. Près de la portée, ils rapetissent jusqu'à disparaître.
                const float keep = saturate(2.0 * distances.y / distance);
                if (Random(placed.xyz) < keep) {
                    const float shrink = saturate((distances.z - distance) / (0.15 * distances.z));
                    Append(2, species, tint, position, scale * min(rsqrt(keep), 1.6) * shrink, rotation);
                }
            }
        }
        // Ombres : le modèle simplifié, dans chaque cascade qu'il touche et dont la tranche peut recevoir son ombre.
        // Elle s'arrête au sol : pas plus loin que la cime divisée par la hauteur du soleil, doublée pour un sol qui
        // descend (au-delà, sur une pente plus raide, elle serait coupée).
        if (distance - radius < shadowEnd) {
            const float sunHeight = dot(frame.toSun.xyz, normal);
            const float top = dot(center - position, normal) + radius;
            const float length = sunHeight > 0.0 ? min(frame.toSun.w, 2.0 * top / sunHeight) : frame.toSun.w;
            [unroll]
            for (uint c = 0; c < 4; ++c) {
                if (InCascade(center, radius, c) && ReachesView(center, radius, length, c)) {
                    Append(3 + c, species, tint, position, scale, rotation);
                }
            }
        }
    }
}

groupshared uint speciesOffsets[SpeciesLists];

[shader("compute")]
[numthreads(MaxCommands, 1, 1)]
void computeFinalize(uint thread : SV_GroupIndex)
{
    // Part de chaque espèce dans sa liste : après les espèces d'avant.
    if (thread < SpeciesLists) {
        const uint list = thread / MaxSpecies;
        uint offset = 0;
        for (uint s = 0; s < thread % MaxSpecies; ++s) {
            offset += commands[SpeciesCounters + list * MaxSpecies + s];
        }
        speciesOffsets[thread] = offset;
        commands[Cursors + thread] = offset;
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread < frame.counts.x) {
        const uint speciesList = frame.commandLists[thread / 4][thread % 4];
        commands[thread * 5 + 1] = commands[SpeciesCounters + speciesList];
        commands[thread * 5 + 4] = FirstInstance(speciesList / MaxSpecies) + speciesOffsets[speciesList];
    }
    if (thread < ListCount) {
        stats[thread] = commands[ListCounters + thread];
    }
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void computeScatter(uint3 id : SV_DispatchThreadID)
{
    const uint index = id.x;
    uint list = ListCount;
    for (uint l = 0; l < ListCount; ++l) {
        if (index >= FirstInstance(l) && index < FirstInstance(l) + Capacity(l)) {
            list = l;
        }
    }
    if (list == ListCount || index - FirstInstance(list) >= min(commands[ListCounters + list], Capacity(list))) {
        return;
    }
    const float4 placed = staged[index * 2 + 0];
    const float4 rotation = staged[index * 2 + 1];
    const uint species = min(uint(rotation.w * 0.25), MaxSpecies - 1);
    uint at;
    InterlockedAdd(commands[Cursors + list * MaxSpecies + species], 1u, at);
    const uint target = (FirstInstance(list) + at) * 2;
    instances[target + 0] = placed;
    instances[target + 1] = float4(rotation.xyz, rotation.w - float(species) * 4.0 - 1.0);
}
