// Tuiles des planètes calculées par le GPU : un groupe de threads par tuile écrit ses sommets (ModelVertex) et leur
// glissement vers la forme de la parente (MorphVertex), les mêmes que Planetology::BuildTile sur le CPU, qui sert de
// référence (Tests/src/PlanetGpuTests.cpp vérifie qu'ils s'accordent). Le travail en double (directions, points de
// départ du bruit) est fait par Planetology::PrepareTileGpu : ici tout est en float, relatif au centre de la tuile.
// Le relief (Height) doit rester le même que Terrain::GetHeight dans Engine/src/Planetology/Planet.cpp.
// Arbres (computeTrees) : un groupe par plaine de VegetationRenderer sème ses arbres sur ce même relief, au détail le
// plus fin (celui du sol vu de près).

#include "PlanetClimate.slang"

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint Wide = TileSide + 2;          // grille avec une rangée de plus autour (normales du bord)
static const uint Coarse = TileQuads / 2 + 3;   // sommets de la parente, de -2 à TileQuads + 2, de deux en deux
static const uint Columns = TileSide + 4;
static const uint OctaveCount = 45;
static const uint GroupSize = 256;

// Doit correspondre à Planetology::TileGpuInputs.
struct TileInputs
{
    float4 center; // xyz : direction du centre ; w : |v_c|
    float4 axisN;  // xyz : normale de la face ; w : rayon
    float4 axisS;  // xyz : axe s ; w : relief
    float4 axisT;  // xyz : axe t ; w : profondeur des jupes
    uint4 options; // x : octaves ; y : octaves de la parente ; z : niveau ; w : décalage des couches à la largeur des montagnes
    float4 columns[Columns];
    int4 cells[OctaveCount];
    float4 fractions[OctaveCount];
    uint4 permutation[32];
};

struct Constants
{
    uint job;         // indice dans `inputs` et `stats`
    uint firstVertex; // premier sommet de la tuile dans les buffers de sa page (PlanetTileCompute)
};

[[vk::binding(0, 0)]] StructuredBuffer<TileInputs> inputs;
// Par tuile, distances au centre de la planète de ses sommets (forme de la parente comprise, sans les jupes) : x la
// plus petite (océan), z la plus grande ; y rayon de la sphère englobante (autour du centre de la tuile).
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> stats;
// Page de tuiles : la tuile écrit à partir de constants.firstVertex.
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

// Réglages du relief, comme dans Planet.cpp.
static const float DetailPlains = 0.012;
static const float DetailHills = 0.03;
static const float DetailMountains = 0.065;
static const float ChainStart = 0.84;
static const float ChainEnd = 0.97;
static const float ChainBase = 0.25;
static const float RidgeContrast = 0.95;
static const float RidgeRounding = 0.06;
static const float MountainWeight = 0.85;
static const float HillWeight = 0.15;
static const float WarpLarge = 0.06;
static const float WarpSmall = 0.012;

// Couches du relief, comme Layers dans Planet.cpp : premières et dernières octaves, gain, crêtes, à la largeur des
// montagnes, octaves perdues plus tôt, érosion et octaves dont la pente compte.
struct Layer
{
    uint first;
    uint last;
    float gain;
    bool ridged;
    bool scaled;
    bool shifted;
    float erosion;
    uint erodedOctaves;
};
static const Layer Layers[8] = {
    { 0, 5, 0.5, false, false, false, 0.0, 0 },   // continents
    { 1, 3, 0.5, false, true, false, 0.0, 0 },   // zones des chaînes de montagnes
    { 2, 5, 0.5, false, true, false, 0.0, 0 },   // zones de collines
    { 2, 10, 0.5, true, true, true, 2.5, 8 },   // montagnes (crêtes)
    { 4, 9, 0.5, false, true, true, 4.0, 5 },   // collines
    { 6, 22, 0.58, false, true, true, 3.0, 6 }, // détail
    { 0, 3, 0.5, false, false, false, 0.0, 0 },   // grande déformation
    { 3, 6, 0.5, false, false, false, 0.0, 0 },   // petite déformation
};

float Fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float FadeSlope(float t) { return 30.0 * t * t * (t * (t - 2.0) + 1.0); }

// Vecteur du gradient de Perlin choisi par hash (GradientVector dans Planet.cpp).
float3 GradientVector(uint hash)
{
    const uint h = hash & 15u;
    const float su = (h & 1u) != 0 ? -1.0 : 1.0;
    const float sv = (h & 2u) != 0 ? -1.0 : 1.0;
    float3 g = float3(0.0, 0.0, 0.0);
    if (h < 8) { g.x += su; } else { g.y += su; }
    if (h < 4) { g.y += sv; } else if (h == 12 || h == 14) { g.x += sv; } else { g.z += sv; }
    return g;
}

