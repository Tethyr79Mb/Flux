// Arbres des planètes (Planetology::VegetationRenderer), à chaque image : un groupe par plaine semée en vue
// (computeCull) range ses arbres dans des listes, chacune dessinée en un appel indirect par primitive de son modèle :
// détaillés de près, simplifiés plus loin, imposteurs au loin, et pour les ombres (modèle simplifié) une liste par
// cascade qu'ils touchent. computeFinalize écrit ensuite le nombre d'arbres de chaque liste dans ses commandes.
// Les arbres eux-mêmes (place, haut, taille, angle) ont été semés une fois par computeTrees (shaders/PlanetTile.slang).

static const uint ListCount = 7;    // VegetationRenderer::ListCount
static const uint MaxCommands = 32; // VegetationRenderer::MaxCommands
static const uint TreesPerCell = 64 * 64;
static const uint GroupSize = 256;

// Doit correspondre à VegetationRenderer::CullFrame.
struct CullFrame
{
    float4 forward;   // de la caméra ; w : tangente du demi-champ horizontal
    float4 right;     // w : tangente du demi-champ vertical
    float4 up;        // w : 1 / sqrt(1 + tangente horizontale²)
    float4 distances; // x : simplifiés au-delà ; y : imposteurs au-delà ; z : plus rien au-delà ; w : 1 / sqrt(1 + tangente verticale²)
    float4 sphere;    // sphère du modèle détaillé : centre (repère du modèle), rayon
    float4 rotation;  // de la planète (quaternion xyzw)
    float4 cascades[16];    // comme FrameData::cascades (shaders/Mesh.slang)
    float4 cascadeLimits[4]; // x : 1 / demi-côté ; y : 1 / profondeur couverte ; z : rayon minimal (m) ; w : fin de la tranche (m)
    uint4 firstInstances[2]; // de chaque liste
    uint4 capacities[2];
    uint4 commandLists[8];   // liste de chaque commande
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
// Deux float4 par arbre retenu : position relative à la caméra, taille ; rotation (quaternion). Lus par vertexTree.
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> instances;
// Commandes de dessin (5 entiers chacune, RHI::DrawIndexedCommand), puis le compteur de chaque liste.
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> commands;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> stats; // arbres de chaque liste, relus par le CPU

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

void Append(uint list, float3 position, float scale, float4 rotation)
{
    uint index;
    InterlockedAdd(commands[MaxCommands * 5 + list], 1u, index);
    if (index < Capacity(list)) {
        const uint at = (FirstInstance(list) + index) * 2;
        instances[at + 0] = float4(position, scale);
        instances[at + 1] = rotation;
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
    const float4 sphere = frame.sphere;
    const float4 distances = frame.distances;
    const float shadowEnd = frame.cascadeLimits[3].w;
    for (uint t = thread; t < count; t += GroupSize) {
        const uint at = (slot * TreesPerCell + t) * 2;
        const float4 placed = trees[at + 0];
        const float4 up = trees[at + 1];
        const float3 position = cell.origin.xyz + Rotate(planet, placed.xyz);
        const float angle = 0.5 * up.w;
        const float4 rotation = Multiply(Align(Rotate(planet, up.xyz)), float4(0.0, sin(angle), 0.0, cos(angle)));
        const float scale = placed.w;
        const float3 center = position + Rotate(rotation, sphere.xyz * scale);
        const float radius = sphere.w * scale;
        const float distance = length(center);
        if (distance - radius > distances.z) {
            continue;
        }
        if (InView(center, radius)) {
            if (distance < distances.y) {
                Append(distance < distances.x ? 0 : 1, position, scale, rotation);
            } else {
                // Au loin, un arbre couvre de moins en moins de pixels et il y en a de plus en plus : passé deux fois
                // la distance des imposteurs, une part d'eux seulement (inverse de la distance), toujours les mêmes
                // (hasard tiré de leur place), un peu plus grands pour couvrir autant. La couleur des forêts sur le
                // sol (fragmentPlanet) comble le reste. Près de la portée, ils rapetissent jusqu'à disparaître.
                const float keep = saturate(2.0 * distances.y / distance);
                if (Random(placed.xyz) < keep) {
                    const float shrink = saturate((distances.z - distance) / (0.15 * distances.z));
                    Append(2, position, scale * min(rsqrt(keep), 1.6) * shrink, rotation);
                }
            }
        }
        // Ombres : le modèle simplifié, dans chaque cascade qu'il touche.
        if (distance - radius < shadowEnd) {
            [unroll]
            for (uint c = 0; c < 4; ++c) {
                if (InCascade(center, radius, c)) {
                    Append(3 + c, position, scale, rotation);
                }
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeFinalize(uint thread : SV_GroupIndex)
{
    if (thread < frame.counts.x) {
        const uint list = frame.commandLists[thread / 4][thread % 4];
        commands[thread * 5 + 1] = min(commands[MaxCommands * 5 + list], Capacity(list));
    }
    if (thread < ListCount) {
        stats[thread] = commands[MaxCommands * 5 + thread];
    }
}
