// Tuiles des planètes calculées par le GPU : un groupe de threads par tuile écrit ses sommets (ModelVertex) et leur
// glissement vers la forme de la parente (MorphVertex), les mêmes que Planetology::BuildTile sur le CPU, qui sert de
// référence (Tests/src/PlanetGpuTests.cpp vérifie qu'ils s'accordent). Le travail en double (directions, points de
// départ du bruit) est fait par Planetology::PrepareTileGpu : ici presque tout est en float, relatif au centre de la tuile.
// Exception, la déformation (Warp) : 0,06 radian, multiplié ensuite par la fréquence des sommets (plusieurs milliers sur
// une planète comme la Terre), il lui faut une précision de 1e-10 radian que le float n'a pas (écart de 1 à 2,5 m avec la
// collision). PlanetTile64.slang compile ce fichier avec FLUX_FLOAT64 : la déformation et les points du bruit y sont en
// double (Precise) ; sans shaderFloat64, Precise reste un float.
// Le relief (Height) doit rester le même que Terrain::GetHeight dans Engine/src/Planetology/Planet.cpp.
// Arbres (computeTrees) : un groupe par plaine de VegetationRenderer sème ses arbres sur ce même relief, au détail le
// plus fin (celui du sol vu de près).

#include "PlanetClimate.slang"

static const uint TileQuads = 32;
static const uint TileSide = TileQuads + 1;
static const uint Wide = TileSide + 2;          // grille avec une rangée de plus autour (normales du bord)
static const uint Coarse = TileQuads / 2 + 3;   // sommets de la parente, de -2 à TileQuads + 2, de deux en deux
static const uint Columns = TileSide + 4;
static const uint OctaveCount = 43;
static const uint GroupSize = 256;
static const uint VertexCount = TileSide * TileSide + 4 * TileSide; // TileVertexCount
static const uint MeshletQuads = 8;                                 // TileMeshletQuads
static const uint MeshletSide = TileQuads / MeshletQuads;
static const uint MeshletCount = MeshletSide * MeshletSide;

#ifdef FLUX_FLOAT64
typealias Precise = double;
typealias Precise3 = vector<double, 3>;
#define PRECISE(x) x##l
#else
typealias Precise = float;
typealias Precise3 = float3;
#define PRECISE(x) x
#endif

