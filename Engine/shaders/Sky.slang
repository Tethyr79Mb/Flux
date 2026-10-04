// Ciel : le fond de la scène, à l'infini, dessiné après les opaques là où rien n'a été dessiné (profondeur restée à
// 0 en reverse-Z). Généré ou tiré d'un panorama ; plus le disque du soleil et son halo. Même exposition et même courbe
// de tons que shaders/Mesh.slang.
// Ciel généré : la nébuleuse et la bande galactique (une trentaine de couches de bruit 3D, chères) sont calculées une
// fois dans une texture cube (bakeFragment, quand un réglage change), puis lues ; les étoiles et le soleil, fins et
// peu coûteux, restent calculés au pixel.

// Doit correspondre à SkyData dans SkyRenderer.cpp.
struct SkyData
{
    float4 right;        // xyz : droite de la caméra x tangente du demi-champ horizontal
    float4 up;           // xyz : haut de la caméra x tangente du demi-champ vertical
    float4 forward;      // xyz : avant de la caméra ; w : angle couvert par un pixel (radians)
    float4 toSun;        // xyz : direction du soleil (unitaire) ; w : rayon apparent du disque (radians)
    float4 sunColor;     // rgb : couleur x intensité ; w : exposition
    float4 nebulaColor;  // rgb ; w : densité de la nébuleuse
    float4 nebulaColor2; // rgb ; w : éclat de la bande galactique
    float4 galaxyAxis;   // xyz ; w : quantité d'étoiles
    float4 galaxyCenter; // xyz ; w : éclat des étoiles
    float4 noiseOffset;  // xyz ; w : luminosité du fond
    uint4 options;       // x : graine mélangée ; y : 1 panorama, 0 ciel généré
};

[[vk::binding(0, 0)]] StructuredBuffer<SkyData> sky;
[[vk::binding(1, 0)]] Sampler2D panorama; // sRGB, ramené en linéaire à la lecture
[[vk::binding(2, 0)]] SamplerCube background; // nébuleuse et bande galactique (bakeFragment), en linéaire

// bakeFragment : la face de la texture cube en cours (0 à 5 : +X, -X, +Y, -Y, +Z, -Z).
struct BakeParams
{
    uint face;
};
[[vk::push_constant]] ConstantBuffer<BakeParams> bake;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 clip     : TEXCOORD; // -1 à 1, y vers le bas comme le clip space de Vulkan
};

// Un triangle qui couvre tout l'écran, à la profondeur 0 (infiniment loin).
// SV_VulkanVertexID : l'indice brut de Vulkan ; SV_VertexID demanderait la capacité DrawParameters (non activée).
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.clip = clip;
    return output;
}

// --- Bruit : exactement les calculs de Engine/src/Graphics/Sky.cpp (toute modification des deux côtés). ---

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint HashCell(int3 c, uint seed)
{
    return Hash(uint(c.x) ^ Hash(uint(c.y) ^ Hash(uint(c.z) ^ seed)));
}

float ToUnit(uint hash)
{
    return float(hash >> 8) * (1.0 / 16777216.0);
}

float ValueNoise(float3 p, uint seed)
{
    const float3 cell = floor(p);
    const int3 c = int3(cell);
    const float3 f = p - cell;
    const float3 t = f * f * (3.0 - 2.0 * f);
    const float x00 = lerp(ToUnit(HashCell(c + int3(0, 0, 0), seed)), ToUnit(HashCell(c + int3(1, 0, 0), seed)), t.x);
    const float x10 = lerp(ToUnit(HashCell(c + int3(0, 1, 0), seed)), ToUnit(HashCell(c + int3(1, 1, 0), seed)), t.x);
    const float x01 = lerp(ToUnit(HashCell(c + int3(0, 0, 1), seed)), ToUnit(HashCell(c + int3(1, 0, 1), seed)), t.x);
    const float x11 = lerp(ToUnit(HashCell(c + int3(0, 1, 1), seed)), ToUnit(HashCell(c + int3(1, 1, 1), seed)), t.x);
    return lerp(lerp(x00, x10, t.y), lerp(x01, x11, t.y), t.z);
}

float Fbm(float3 p, int octaves, uint seed)
{
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * ValueNoise(p, seed + uint(i) * 0x9e3779b9U);
        total += amplitude;
        amplitude *= 0.5;
        p = p * 2.03 + float3(17.1, 9.7, 3.3);
    }
    return sum / total;
}