// Bruit de Perlin au point cell + q (cell : maille du réseau modulo 256 ; q : position, petite, depuis cette maille) :
// x la valeur, yzw sa pente (Terrain::Noise).
float4 Noise(int3 cell, float3 q)
{
    const float3 floored = floor(q);
    const int3 c = (cell + int3(floored)) & 255;
    const float3 f = q - floored;
    const float3 u = float3(Fade(f.x), Fade(f.y), Fade(f.z));
    const float3 du = float3(FadeSlope(f.x), FadeSlope(f.y), FadeSlope(f.z));
    const uint A = permutation[c.x] + uint(c.y);
    const uint AA = permutation[A] + uint(c.z);
    const uint AB = permutation[A + 1] + uint(c.z);
    const uint B = permutation[c.x + 1] + uint(c.y);
    const uint BA = permutation[B] + uint(c.z);
    const uint BB = permutation[B + 1] + uint(c.z);
    const float3 ga = GradientVector(permutation[AA]), gb = GradientVector(permutation[BA]);
    const float3 gc = GradientVector(permutation[AB]), gd = GradientVector(permutation[BB]);
    const float3 ge = GradientVector(permutation[AA + 1]), gf = GradientVector(permutation[BA + 1]);
    const float3 gg = GradientVector(permutation[AB + 1]), gh = GradientVector(permutation[BB + 1]);
    const float va = dot(ga, f), vb = dot(gb, f - float3(1, 0, 0)), vc = dot(gc, f - float3(0, 1, 0));
    const float vd = dot(gd, f - float3(1, 1, 0)), ve = dot(ge, f - float3(0, 0, 1)), vf = dot(gf, f - float3(1, 0, 1));
    const float vg = dot(gg, f - float3(0, 1, 1)), vh = dot(gh, f - float3(1, 1, 1));
    const float k1 = vb - va, k2 = vc - va, k3 = ve - va;
    const float k4 = va - vb - vc + vd, k5 = va - vc - ve + vg, k6 = va - vb - ve + vf;
    const float k7 = -va + vb + vc - vd + ve - vf - vg + vh;
    const float value = va + u.x * k1 + u.y * k2 + u.z * k3 + u.x * u.y * k4 + u.y * u.z * k5 + u.z * u.x * k6
                      + u.x * u.y * u.z * k7;
    float3 gradient = ga + (gb - ga) * u.x + (gc - ga) * u.y + (ge - ga) * u.z + (ga - gb - gc + gd) * (u.x * u.y)
                    + (ga - gc - ge + gg) * (u.y * u.z) + (ga - gb - ge + gf) * (u.z * u.x)
                    + (-ga + gb + gc - gd + ge - gf - gg + gh) * (u.x * u.y * u.z);
    gradient += float3(du.x * (k1 + u.y * k4 + u.z * k6 + u.y * u.z * k7),
                       du.y * (k2 + u.z * k5 + u.x * k4 + u.z * u.x * k7),
                       du.z * (k3 + u.x * k6 + u.y * k5 + u.x * u.y * k7));
    return float4(value, gradient);
}

uint FirstOctaveIndex(uint layer)
{
    uint index = 0;
    for (uint l = 0; l < layer; ++l) { index += Layers[l].last - Layers[l].first; }
    return index;
}

float3 Tangent(float3 v, float3 direction) { return v - direction * dot(v, direction); }

// Terrain::Fbm : `point` = direction (déformée ou non) - direction du centre de la tuile.
float Fbm(float3 point, float3 direction, uint layer, uint octaves, float slopeScale)
{
    const Layer L = Layers[layer];
    const float total = (1.0 - pow(L.gain, float(L.last - L.first))) / (1.0 - L.gain);
    // Fréquences déjà multipliées dans fractions.w ; la couche perd ses octaves options.w niveaux plus tôt.
    const uint shift = L.shifted ? tile.options.w : 0;
    const uint last = min(L.last, octaves > shift ? octaves - shift : 0);
    const uint base = FirstOctaveIndex(layer);
    float sum = 0.0;
    float amplitude = 1.0 / total;
    float3 slope = float3(0.0, 0.0, 0.0);
    for (uint k = L.first; k < last; ++k) {
        const uint o = base + k - L.first;
        const float4 fraction = tile.fractions[o];
        const float4 n = Noise(tile.cells[o].xyz, fraction.xyz + point * fraction.w);
        float value = n.x;
        float ridgeSlope = 1.0;
        if (L.ridged) {
            const float rounded = sqrt(n.x * n.x + RidgeRounding * RidgeRounding);
            const float ridge = 1.0 - rounded;
            value = ridge * ridge * ridge;
            ridgeSlope = -3.0 * ridge * ridge * n.x / rounded;
        }
        if (k - L.first < L.erodedOctaves) {
            slope += Tangent(n.yzw, direction) * (fraction.w * ridgeSlope * amplitude);
        }
        const float steep = slopeScale * length(slope);
        sum += amplitude * value / (1.0 + L.erosion * steep * steep);
        amplitude *= L.gain;
    }
    return sum;
}