// Doit correspondre à Planetology::TileGpuInputs.
struct TileInputs
{
    float4 center; // xyz : direction du centre ; w : |v_c|
    float4 centerLow; // xyz : reste de la direction du centre (d_c - center.xyz)
    float4 axisN;  // xyz : normale de la face ; w : rayon
    float4 axisS;  // xyz : axe s ; w : relief
    float4 axisT;  // xyz : axe t ; w : profondeur des jupes
    uint4 options; // x : octaves ; y : octaves de la parente ; z : niveau ; w : décalage des couches à la largeur des montagnes
    float4 columns[Columns];
    int4 cells[OctaveCount];
    float4 fractions[OctaveCount];
    float4 fractionsLow[OctaveCount]; // reste de fractions (position et fréquence)
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
// Boîte de chaque paquet des mesh shaders (PlanetTileCompute::Meshlet : bas, haut) : MeshletCount par tuile.
[[vk::binding(2, 1)]] RWStructuredBuffer<float4> meshlets;
[[vk::push_constant]] ConstantBuffer<Constants> constants;
// Lue directement dans le buffer (une copie locale de 2,5 Ko déborderait des registres).
#define tile inputs[constants.job]

groupshared uint permutation[512];
groupshared float wideHeights[Wide * Wide];
groupshared uint wideZones[Wide * Wide]; // Terrain::Zones, deux demi-floats (f32tof16)
groupshared float coarseHeights[Coarse * Coarse];
groupshared float lowestShared[GroupSize];
groupshared float radiusShared[GroupSize];
groupshared float highestShared[GroupSize];
// Boîtes des paquets en construction : bas xyz puis haut xyz de chacun, en OrderedFloat (min et max atomiques).
groupshared uint meshletBounds[MeshletCount * 6];

// Réglages du relief, comme dans Planet.cpp.
static const float DetailPlains = 0.012;
static const float DetailHills = 0.03;
static const float DetailMountains = 0.065;
static const float ChainStart = 0.75;
static const float ChainEnd = 0.95;
static const float Foothills = 0.05;
static const float ChainBase = 0.2;
static const float PeakHeight = 0.85;
static const uint PeakChance = 218;
static const float PeakSecondary = 0.45;
static const float PeakSlabInner = 0.35;
static const float PeakSlabOuter = 0.5;
static const float PeakLowest = 0.35;
static const float PeakRadius = 0.75;
static const float PeakShape = 1.6;
static const float PeakTop = 0.5;
static const float PeakTip = 0.05;
static const float PeakAsymmetry = 0.2;
static const float PeakElongation = 0.1;
static const float PeakSpurs = 0.25;
static const float PeakSpurSharpness = 6.0;
static const uint PeakSpurCount = 4;
static const float PeakStretchSoftness = 0.2;
static const float PeakBlend = 0.04;
static const float GullyAmplitude = 0.09;
static const float GullyStartSlope = 0.08;
static const float GullyFullSlope = 0.35;
static const float GullyGain = 0.5;
static const float GullyBranch = 1.5;
static const float GullyJitter = 0.5;
static const float GullySharpness = 4.0;
static const float MountainWeight = 0.85;
static const float HillWeight = 0.15;
static const Precise WarpLarge = PRECISE(0.06);
static const Precise WarpSmall = PRECISE(0.012);

// Couches du relief, comme Layers dans Planet.cpp : premières et dernières octaves, gain, à la largeur des montagnes,
// octaves perdues plus tôt, érosion et octaves dont la pente compte.
struct Layer
{
    uint first;
    uint last;
    float gain;
    bool scaled;
    bool shifted;
    float erosion;
    uint erodedOctaves;
};
static const Layer Layers[9] = {
    { 0, 5, 0.5, false, false, 0.0, 0 },   // continents
    { 1, 3, 0.5, false, false, 0.0, 0 },   // zones des chaînes de montagnes
    { 2, 5, 0.5, false, false, 0.0, 0 },   // zones de collines
    { 2, 4, 0.5, true, true, 0.0, 0 },     // sommets
    { 4, 9, 0.5, true, true, 4.0, 5 },     // collines
    { 6, 22, 0.58, false, false, 3.0, 6 }, // détail
    { 0, 3, 0.5, false, false, 0.0, 0 },   // grande déformation
    { 3, 6, 0.5, false, false, 0.0, 0 },   // petite déformation
    { 3, 7, 0.5, true, true, 0.0, 0 },     // ravines
};

__generic<T : __BuiltinFloatingPointType>
T Fade(T t) { return t * t * t * (t * (t * T(6.0) - T(15.0)) + T(10.0)); }
__generic<T : __BuiltinFloatingPointType>
T FadeSlope(T t) { return T(30.0) * t * t * (t * (t - T(2.0)) + T(1.0)); }

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

// Bruit de Perlin dans la maille `cell` (non masquée) en f (0 à 1) : x la valeur, yzw sa pente (Terrain::Noise). En
// float pour le relief, en Precise pour la déformation.
__generic<T : __BuiltinFloatingPointType>
vector<T, 4> Noise(int3 cell, vector<T, 3> f)
{
    typealias V = vector<T, 3>;
    const int3 c = cell & 255;
    const V u = V(Fade(f.x), Fade(f.y), Fade(f.z));
    const V du = V(FadeSlope(f.x), FadeSlope(f.y), FadeSlope(f.z));
    const uint A = permutation[c.x] + uint(c.y);
    const uint AA = permutation[A] + uint(c.z);
    const uint AB = permutation[A + 1] + uint(c.z);
    const uint B = permutation[c.x + 1] + uint(c.y);
    const uint BA = permutation[B] + uint(c.z);
    const uint BB = permutation[B + 1] + uint(c.z);
    const V ga = V(GradientVector(permutation[AA])), gb = V(GradientVector(permutation[BA]));
    const V gc = V(GradientVector(permutation[AB])), gd = V(GradientVector(permutation[BB]));
    const V ge = V(GradientVector(permutation[AA + 1])), gf = V(GradientVector(permutation[BA + 1]));
    const V gg = V(GradientVector(permutation[AB + 1])), gh = V(GradientVector(permutation[BB + 1]));
    const V x = V(T(1.0), T(0.0), T(0.0)), y = V(T(0.0), T(1.0), T(0.0)), z = V(T(0.0), T(0.0), T(1.0));
    const T va = dot(ga, f), vb = dot(gb, f - x), vc = dot(gc, f - y);
    const T vd = dot(gd, f - x - y), ve = dot(ge, f - z), vf = dot(gf, f - x - z);
    const T vg = dot(gg, f - y - z), vh = dot(gh, f - x - y - z);
    const T k1 = vb - va, k2 = vc - va, k3 = ve - va;
    const T k4 = va - vb - vc + vd, k5 = va - vc - ve + vg, k6 = va - vb - ve + vf;
    const T k7 = -va + vb + vc - vd + ve - vf - vg + vh;
    const T value = va + u.x * k1 + u.y * k2 + u.z * k3 + u.x * u.y * k4 + u.y * u.z * k5 + u.z * u.x * k6
                  + u.x * u.y * u.z * k7;
    V gradient = ga + (gb - ga) * u.x + (gc - ga) * u.y + (ge - ga) * u.z + (ga - gb - gc + gd) * (u.x * u.y)
               + (ga - gc - ge + gg) * (u.y * u.z) + (ga - gb - ge + gf) * (u.z * u.x)
               + (-ga + gb + gc - gd + ge - gf - gg + gh) * (u.x * u.y * u.z);
    gradient += V(du.x * (k1 + u.y * k4 + u.z * k6 + u.y * u.z * k7),
                  du.y * (k2 + u.z * k5 + u.x * k4 + u.z * u.x * k7),
                  du.z * (k3 + u.x * k6 + u.y * k5 + u.x * u.y * k7));
    return vector<T, 4>(value, gradient.x, gradient.y, gradient.z);
}

// Octave o au point `point` (écart à la direction du centre de la tuile) : sa maille (cell, non masquée) et la place
// dedans (f), en Precise. `shift` 0,5 : la maille la plus proche plutôt que celle d'en dessous.
void Lattice(uint o, Precise3 point, Precise shift, out int3 cell, out Precise3 f)
{
    const float4 fraction = tile.fractions[o];
    const float4 low = tile.fractionsLow[o];
    const Precise3 q = Precise3(fraction.xyz) + Precise3(low.xyz) + point * (Precise(fraction.w) + Precise(low.w));
    const Precise3 floored = floor(q + shift);
    cell = tile.cells[o].xyz + int3(floored);
    f = q - floored;
}

uint FirstOctaveIndex(uint layer)
{
    uint index = 0;
    for (uint l = 0; l < layer; ++l) { index += Layers[l].last - Layers[l].first; }
    return index;
}

float3 Tangent(float3 v, float3 direction) { return v - direction * dot(v, direction); }

// Terrain::Fbm : `point` = direction (déformée ou non) - direction du centre de la tuile.
float Fbm(Precise3 point, float3 direction, uint layer, uint octaves, float slopeScale)
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
        int3 cell;
        Precise3 f;
        Lattice(o, point, 0.0, cell, f);
        const float4 n = Noise(cell, float3(f));
        if (k - L.first < L.erodedOctaves) {
            slope += Tangent(n.yzw, direction) * (tile.fractions[o].w * amplitude);
        }
        const float steep = slopeScale * length(slope);
        sum += amplitude * n.x / (1.0 + L.erosion * steep * steep);
        amplitude *= L.gain;
    }
    return sum;
}