// Nébuleuse et bande galactique, en linéaire, avant la luminosité (SkyBackground dans Sky.cpp).
float3 SkyBackground(float3 direction, SkyData s)
{
    const uint seed = s.options.x;
    const float3 p = direction * 2.2 + s.noiseOffset.xyz;
    float3 color = float3(0.0);

    const float nebula = s.nebulaColor.w;
    if (nebula > 0.0) {
        const float3 warp = float3(Fbm(p + float3(5.2, 1.3, 8.1), 4, seed + 11), Fbm(p + float3(1.7, 9.2, 3.4), 4, seed + 12),
                                   Fbm(p + float3(8.3, 2.8, 6.1), 4, seed + 13));
        const float3 q = p * 0.7 + (warp - 0.5) * 3.0;
        const float n = Fbm(q, 6, seed + 14);
        const float threshold = lerp(0.64, 0.4, nebula);
        const float core = smoothstep(threshold, threshold + 0.3, n);
        const float veil = smoothstep(threshold - 0.2, threshold + 0.1, n);
        const float filaments = 0.55 + 0.45 * smoothstep(0.35, 0.65, Fbm(q * 3.0 + float3(9.1, 2.6, 5.3), 4, seed + 18));
        const float tone = smoothstep(0.4, 0.6, Fbm(q * 0.8 + float3(3.1, 7.4, 1.2), 3, seed + 15));
        const float glow = (core * core * filaments + 0.12 * veil) * (0.4 + 0.6 * nebula);
        color += lerp(s.nebulaColor.rgb, s.nebulaColor2.rgb, tone) * glow;
    }

    const float galaxy = s.nebulaColor2.w;
    if (galaxy > 0.0) {
        const float height = dot(direction, s.galaxyAxis.xyz);
        const float along = dot(direction, s.galaxyCenter.xyz);
        const float width = 0.1 + 0.08 * (along * 0.5 + 0.5);
        const float band = exp(-(height * height) / (width * width));
        const float core = exp(-(1.0 - along) * 4.0);
        const float haze = Fbm(p * 2.5 + float3(11.0, 4.0, 7.0), 5, seed + 16);
        const float dust = smoothstep(0.5, 0.62, Fbm(p * 4.0 + float3(23.0, 5.0, 2.0), 5, seed + 17))
                         * exp(-(height * height) / 0.0025);
        const float light = band * (0.25 + 0.75 * haze) * (1.0 + 1.5 * core) * (1.0 - 0.85 * dust) * galaxy * 0.3;
        color += float3(1.0, 0.88, 0.72) * light;
    }
    return color;
}

// --- Étoiles : seulement ici (trop fines pour la texture cube de la lumière ambiante). ---

// Point d'une face (x, y de -1 à 1) -> direction, pour le découpage des étoiles (pas celui des textures cube).
float3 StarFaceDirection(int face, float2 xy)
{
    switch (face) {
        case 0: return float3(1.0, xy.y, xy.x);
        case 1: return float3(-1.0, xy.y, xy.x);
        case 2: return float3(xy.x, 1.0, xy.y);
        case 3: return float3(xy.x, -1.0, xy.y);
        case 4: return float3(xy.x, xy.y, 1.0);
        default: return float3(xy.x, xy.y, -1.0);
    }
}

