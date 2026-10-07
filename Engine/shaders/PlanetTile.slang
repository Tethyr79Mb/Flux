// Tuiles des planètes calculées par le GPU : un groupe de threads par tuile écrit ses sommets (ModelVertex) et leur
// glissement vers la forme de la parente (MorphVertex), les mêmes que Planet::BuildTile sur le CPU, qui sert de
// référence (Tests/src/PlanetGpuTests.cpp vérifie qu'ils s'accordent). Le travail en double (directions, points de
// départ du bruit) est fait par Planet::PrepareTileGpu : ici tout est en float, relatif au centre de la tuile.
// Le relief (Height) doit rester le même que Terrain::GetHeight dans Engine/src/Graphics/Planet.cpp.

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint Wide = TileSide + 2;          // grille avec une rangée de plus autour (normales du bord)
static const uint Coarse = TileQuads / 2 + 3;   // sommets de la parente, de -2 à TileQuads + 2, de deux en deux
static const uint Columns = TileSide + 4;
static const uint OctaveCount = 40;
static const uint GroupSize = 256;

// Doit correspondre à Planet::TileGpuInputs.
struct TileInputs
{
    float4 center; // xyz : direction du centre ; w : |v_c|
    float4 axisN;  // xyz : normale de la face ; w : rayon
    float4 axisS;  // xyz : axe s ; w : relief
    float4 axisT;  // xyz : axe t ; w : profondeur des jupes
    uint4 options; // x : octaves ; y : octaves de la parente ; z : niveau
    float4 columns[Columns];
    int4 cells[OctaveCount];
    float4 fractions[OctaveCount];
    uint4 permutation[32];
};

struct Constants
{
    uint job; // indice dans `inputs` et `stats`
};

[[vk::binding(0, 0)]] StructuredBuffer<TileInputs> inputs;
// Par tuile, distances au centre de la planète de ses sommets (forme de la parente comprise, sans les jupes) : x la
// plus petite (océan), z la plus grande ; y rayon de la sphère englobante (autour du centre de la tuile).
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> stats;
[[vk::binding(0, 1)]] RWStructuredBuffer<float> vertices; // ModelVertex : position, normale, uv (8 floats)
[[vk::binding(1, 1)]] RWStructuredBuffer<float> morph;    // MorphVertex : décalage, normale (6 floats)
[[vk::push_constant]] ConstantBuffer<Constants> constants;
// Lue directement dans le buffer (une copie locale de 2,5 Ko déborderait des registres).
#define tile inputs[constants.job]

groupshared uint permutation[512];
groupshared float wideHeights[Wide * Wide];
groupshared float coarseHeights[Coarse * Coarse];
groupshared float lowestShared[GroupSize];
groupshared float radiusShared[GroupSize];
groupshared float highestShared[GroupSize];

// Couches du relief, comme Layers dans Planet.cpp : premières et dernières octaves, gain, crêtes.
struct Layer
{
    uint first;
    uint last;
    float gain;
    bool ridged;
};
static const Layer Layers[6] = {
    { 0, 5, 0.5, false },   // continents
    { 1, 4, 0.5, false },   // zones de montagnes
    { 2, 5, 0.5, false },   // zones de collines
    { 2, 10, 0.5, true },   // montagnes (crêtes)
    { 4, 9, 0.5, false },   // collines
    { 6, 22, 0.58, false }, // détail
};

float Fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float Gradient(uint hash, float x, float y, float z)
{
    const uint h = hash & 15u;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1u) != 0 ? -u : u) + ((h & 2u) != 0 ? -v : v);
}

// Bruit de Perlin au point cell + q (cell : maille du réseau modulo 256 ; q : position, petite, depuis cette maille).
float Noise(int3 cell, float3 q)
{
    const float3 floored = floor(q);
    const int3 c = (cell + int3(floored)) & 255;
    const float3 f = q - floored;
    const float u = Fade(f.x);
    const float v = Fade(f.y);
    const float w = Fade(f.z);
    const uint A = permutation[c.x] + uint(c.y);
    const uint AA = permutation[A] + uint(c.z);
    const uint AB = permutation[A + 1] + uint(c.z);
    const uint B = permutation[c.x + 1] + uint(c.y);
    const uint BA = permutation[B] + uint(c.z);
    const uint BB = permutation[B + 1] + uint(c.z);
    const float x = f.x, y = f.y, z = f.z;
    return lerp(lerp(lerp(Gradient(permutation[AA], x, y, z), Gradient(permutation[BA], x - 1, y, z), u),
                     lerp(Gradient(permutation[AB], x, y - 1, z), Gradient(permutation[BB], x - 1, y - 1, z), u), v),
                lerp(lerp(Gradient(permutation[AA + 1], x, y, z - 1), Gradient(permutation[BA + 1], x - 1, y, z - 1), u),
                     lerp(Gradient(permutation[AB + 1], x, y - 1, z - 1), Gradient(permutation[BB + 1], x - 1, y - 1, z - 1), u), v),
                w);
}