float PeakRandom(uint h, uint index) { return float(permutation[h + index]) / 255.0; }
float3 PeakVector(uint h, uint index)
{
    return float3(PeakRandom(h, index), PeakRandom(h, index + 1), PeakRandom(h, index + 2)) - 0.5;
}

// Terrain::Peaks : hauteur des sommets (0 à 1) et sa pente `slope` (par radian, tangente).
float Peaks(Precise3 point, float3 direction, uint octaves, out float3 slope)
{
    const Layer L = Layers[3];
    const uint last = min(L.last, octaves > tile.options.w ? octaves - tile.options.w : 0);
    const uint base = FirstOctaveIndex(3);
    float sum = 0.0;
    float3 gradient = float3(0.0, 0.0, 0.0);
    float scale = 1.0;
    for (uint k = L.first; k < last; ++k) {
        const uint o = base + k - L.first;
        const float4 fraction = tile.fractions[o];
        int3 cell;
        Precise3 precise;
        Lattice(o, point, 0.0, cell, precise);
        const float3 f = float3(precise);
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                for (int l = -1; l <= 1; ++l) {
                    const int3 c = (cell + int3(i, j, l)) & 255;
                    const uint h = permutation[permutation[permutation[c.x] + uint(c.y)] + uint(c.z)];
                    if (permutation[h + 3] >= PeakChance) { continue; }
                    const float3 e = f - float3(i, j, l) - PeakVector(h, 0) * 0.7 - 0.5;
                    const float radial = dot(e, direction);
                    const float slab = 1.0 - smoothstep(PeakSlabInner, PeakSlabOuter, abs(radial));
                    const float3 flat = e - direction * radial;
                    const float r2 = dot(flat, flat);
                    if (slab <= 0.0 || r2 >= 1.0) { continue; }
                    const float raw = PeakRandom(h, 4);
                    const float height = (PeakLowest + (1.0 - PeakLowest) * raw) * slab * scale;
                    const float radius = PeakRadius * (0.6 + 0.4 * raw) * (0.8 + 0.4 * PeakRandom(h, 5));
                    const float r = sqrt(r2);
                    float3 slopeOf = float3(0.0, 0.0, 0.0);
                    float t = 0.0;
                    if (r > 1e-9) {
                        const float3 u = flat / r;
                        const float3 a1 = PeakVector(h, 6);
                        const float3 a2 = PeakVector(h, 9);
                        const float along2 = dot(u, a2);
                        float stretch = 1.0 + 2.0 * PeakAsymmetry * dot(u, a1) + PeakElongation * (4.0 * along2 * along2 - 0.5)
                                      - 0.4 * PeakSpurs;
                        float3 stretchSlope = a1 * (2.0 * PeakAsymmetry) + a2 * (8.0 * PeakElongation * along2);
                        for (uint n = 0; n < PeakSpurCount; ++n) {
                            const float3 spur = PeakVector(h, 12 + 3 * n);
                            const float spurLength = length(spur);
                            const float cosine = spurLength > 0.0 ? dot(u, spur) / spurLength : 0.0;
                            if (cosine > 0.0) {
                                const float lobe = pow(cosine, PeakSpurSharpness - 1.0);
                                stretch += PeakSpurs * lobe * cosine;
                                stretchSlope += spur * (PeakSpurs * PeakSpurSharpness * lobe / spurLength);
                            }
                        }
                        const float gap = stretch - 1.0 / radius;
                        const float root = sqrt(gap * gap + PeakStretchSoftness * PeakStretchSoftness);
                        const float clamped = 0.5 * (stretch + 1.0 / radius - root);
                        t = r / (radius * clamped);
                        if (t >= 1.0) { continue; }
                        const float3 g = stretchSlope - u * dot(u, stretchSlope) - direction * dot(direction, stretchSlope);
                        slopeOf = u / (radius * clamped) - g * (0.5 * (1.0 - gap / root) * t / (clamped * r));
                    }
                    const float tip = sqrt(t * t + PeakTip * PeakTip);
                    const float norm = sqrt(1.0 + PeakTip * PeakTip) - PeakTip;
                    const float rounded = (tip - PeakTip) / norm;
                    slopeOf *= t / (tip * norm);
                    const float rest = 1.0 - rounded;
                    const float lower = pow(rest, PeakShape - 1.0);
                    const float shape = lower * rest * (1.0 + PeakTop * PeakShape * rounded);
                    const float shapeSlope = PeakShape * lower * (rest * PeakTop - (1.0 + PeakTop * PeakShape * rounded));
                    const float weight = exp(height * shape / PeakBlend);
                    sum += weight - 1.0;
                    gradient += slopeOf * (height * shapeSlope * weight * fraction.w);
                }
            }
        }
        scale *= PeakSecondary;
    }
    slope = gradient / (1.0 + sum);
    return PeakBlend * log(1.0 + sum);
}

