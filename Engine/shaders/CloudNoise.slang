// Bruit 3D des nuages (Planetology/CloudNoise.cpp) : calculé une fois au démarrage dans deux textures 3D qui se
// répètent sans couture, puis lu par le rendu des nuages en marchant le long du rayon. Lire une texture coûte bien
// moins que recalculer ces bruits à chaque pas : c'est ce qui permet assez de pas pour un vrai volume.
// - Forme (128³) : r, bruit de Perlin creusé par du Worley (« Perlin-Worley ») : des masses arrondies et
//   bourgeonnantes ; g, b, a, Worley de plus en plus fin, qui découpe ces masses en bosses de chou-fleur.
// - Détail (32³) : r, g, b, Worley de plus en plus fin, qui ronge les bords d'un nuage.
// Worley ici est inversé (1 au centre d'une cellule, 0 entre deux) et sommé sur trois octaves.

[[vk::binding(0, 0)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> target;

struct Constants
{
    uint size; // côté de la texture (pixels)
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

// Hachage entier (PCG 3D) : trois valeurs de 0 à 1 par cellule, les mêmes sur tous les GPU.
float3 Hash(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float3(v) * (1.0 / 4294967296.0);
}

// Cellule ramenée dans la période : le bruit se répète tous les `period` cellules, donc sans couture d'un bord de la
// texture à l'autre. `seed` sépare les canaux.
uint3 Wrap(int3 cell, int period, uint seed)
{
    return uint3((cell % period + period) % period) + seed * 4096u;
}

// Worley inversé, périodique : 1 sur le point d'une cellule, 0 à une cellule de distance ou plus. p de 0 à 1.
float Worley(float3 p, int period, uint seed)
{
    const float3 scaled = p * float(period);
    const int3 cell = int3(floor(scaled));
    const float3 local = scaled - float3(cell);
    float nearest = 1.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const int3 offset = int3(x, y, z);
                const float3 point = float3(offset) + Hash(Wrap(cell + offset, period, seed));
                const float3 d = point - local;
                nearest = min(nearest, dot(d, d));
            }
        }
    }
    return 1.0 - saturate(sqrt(nearest));
}

// Octaves bornées à une cellule pour 2 pixels : plus fines, elles ne seraient que du grain qui scintille.
float WorleyFbm(float3 p, int period, uint seed)
{
    const int finest = int(constants.size) / 2;
    return Worley(p, min(period, finest), seed) * 0.625 + Worley(p, min(period * 2, finest), seed + 1u) * 0.25
         + Worley(p, min(period * 4, finest), seed + 2u) * 0.125;
}

// Bruit de gradient (Perlin), périodique : -1 à 1 environ.
float Perlin(float3 p, int period, uint seed)
{
    const float3 scaled = p * float(period);
    const int3 cell = int3(floor(scaled));
    const float3 f = scaled - float3(cell);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintique : pas de cassure entre cellules
    float corners[8];
    for (int i = 0; i < 8; ++i) {
        const int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const float3 gradient = normalize(Hash(Wrap(cell + corner, period, seed)) * 2.0 - 1.0 + 1.0e-4);
        corners[i] = dot(gradient, f - float3(corner));
    }
    const float x0 = lerp(lerp(corners[0], corners[1], u.x), lerp(corners[2], corners[3], u.x), u.y);
    const float x1 = lerp(lerp(corners[4], corners[5], u.x), lerp(corners[6], corners[7], u.x), u.y);
    return lerp(x0, x1, u.z);
}

// Perlin sur trois octaves ramené de 0 à 1.
float PerlinFbm(float3 p, int period, uint seed)
{
    const float value = Perlin(p, period, seed) * 0.5714 + Perlin(p, period * 2, seed + 1u) * 0.2857
                      + Perlin(p, period * 4, seed + 2u) * 0.1429;
    return saturate(value * 1.2 + 0.5);
}

float Remap(float value, float from, float to, float newFrom, float newTo)
{
    return newFrom + (value - from) / (to - from) * (newTo - newFrom);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void computeShape(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= constants.size)) {
        return;
    }
    const float3 p = (float3(id) + 0.5) / float(constants.size);
    const float worley = WorleyFbm(p, 4, 10u);
    // Le Perlin donne les grandes masses, le Worley les arrondit : les creux du Worley mordent le Perlin.
    const float perlinWorley = saturate(Remap(PerlinFbm(p, 4, 1u), worley - 1.0, 1.0, 0.0, 1.0));
    target[id] = float4(perlinWorley, worley, WorleyFbm(p, 8, 20u), WorleyFbm(p, 16, 30u));
}

[shader("compute")]
[numthreads(4, 4, 4)]
void computeDetail(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= constants.size)) {
        return;
    }
    const float3 p = (float3(id) + 0.5) / float(constants.size);
    target[id] = float4(WorleyFbm(p, 2, 40u), WorleyFbm(p, 4, 50u), WorleyFbm(p, 8, 60u), 1.0);
}