uint FirstOctaveIndex(uint layer)
{
    uint index = 0;
    for (uint l = 0; l < layer; ++l) { index += Layers[l].last - Layers[l].first; }
    return index;
}

// Terrain::Fbm : `delta` = direction - direction du centre de la tuile.
float Fbm(float3 delta, uint layer, uint octaves)
{
    const Layer L = Layers[layer];
    const float total = (1.0 - pow(L.gain, float(L.last - L.first))) / (1.0 - L.gain);
    const uint last = min(L.last, octaves);
    const uint base = FirstOctaveIndex(layer);
    float sum = 0.0;
    float amplitude = 1.0;
    for (uint k = L.first; k < last; ++k) {
        const uint o = base + k - L.first;
        const float4 fraction = tile.fractions[o];
        const float n = Noise(tile.cells[o].xyz, fraction.xyz + delta * fraction.w);
        sum += amplitude * (L.ridged ? (1.0 - abs(n)) * (1.0 - abs(n)) : n);
        amplitude *= L.gain;
    }
    return sum / total;
}

// Terrain::GetHeight.
float Height(float3 delta, uint octaves)
{
    const float relief = tile.axisS.w;
    if (relief <= 0.0) { return 0.0; }
    const float continents = Fbm(delta, 0, octaves);
    const float land = smoothstep(-0.26, 0.06, continents);
    const float mountainZone = smoothstep(0.08, 0.36, Fbm(delta, 1, octaves)) * land;
    const float hillZone = smoothstep(0.05, 0.4, Fbm(delta, 2, octaves));
    const float mountains = mountainZone > 0.0 ? Fbm(delta, 3, octaves) : 0.0;
    const float hills = hillZone > 0.0 ? Fbm(delta, 4, octaves) : 0.0;
    const float roughness = 0.012 + 0.03 * hillZone + 0.065 * mountainZone;
    const float detail = Fbm(delta, 5, octaves);
    const float height = -0.4 + 0.45 * land + 0.15 * continents + 0.85 * mountainZone * mountains
                       + 0.15 * hillZone * hills + roughness * detail;
    return relief * clamp(height, -1.0, 1.0);
}

// Écart δ entre la direction du point (colonne ci, ligne cj de inputs.columns) et celle du centre, sans soustraire
// deux vecteurs unitaires voisins : v = v_c + Δv, |v|² = |v_c|² + Δa(a + a_c) + Δb(b + b_c).
float3 Delta(uint ci, uint cj)
{
    const float4 column = tile.columns[ci];
    const float4 row = tile.columns[cj];
    const float3 dv = tile.axisS.xyz * column.x + tile.axisT.xyz * row.z;
    const float growth = column.x * column.y + row.z * row.w;
    const float lc = tile.center.w;
    const float l = sqrt(lc * lc + growth);
    const float3 vc = tile.center.xyz * lc;
    return dv / l - vc * (growth / (l * lc * (l + lc)));
}

// Position relative au centre de la tuile : d.(R + h) - d_c.R = d_c.h + δ.(R + h).
float3 Local(float3 delta, float height)
{
    return tile.center.xyz * height + delta * (tile.axisN.w + height);
}

// Point (i, j) de la grille large (0 à Wide - 1, décalage -1) ; colonne i + 1.
float3 WideLocal(uint i, uint j)
{
    return Local(Delta(i + 1, j + 1), wideHeights[j * Wide + i]);
}

// Sommet (ci, cj) de la parente (0 à Coarse - 1, décalage 2.ci - 2) ; colonne 2.ci.
float3 CoarseLocal(uint ci, uint cj)
{
    return Local(Delta(2 * ci, 2 * cj), coarseHeights[cj * Coarse + ci]);
}

struct TileVertex
{
    float3 position;
    float3 normal;
    float3 morphOffset;
    float3 morphNormal;
};

// Sommet (i, j) de la tuile (0 à TileQuads) et son glissement, comme BuildTile.
TileVertex BuildVertex(uint i, uint j)
{
    TileVertex vertex;
    const uint wi = i + 1;
    const uint wj = j + 1;
    vertex.position = WideLocal(wi, wj);
    const float3 along = WideLocal(wi + 1, wj) - WideLocal(wi - 1, wj);
    const float3 across = WideLocal(wi, wj + 1) - WideLocal(wi, wj - 1);
    vertex.normal = normalize(cross(along, across));
    if (tile.options.z == 0) {
        vertex.morphOffset = float3(0.0, 0.0, 0.0);
        vertex.morphNormal = vertex.normal;
        return vertex;
    }
    // Les deux sommets de la parente dont celui-ci est le milieu (le même deux fois s'il est à elle).
    const uint i0 = i & ~1u;
    const uint j0 = j & ~1u;
    const uint i1 = i0 + (i & 1u) * 2;
    const uint j1 = j0 + (j & 1u) * 2;
    const uint2 c0 = uint2(i0 / 2 + 1, j0 / 2 + 1);
    const uint2 c1 = uint2(i1 / 2 + 1, j1 / 2 + 1);
    const float3 target = (CoarseLocal(c0.x, c0.y) + CoarseLocal(c1.x, c1.y)) * 0.5;
    const float3 n0 = normalize(cross(CoarseLocal(c0.x + 1, c0.y) - CoarseLocal(c0.x - 1, c0.y),
                                      CoarseLocal(c0.x, c0.y + 1) - CoarseLocal(c0.x, c0.y - 1)));
    const float3 n1 = normalize(cross(CoarseLocal(c1.x + 1, c1.y) - CoarseLocal(c1.x - 1, c1.y),
                                      CoarseLocal(c1.x, c1.y + 1) - CoarseLocal(c1.x, c1.y - 1)));
    vertex.morphOffset = target - vertex.position;
    vertex.morphNormal = normalize(n0 + n1);
    return vertex;
}