// Terrain::Gullies : ravines sur la pente `slope` (m/m), fraction du relief.
float Gullies(Precise3 point, float3 direction, uint octaves, float3 slope, float amount)
{
    const Layer L = Layers[8];
    const uint last = min(L.last, octaves > tile.options.w ? octaves - tile.options.w : 0);
    const uint base = FirstOctaveIndex(8);
    const float relief = tile.axisS.w;
    const float radius = tile.axisN.w;
    float sum = 0.0;
    float amplitude = GullyAmplitude * amount;
    for (uint k = L.first; k < last; ++k) {
        const float steep = length(slope);
        const float fade = saturate((steep - GullyStartSlope) / (GullyFullSlope - GullyStartSlope));
        const uint o = base + k - L.first;
        const float4 fraction = tile.fractions[o];
        if (fade > 0.0) {
            const float3 along = cross(direction, slope) / steep;
            int3 cell;
            Precise3 precise;
            Lattice(o, point, 0.5, cell, precise);
            const float3 f = float3(precise);
            float value = 0.0, change = 0.0, weights = 0.0;
            for (int i = -1; i <= 1; ++i) {
                for (int j = -1; j <= 1; ++j) {
                    for (int l = -1; l <= 1; ++l) {
                        const int3 c = (cell + int3(i, j, l)) & 255;
                        const uint h = permutation[permutation[permutation[c.x] + uint(c.y)] + uint(c.z)];
                        const float3 jitter = float3(permutation[h], permutation[h + 1], permutation[h + 2]) / 255.0 - 0.5;
                        const float3 d = f - float3(i, j, l) - jitter * GullyJitter;
                        const float w = exp(-GullySharpness * dot(d, d));
                        const float phase = 2.0 * 3.14159265 * dot(d, along);
                        value += cos(phase) * w;
                        change -= sin(phase) * w;
                        weights += w;
                    }
                }
            }
            sum += amplitude * fade * value / weights;
            slope += along * (GullyBranch * relief * amplitude * fade * change / weights * 2.0 * 3.14159265 * fraction.w / radius);
        }
        amplitude *= GullyGain;
    }
    return sum;
}