// Chaque face du cube autour de la caméra est quadrillée ; chaque case a peut-être une étoile, placée au hasard
// dedans. Les cases voisines sont lues aussi : une étoile près d'un bord n'est pas coupée. Deux couches : beaucoup
// d'étoiles faibles, quelques brillantes. Une étoile fait toujours environ un pixel et demi, quelle que soit la
// résolution : nette sans scintiller.
float3 Stars(float3 d, float pixelAngle, uint seed, float density, float brightness)
{
    const float3 a = abs(d);
    int face;
    float2 xy;
    if (a.x >= a.y && a.x >= a.z) {
        face = d.x > 0.0 ? 0 : 1;
        xy = d.zy / a.x;
    } else if (a.y >= a.z) {
        face = d.y > 0.0 ? 2 : 3;
        xy = d.xz / a.y;
    } else {
        face = d.z > 0.0 ? 4 : 5;
        xy = d.xy / a.z;
    }
    float3 sum = float3(0.0);
    const float radius = pixelAngle * 0.75;
    for (int layer = 0; layer < 2; ++layer) {
        const float cells = layer == 0 ? 320.0 : 48.0;
        const float probability = density * (layer == 0 ? 0.25 : 0.15);
        const int2 cell = int2(floor((xy * 0.5 + 0.5) * cells));
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int2 c = cell + int2(ox, oy);
                const uint h = HashCell(int3(c, face * 8 + layer), seed ^ 0xa511e9b3U);
                const float2 star = (float2(c) + float2(ToUnit(Hash(h + 1)), ToUnit(Hash(h + 2)))) / cells * 2.0 - 1.0;
                // Les cases près des coins d'une face couvrent moins de ciel : moins d'étoiles, pour une densité égale.
                const float area = pow(1.0 + dot(star, star), -1.5);
                if (ToUnit(h) >= probability * area) {
                    continue;
                }
                const float3 offset = d - normalize(StarFaceDirection(face, star));
                const float magnitude = ToUnit(Hash(h + 3));
                const float peak = (layer == 0 ? pow(magnitude, 8.0) * 1.2 : pow(magnitude, 4.0) * 5.0) * brightness;
                // Couleur selon la température : bleutée, blanche ou orangée.
                const float t = ToUnit(Hash(h + 4));
                const float3 tint = t < 0.5 ? float3(0.65 + 0.7 * t, 0.8 + 0.4 * t, 1.0)
                                            : float3(1.0, 1.0 - 0.35 * (t - 0.5), 1.0 - 0.9 * (t - 0.5));
                sum += tint * (peak * exp(-dot(offset, offset) / (radius * radius)));
            }
        }
    }
    return sum;
}

// Point (u, v de -1 à 1, au centre des texels) d'une face de texture cube -> direction (CubeFaceDirection dans
// Sky.cpp : l'ordre et l'orientation des faces de Vulkan).
float3 CubeFaceDirection(uint face, float u, float v)
{
    float3 d;
    switch (face) {
        case 0: d = float3(1.0, -v, -u); break;
        case 1: d = float3(-1.0, -v, u); break;
        case 2: d = float3(u, 1.0, v); break;
        case 3: d = float3(u, -1.0, -v); break;
        case 4: d = float3(u, -v, 1.0); break;
        default: d = float3(-u, -v, -1.0); break;
    }
    return normalize(d);
}

// Une face de la texture cube du fond : le triangle plein écran la couvre, clip donne le point de la face.
[shader("fragment")]
float4 bakeFragment(VertexOutput input) : SV_Target
{
    return float4(SkyBackground(CubeFaceDirection(bake.face, input.clip.x, input.clip.y), sky[0]), 1.0);
}

// Point du panorama vu dans `direction` (PanoramaUv dans Sky.cpp).
float2 PanoramaUv(float3 direction)
{
    const float Pi = 3.14159265;
    return float2(atan2(direction.x, -direction.z) / (2.0 * Pi) + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / Pi);
}

float3 ToneMap(float3 color, float exposure)
{
    color *= exposure * 0.6;
    return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    const SkyData s = sky[0];
    const float3 direction = normalize(s.forward.xyz + s.right.xyz * input.clip.x - s.up.xyz * input.clip.y);
    const float pixelAngle = s.forward.w;

    float3 color;
    if (s.options.y != 0) {
        // Mip choisi d'après l'écart entre pixels voisins ; u fait le tour : sans correction, l'écart d'un bord à
        // l'autre de l'image prendrait le plus petit mip (une ligne floue verticale).
        const float2 uv = PanoramaUv(direction);
        float2 dx = ddx(uv);
        float2 dy = ddy(uv);
        dx.x -= round(dx.x);
        dy.x -= round(dy.x);
        color = panorama.SampleGrad(uv, dx, dy).rgb;
    } else {
        color = background.SampleLevel(direction, 0.0).rgb
              + Stars(direction, pixelAngle, s.options.x, s.galaxyAxis.w, s.galaxyCenter.w);
    }
    color *= s.noiseOffset.w;

    // Soleil : un disque net (bord adouci sur un pixel), un halo serré et une lueur large.
    const float radius = s.toSun.w;
    if (radius > 0.0) {
        const float3 toSun = s.toSun.xyz;
        const float angle = 2.0 * asin(min(length(direction - toSun) * 0.5, 1.0));
        const float disc = saturate((radius - angle) / pixelAngle + 0.5);
        const float halo = exp(-(angle * angle) / (radius * radius * 9.0)) * 0.5 + exp(-angle * 8.0) * 0.06;
        color += s.sunColor.rgb * (disc * 40.0 + halo);
    }
    return float4(ToneMap(color, s.sunColor.w), 1.0);
}