void Write(uint index, float3 position, float3 normal, float3 morphOffset, float3 morphNormal)
{
    const uint v = index * 8;
    vertices[v + 0] = position.x;
    vertices[v + 1] = position.y;
    vertices[v + 2] = position.z;
    vertices[v + 3] = normal.x;
    vertices[v + 4] = normal.y;
    vertices[v + 5] = normal.z;
    vertices[v + 6] = 0.0;
    vertices[v + 7] = 0.0;
    const uint m = index * 6;
    morph[m + 0] = morphOffset.x;
    morph[m + 1] = morphOffset.y;
    morph[m + 2] = morphOffset.z;
    morph[m + 3] = morphNormal.x;
    morph[m + 4] = morphNormal.y;
    morph[m + 5] = morphNormal.z;
}

// Distance au centre de la planète d'un point relatif au centre de la tuile.
float FromPlanetCenter(float3 local)
{
    return length(tile.center.xyz * tile.axisN.w + local);
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void computeMain(uint thread : SV_GroupIndex)
{
    for (uint p = thread; p < 128; p += GroupSize) {
        const uint word = tile.permutation[p / 4][p % 4];
        permutation[4 * p + 0] = word & 255u;
        permutation[4 * p + 1] = (word >> 8) & 255u;
        permutation[4 * p + 2] = (word >> 16) & 255u;
        permutation[4 * p + 3] = word >> 24;
    }
    GroupMemoryBarrierWithGroupSync();

    // Hauteurs : la grille large avec les octaves de la tuile, celle de la parente avec les siennes.
    for (uint p = thread; p < Wide * Wide + Coarse * Coarse; p += GroupSize) {
        if (p < Wide * Wide) {
            const uint i = p % Wide;
            const uint j = p / Wide;
            wideHeights[p] = Height(Delta(i + 1, j + 1), tile.options.x);
        } else {
            const uint c = p - Wide * Wide;
            const uint ci = c % Coarse;
            const uint cj = c / Coarse;
            coarseHeights[c] = Height(Delta(2 * ci, 2 * cj), tile.options.y);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    float lowest = 1.0e30;
    float highest = 0.0;
    float radius = 0.0;
    for (uint v = thread; v < TileSide * TileSide; v += GroupSize) {
        const TileVertex vertex = BuildVertex(v % TileSide, v / TileSide);
        Write(v, vertex.position, vertex.normal, vertex.morphOffset, vertex.morphNormal);
        const float3 target = vertex.position + vertex.morphOffset;
        const float fromCenter = FromPlanetCenter(vertex.position);
        const float targetFromCenter = FromPlanetCenter(target);
        lowest = min(lowest, min(fromCenter, targetFromCenter));
        highest = max(highest, max(fromCenter, targetFromCenter));
        radius = max(radius, max(length(vertex.position), length(target)));
    }
    // Jupes : le bord parcouru dans le sens trigonométrique (bas, droite, haut, gauche), descendu vers le centre de
    // la planète ; mêmes normale et glissement que leur bord.
    for (uint k = thread; k < 4 * TileSide; k += GroupSize) {
        const uint edge = k / TileSide;
        const uint m = k % TileSide;
        const uint2 at = edge == 0 ? uint2(m, 0) : edge == 1 ? uint2(TileQuads, m)
                       : edge == 2 ? uint2(TileQuads - m, TileQuads) : uint2(0, TileQuads - m);
        const TileVertex vertex = BuildVertex(at.x, at.y);
        const float3 direction = tile.center.xyz + Delta(at.x + 2, at.y + 2);
        const float3 position = vertex.position - direction * tile.axisT.w;
        Write(TileSide * TileSide + k, position, vertex.normal, vertex.morphOffset, vertex.morphNormal);
        radius = max(radius, length(position + vertex.morphOffset));
    }

    lowestShared[thread] = lowest;
    radiusShared[thread] = radius;
    highestShared[thread] = highest;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = GroupSize / 2; stride > 0; stride /= 2) {
        if (thread < stride) {
            lowestShared[thread] = min(lowestShared[thread], lowestShared[thread + stride]);
            radiusShared[thread] = max(radiusShared[thread], radiusShared[thread + stride]);
            highestShared[thread] = max(highestShared[thread], highestShared[thread + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread == 0) {
        stats[constants.job] = float4(lowestShared[0], radiusShared[0], highestShared[0], 0.0);
    }
}