// Terrain::Warp : toutes les octaves de la couche, en radians, en Precise.
Precise3 Warp(Precise3 delta, Precise3 direction, uint layer, Precise amount)
{
    const Layer L = Layers[layer];
    const uint base = FirstOctaveIndex(layer);
    // Somme des amplitudes par multiplications (exacte avec un gain de 0,5), pas par pow.
    Precise total = 0.0;
    Precise amplitude = 1.0;
    for (uint k = L.first; k < L.last; ++k) {
        total += amplitude;
        amplitude *= Precise(L.gain);
    }
    Precise3 warp = Precise3(0.0, 0.0, 0.0);
    amplitude = amount / total;
    for (uint k = L.first; k < L.last; ++k) {
        int3 cell;
        Precise3 f;
        Lattice(base + k - L.first, delta, 0.0, cell, f);
        const Precise3 slope = Noise(cell, f).yzw;
        warp += (slope - direction * dot(slope, direction)) * amplitude;
        amplitude *= Precise(L.gain);
    }
    return warp;
}

// Terrain::GetHeight ; zones : Terrain::Zones (collines, montagnes et piémonts).
float Height(Precise3 delta, uint octaves, out float2 zones)
{
    zones = float2(0.0);
    const float relief = tile.axisS.w;
    if (relief <= 0.0) { return 0.0; }
    const float toSlope = relief / tile.axisN.w;
    const Precise3 exact = normalize(Precise3(tile.center.xyz) + Precise3(tile.centerLow.xyz) + delta);
    const float3 direction = float3(exact);
    const Precise3 large = delta + Warp(delta, exact, 6, WarpLarge);
    const Precise3 small = large + Warp(delta, exact, 7, WarpSmall);
    const float continents = Fbm(large, direction, 0, octaves, 0.0);
    const float land = smoothstep(-0.26, 0.06, continents);
    const float hillZone = smoothstep(0.05, 0.4, Fbm(large, direction, 2, octaves, 0.0));
    const float chains = 1.0 - abs(Fbm(large, direction, 1, octaves, 0.0));
    const float mountainZone = smoothstep(ChainStart, ChainEnd, chains) * smoothstep(0.0, 0.2, continents);
    zones = float2(hillZone, smoothstep(ChainStart - Foothills, ChainEnd, chains) * smoothstep(0.0, 0.2, continents));
    float peaks = 0.0;
    float gullies = 0.0;
    if (mountainZone > 0.0) {
        float3 slope;
        peaks = Peaks(small, direction, octaves, slope);
        gullies = Gullies(large, direction, octaves, slope * (toSlope * MountainWeight * mountainZone * PeakHeight), mountainZone);
    }
    const float hills = hillZone > 0.0 ? Fbm(small, direction, 4, octaves, toSlope * HillWeight * hillZone) : 0.0;
    const float roughness = DetailPlains + DetailHills * hillZone + DetailMountains * mountainZone;
    const float detail = Fbm(delta, direction, 5, octaves, toSlope * roughness);
    const float height = -0.4 + 0.45 * land + 0.15 * continents + MountainWeight * mountainZone * (ChainBase + PeakHeight * peaks)
                       + gullies + HillWeight * hillZone * hills + roughness * detail;
    return relief * clamp(height, -1.0, 1.0);
}

float Height(Precise3 delta, uint octaves)
{
    float2 zones;
    return Height(delta, octaves, zones);
}