// Terrain::Warp : toutes les octaves de la couche, en radians.
float3 Warp(float3 delta, float3 direction, uint layer, float amount)
{
    const Layer L = Layers[layer];
    const float total = (1.0 - pow(L.gain, float(L.last - L.first))) / (1.0 - L.gain);
    const uint base = FirstOctaveIndex(layer);
    float3 warp = float3(0.0, 0.0, 0.0);
    float amplitude = amount / total;
    for (uint k = L.first; k < L.last; ++k) {
        const uint o = base + k - L.first;
        const float4 fraction = tile.fractions[o];
        warp += Tangent(Noise(tile.cells[o].xyz, fraction.xyz + delta * fraction.w).yzw, direction) * amplitude;
        amplitude *= L.gain;
    }
    return warp;
}

// Terrain::GetHeight.
float Height(float3 delta, uint octaves)
{
    const float relief = tile.axisS.w;
    if (relief <= 0.0) { return 0.0; }
    const float toSlope = relief / tile.axisN.w;
    const float3 direction = normalize(tile.center.xyz + delta);
    const float3 large = delta + Warp(delta, direction, 6, WarpLarge);
    const float3 small = large + Warp(delta, direction, 7, WarpSmall);
    const float continents = Fbm(large, direction, 0, octaves, 0.0);
    const float land = smoothstep(-0.26, 0.06, continents);
    const float hillZone = smoothstep(0.05, 0.4, Fbm(large, direction, 2, octaves, 0.0));
    const float chains = 1.0 - abs(Fbm(large, direction, 1, octaves, 0.0));
    const float mountainZone = smoothstep(ChainStart, ChainEnd, chains) * smoothstep(0.0, 0.2, continents);
    const float mountains = mountainZone > 0.0 ? Fbm(small, direction, 3, octaves, toSlope * MountainWeight * mountainZone) : 0.0;
    const float hills = hillZone > 0.0 ? Fbm(small, direction, 4, octaves, toSlope * HillWeight * hillZone) : 0.0;
    const float roughness = DetailPlains + DetailHills * hillZone + DetailMountains * mountainZone;
    const float detail = Fbm(delta, direction, 5, octaves, toSlope * roughness);
    const float height = -0.4 + 0.45 * land + 0.15 * continents + MountainWeight * mountainZone * (ChainBase + RidgeContrast * mountains)
                       + HillWeight * hillZone * hills + roughness * detail;
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
    index += constants.firstVertex;
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

// --- Arbres (VegetationRenderer) ---

// Places possibles d'arbres par côté d'une plaine (une tuile de PlanetRenderer, de niveau VegetationRenderer::CellLevel) :
// deux par maille de la grille, chacune décalée au hasard dans sa case.
static const uint TreeGrid = 64;

// Doit correspondre à VegetationRenderer::TreeJob : une plaine à semer.
struct TreeJob
{
    uint4 cell;      // face, niveau, x, y de la tuile (le hasard en dépend : mêmes arbres à chaque fois)
    uint4 options;   // x : case de la plaine (premier arbre : x * TreeGrid²) ; y : graine
    float4 placement; // x : densité ; y : part couverte de forêts ; z : cosinus de la pente maximale
    float4 scale;    // x, y : tailles extrêmes
    float4 shape;    // x : rayon ; y : relief ; z : altitude de la neige ; w : haut de la plage (m)
    float4 climate;  // xyz : températures à l'équateur, aux pôles, humidité ; w : rayon de l'océan (0 : aucun)
};

[[vk::binding(2, 0)]] StructuredBuffer<TreeJob> treeJobs;
// Deux float4 par arbre : position relative au point de référence de la tuile, taille ; direction du haut (repère de la
// planète), angle autour du haut (radians).
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> trees;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> treeCounts; // un par case

groupshared uint treeCount;

uint TreeHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float TreeRandom(inout uint state)
{
    state = TreeHash(state + 0x9e3779b9u);
    return float(state >> 8) * (1.0 / 16777216.0);
}

// Delta entre deux colonnes et deux lignes (u, v : position dans la grille de la tuile, de 0 à TileQuads) : a et b
// (tangentes) sont presque linéaires sur une maille, l'erreur est sous le micron.
float3 TreeDelta(float u, float v)
{
    const uint ci = min(uint(u), TileQuads - 1);
    const uint cj = min(uint(v), TileQuads - 1);
    const float4 column = lerp(tile.columns[ci + 2], tile.columns[ci + 3], u - float(ci));
    const float4 row = lerp(tile.columns[cj + 2], tile.columns[cj + 3], v - float(cj));
    const float3 dv = tile.axisS.xyz * column.x + tile.axisT.xyz * row.z;
    const float growth = column.x * column.y + row.z * row.w;
    const float lc = tile.center.w;
    const float l = sqrt(lc * lc + growth);
    const float3 vc = tile.center.xyz * lc;
    return dv / l - vc * (growth / (l * lc * (l + lc)));
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void computeTrees(uint thread : SV_GroupIndex)
{
    const TreeJob job = treeJobs[constants.job];
    for (uint p = thread; p < 128; p += GroupSize) {
        const uint word = tile.permutation[p / 4][p % 4];
        permutation[4 * p + 0] = word & 255u;
        permutation[4 * p + 1] = (word >> 8) & 255u;
        permutation[4 * p + 2] = (word >> 16) & 255u;
        permutation[4 * p + 3] = word >> 24;
    }
    if (thread == 0) {
        treeCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const float radius = job.shape.x;
    const float relief = max(job.shape.y, 1.0);
    const uint octaves = tile.options.x;
    const uint first = job.options.x * TreeGrid * TreeGrid;
    const float step = float(TileQuads) / float(TreeGrid);
    for (uint c = thread; c < TreeGrid * TreeGrid; c += GroupSize) {
        const uint gi = c % TreeGrid;
        const uint gj = c / TreeGrid;
        // Hasard tiré de la place sur la face (et de la graine), pas de la case : mêmes arbres à chaque visite.
        uint state = TreeHash(job.options.y ^ TreeHash(job.cell.x * 0x632be5abu ^ TreeHash(job.cell.z * TreeGrid + gi)
                                                       ^ TreeHash(job.cell.w * TreeGrid + gj) * 0x85ebca6bu));
        const float u = (float(gi) + 0.5 + (TreeRandom(state) - 0.5) * 0.9) * step;
        const float v = (float(gj) + 0.5 + (TreeRandom(state) - 0.5) * 0.9) * step;
        const float keep = TreeRandom(state);
        const float3 delta = TreeDelta(u, v);
        // Forêts : taches de 150 m à 1 km, au niveau zéro (le relief ne les déplace pas). Quelques arbres isolés
        // ailleurs. Le tri se fait avant le relief, cher.
        const float forest = max(ForestAt((tile.center.xyz + delta) * radius, job.placement.y), 0.03);
        if (keep >= job.placement.x * forest) {
            continue;
        }
        const float height = Height(delta, octaves);
        // Ni plage (ni fond de la mer), ni neige (en dessous, la limite des arbres).
        if (height < job.shape.w + 2.0 || height > job.shape.z - 0.1 * relief) {
            continue;
        }
        const float3 position = Local(delta, height);
        const float3 local = tile.center.xyz * tile.axisN.w + position;
        // Climat comme le sol (fragmentPlanet) : il faut de l'herbe, pas de gel.
        const float2 climate = ClimateAt(radius, relief, job.climate.xyz, job.climate.w, local);
        if (keep >= job.placement.x * forest * TreeClimate(climate)) {
            continue;
        }
        // Pente : le sol un quart de maille plus loin le long des deux axes de la face.
        const float e = 0.25;
        const float3 du = Local(TreeDelta(u + e, v), Height(TreeDelta(u + e, v), octaves)) - position;
        const float3 dv = Local(TreeDelta(u, v + e), Height(TreeDelta(u, v + e), octaves)) - position;
        const float3 up = normalize(local);
        if (abs(dot(normalize(cross(du, dv)), up)) < job.placement.z) {
            continue;
        }
        uint slot;
        InterlockedAdd(treeCount, 1u, slot);
        const float scale = lerp(job.scale.x, job.scale.y, TreeRandom(state));
        trees[(first + slot) * 2 + 0] = float4(position, scale);
        trees[(first + slot) * 2 + 1] = float4(up, TreeRandom(state) * 6.2831853);
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        treeCounts[job.options.x] = treeCount;
    }
}