// Écart δ entre la direction du point (colonne ci, ligne cj de inputs.columns) et celle du centre, sans soustraire
// deux vecteurs unitaires voisins : v = v_c + Δv, |v|² = |v_c|² + Δa(a + a_c) + Δb(b + b_c). En Precise pour le relief
// (multiplié par la fréquence des sommets comme la déformation), en float pour les positions.
Precise3 PreciseDelta(uint ci, uint cj)
{
    const float4 column = tile.columns[ci];
    const float4 row = tile.columns[cj];
    const Precise3 dv = Precise3(tile.axisS.xyz) * Precise(column.x) + Precise3(tile.axisT.xyz) * Precise(row.z);
    const Precise growth = Precise(column.x) * Precise(column.y) + Precise(row.z) * Precise(row.w);
    const Precise lc = Precise(tile.center.w);
    const Precise l = sqrt(lc * lc + growth);
    const Precise3 vc = (Precise3(tile.center.xyz) + Precise3(tile.centerLow.xyz)) * lc;
    return dv / l - vc * (growth / (l * lc * (l + lc)));
}
float3 Delta(uint ci, uint cj) { return float3(PreciseDelta(ci, cj)); }

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
float2 WideZones(uint i, uint j)
{
    const uint packed = wideZones[(j + 1) * Wide + (i + 1)];
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

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

// zones : Terrain::Zones, dans les coordonnées de texture (lues par vertexPlanet pour les biomes).
void Write(uint index, float3 position, float3 normal, float3 morphOffset, float3 morphNormal, float2 zones)
{
    index += constants.firstVertex;
    const uint v = index * 8;
    vertices[v + 0] = position.x;
    vertices[v + 1] = position.y;
    vertices[v + 2] = position.z;
    vertices[v + 3] = normal.x;
    vertices[v + 4] = normal.y;
    vertices[v + 5] = normal.z;
    vertices[v + 6] = zones.x;
    vertices[v + 7] = zones.y;
    const uint m = index * 6;
    morph[m + 0] = morphOffset.x;
    morph[m + 1] = morphOffset.y;
    morph[m + 2] = morphOffset.z;
    morph[m + 3] = morphNormal.x;
    morph[m + 4] = morphNormal.y;
    morph[m + 5] = morphNormal.z;
}

// Un float en entier qui garde l'ordre (les négatifs inversés) : InterlockedMin et InterlockedMax s'y appliquent.
uint OrderedFloat(float value)
{
    const uint bits = asuint(value);
    return (bits & 0x80000000u) != 0 ? ~bits : bits | 0x80000000u;
}

float FromOrdered(uint bits)
{
    return asfloat((bits & 0x80000000u) != 0 ? bits & 0x7FFFFFFFu : ~bits);
}

// Le point (sommet, jupe ou forme parente) entre dans la boîte des paquets qui touchent le point (i, j) de la grille :
// jusqu'à quatre sur le bord de deux paquets.
void AddToMeshlets(uint i, uint j, float3 position)
{
    const uint2 last = min(uint2(i, j) / MeshletQuads, MeshletSide - 1);
    const uint2 first = uint2(i % MeshletQuads == 0 && i > 0 ? i / MeshletQuads - 1 : last.x,
                              j % MeshletQuads == 0 && j > 0 ? j / MeshletQuads - 1 : last.y);
    for (uint my = first.y; my <= last.y; ++my) {
        for (uint mx = first.x; mx <= last.x; ++mx) {
            const uint at = (my * MeshletSide + mx) * 6;
            for (uint axis = 0; axis < 3; ++axis) {
                const uint value = OrderedFloat(position[axis]);
                InterlockedMin(meshletBounds[at + axis], value);
                InterlockedMax(meshletBounds[at + 3 + axis], value);
            }
        }
    }
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
    for (uint b = thread; b < MeshletCount * 6; b += GroupSize) {
        meshletBounds[b] = b % 6 < 3 ? 0xFFFFFFFFu : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Hauteurs : la grille large avec les octaves de la tuile, celle de la parente avec les siennes.
    for (uint p = thread; p < Wide * Wide + Coarse * Coarse; p += GroupSize) {
        if (p < Wide * Wide) {
            const uint i = p % Wide;
            const uint j = p / Wide;
            float2 zones;
            wideHeights[p] = Height(PreciseDelta(i + 1, j + 1), tile.options.x, zones);
            wideZones[p] = f32tof16(zones.x) | (f32tof16(zones.y) << 16);
        } else {
            const uint c = p - Wide * Wide;
            const uint ci = c % Coarse;
            const uint cj = c / Coarse;
            coarseHeights[c] = Height(PreciseDelta(2 * ci, 2 * cj), tile.options.y);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    float lowest = 1.0e30;
    float highest = 0.0;
    float radius = 0.0;
    for (uint v = thread; v < TileSide * TileSide; v += GroupSize) {
        const TileVertex vertex = BuildVertex(v % TileSide, v / TileSide);
        Write(v, vertex.position, vertex.normal, vertex.morphOffset, vertex.morphNormal, WideZones(v % TileSide, v / TileSide));
        const float3 target = vertex.position + vertex.morphOffset;
        AddToMeshlets(v % TileSide, v / TileSide, vertex.position);
        AddToMeshlets(v % TileSide, v / TileSide, target);
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
        Write(TileSide * TileSide + k, position, vertex.normal, vertex.morphOffset, vertex.morphNormal, WideZones(at.x, at.y));
        radius = max(radius, length(position + vertex.morphOffset));
        AddToMeshlets(at.x, at.y, position);
        AddToMeshlets(at.x, at.y, position + vertex.morphOffset);
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
    // Les boîtes sont complètes : la réduction ci-dessus a passé des barrières depuis le dernier AddToMeshlets.
    if (thread < MeshletCount) {
        const uint at = thread * 6;
        const uint slot = (constants.firstVertex / VertexCount * MeshletCount + thread) * 2;
        meshlets[slot] = float4(FromOrdered(meshletBounds[at]), FromOrdered(meshletBounds[at + 1]),
                                FromOrdered(meshletBounds[at + 2]), 0.0);
        meshlets[slot + 1] = float4(FromOrdered(meshletBounds[at + 3]), FromOrdered(meshletBounds[at + 4]),
                                    FromOrdered(meshletBounds[at + 5]), 0.0);
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
    float4 shape;    // x : rayon ; y : relief ; z : altitude de la neige ; w : haut de la plage (m)
    float4 climate;  // xyz : températures à l'équateur, aux pôles, humidité ; w : rayon de l'océan (0 : aucun)
};

static const uint MaxSpecies = 8; // VegetationLibrary::MaxSpecies

// Doit correspondre à VegetationRenderer::SowVegetation : biomes et espèces de la planète.
struct SowVegetation
{
    Biome biomes[MaxBiomes];
    uint4 biomeSpecies[MaxBiomes]; // espèces de chaque biome, 4 au plus : première variante (dans species) | nombre << 8
    float4 biomeShares[MaxBiomes]; // leurs parts cumulées (la dernière à 1)
    float4 species[MaxSpecies];    // x, y : tailles extrêmes (x le modèle) ; z : cosinus de la pente maximale
    uint4 counts;                  // x : biomes
};

[[vk::binding(2, 0)]] StructuredBuffer<TreeJob> treeJobs;
// Deux float4 par arbre : position relative au point de référence de la tuile, taille ; direction du haut (repère de la
// planète), espèce x 8 + angle autour du haut (radians, moins de 8).
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> trees;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> treeCounts; // un par case
[[vk::binding(5, 0)]] StructuredBuffer<SowVegetation> sowVegetation;

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

// Avant le semis : les compteurs des plaines de l'image à zéro (constants.job : nombre de plaines), les groupes de
// computeTrees y ajoutent leurs arbres.
[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void clearTreeCounts(uint thread : SV_GroupIndex)
{
    if (thread < constants.job) {
        treeCounts[treeJobs[thread].options.x] = 0;
    }
}

// Une place de la plaine : false sans arbre, sinon ses deux float4 (voir trees).
bool SowPlace(TreeJob job, uint c, out float4 first, out float4 second)
{
    first = float4(0.0);
    second = float4(0.0);
    const float radius = job.shape.x;
    const float relief = max(job.shape.y, 1.0);
    const uint octaves = tile.options.x;
    const float step = float(TileQuads) / float(TreeGrid);
    const uint gi = c % TreeGrid;
    const uint gj = c / TreeGrid;
    // Hasard tiré de la place sur la face (et de la graine), pas de la case : mêmes arbres à chaque visite.
    uint state = TreeHash(job.options.y ^ TreeHash(job.cell.x * 0x632be5abu ^ TreeHash(job.cell.z * TreeGrid + gi)
                                                   ^ TreeHash(job.cell.w * TreeGrid + gj) * 0x85ebca6bu));
    const float u = (float(gi) + 0.5 + (TreeRandom(state) - 0.5) * 0.9) * step;
    const float v = (float(gj) + 0.5 + (TreeRandom(state) - 0.5) * 0.9) * step;
    const float keep = TreeRandom(state);
    const float pick = TreeRandom(state);
    const float jitter = TreeRandom(state);
    const float3 delta = TreeDelta(u, v);
    // Forêts : taches de 150 m à 1 km, au niveau zéro (le relief ne les déplace pas), plus ou moins couvrantes
    // selon le biome. Quelques arbres isolés ailleurs. Le tri se fait avant le relief, cher, sur la forêt la plus
    // dense des biomes.
    const float noise = ForestNoise((tile.center.xyz + delta) * radius);
    const uint biomeCount = min(sowVegetation[0].counts.x, MaxBiomes);
    float densest = 0.0;
    for (uint b = 0; b < biomeCount; ++b) {
        densest = max(densest, sowVegetation[0].biomes[b].humidity.z * max(ForestMask(noise, sowVegetation[0].biomes[b].zones.w), 0.03));
    }
    if (keep >= densest) {
        return false;
    }
    float2 zones;
    const float height = Height(Precise3(delta), octaves, zones);
    // Ni plage (ni fond de la mer), ni neige (en dessous, la limite des arbres).
    if (height < job.shape.w + 2.0 || height > job.shape.z - 0.1 * relief) {
        return false;
    }
    const float3 position = Local(delta, height);
    const float3 local = tile.center.xyz * tile.axisN.w + position;
    // Biome : tiré au hasard selon son poids au point (BiomeFit, dans l'ordre de la planète), sa densité et sa forêt
    // (le même nombre décide s'il y a un arbre), puis l'espèce selon les parts du biome.
    float altitude = height / relief;
    float2 climate = ClimateAt(radius, relief, job.climate.xyz, job.climate.w, local);
    JitterBiomeConditions(jitter, altitude, climate);
    const float3 zoneShares = ZoneShares(zones);
    uint biome = MaxBiomes;
    float remaining = 1.0;
    float total = 0.0;
    for (uint b = 0; b < biomeCount; ++b) {
        const Biome candidate = sowVegetation[0].biomes[b];
        const float weight = BiomeFit(candidate, zoneShares, altitude, climate) * remaining;
        remaining -= weight;
        total += weight * candidate.humidity.z * max(ForestMask(noise, candidate.zones.w), 0.03);
        if (biome == MaxBiomes && keep < total) {
            biome = b;
        }
    }
    if (biome == MaxBiomes) {
        return false;
    }
    const float4 shares = sowVegetation[0].biomeShares[biome];
    const uint choice = pick < shares.x ? 0 : pick < shares.y ? 1 : pick < shares.z ? 2 : 3;
    // Puis la variante, avec ce qui reste du même nombre dans la part de l'espèce.
    const float low = choice == 0 ? 0.0 : shares[choice - 1];
    const uint entry = sowVegetation[0].biomeSpecies[biome][choice];
    const uint variants = max(entry >> 8, 1u);
    const uint variant = min(uint((pick - low) / max(shares[choice] - low, 1.0e-6) * float(variants)), variants - 1);
    const uint species = min((entry & 255u) + variant, MaxSpecies - 1);
    const float4 traits = sowVegetation[0].species[species];
    // Pente : le sol un quart de maille plus loin le long des deux axes de la face.
    const float e = 0.25;
    const float3 du = Local(TreeDelta(u + e, v), Height(Precise3(TreeDelta(u + e, v)), octaves)) - position;
    const float3 dv = Local(TreeDelta(u, v + e), Height(Precise3(TreeDelta(u, v + e)), octaves)) - position;
    const float3 up = normalize(local);
    if (abs(dot(normalize(cross(du, dv)), up)) < traits.z) {
        return false;
    }
    const float scale = lerp(traits.x, traits.y, TreeRandom(state)) * sowVegetation[0].biomes[biome].humidity.w;
    first = float4(position, scale);
    second = float4(up, float(species) * 8.0 + TreeRandom(state) * 6.2831853);
    return true;
}

groupshared uint treeBase;

// Semis d'une plaine (constants.job) : une place par thread, TreeGrid² / GroupSize groupes par plaine. Un seul
// groupe par plaine laissait presque tout le GPU sans travail (7,6 ms pour 4 plaines sur une RX 7700 XT). Chaque
// groupe compte ses arbres, en réserve la place d'un coup dans la liste de la plaine (un atomique global par groupe),
// puis les écrit.
[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void computeTrees(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
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

    float4 first;
    float4 second;
    const bool sown = SowPlace(job, group.x * GroupSize + thread, first, second);
    uint slot = 0;
    if (sown) {
        InterlockedAdd(treeCount, 1u, slot);
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        uint base;
        InterlockedAdd(treeCounts[job.options.x], treeCount, base);
        treeBase = base;
    }
    GroupMemoryBarrierWithGroupSync();
    if (sown) {
        const uint at = job.options.x * TreeGrid * TreeGrid + treeBase + slot;
        trees[at * 2 + 0] = first;
        trees[at * 2 + 1] = second;
    }
}
