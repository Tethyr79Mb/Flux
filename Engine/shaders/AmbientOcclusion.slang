// Occlusion ambiante GTAO (AmbientOcclusionRenderer), entre la passe « G-buffer » et celle de l'éclairage, d'après
// « Practical Real-Time Strategies for Accurate Indirect Occlusion » (Jimenez et al., Activision 2016) et XeGTAO
// (Intel, 2021) :
// 1. computeAmbientOcclusion : pour chaque pixel du G-buffer, quelques tranches (plans qui contiennent le rayon de
//    l'œil) tournées autour de lui ; dans chacune, l'horizon le plus haut cherché des deux côtés dans la profondeur,
//    puis la part du ciel vue entre ces horizons, pondérée par le cosinus de la normale (intégrale exacte). Angle des
//    tranches et pas tirés par pixel et par image : l'anticrénelage temporel (ou FSR) qui lisse l'image en fait la
//    moyenne, sans historique à part. Pleine taille de rendu (Haute) ou un pixel sur deux en x et en y (Moyenne,
//    écrit dans le quart en haut à gauche de la texture).
// 2. computeDenoise : moyenne 3 x 3 des voisins à la même profondeur (les bords des objets restent nets).
// Sortie : x, la part de la lumière ambiante qui arrive (1 : rien ne la cache) ; y, la distance devant la caméra (m)
// du pixel, que le lissage et l'agrandissement de la demi-résolution (Lighting.slang) comparent.

#include "Octahedron.slang"

// Doit correspondre à AmbientOcclusionParams dans AmbientOcclusionRenderer.cpp.
struct AmbientOcclusionParams
{
    float4 right;    // xyz : droite de la caméra à l'échelle du champ ; w : décalage d'un sous-pixel en x (clip)
    float4 up;       // xyz : haut, de même ; w : décalage en y
    float4 forward;  // xyz : avant (unitaire) ; w : plan proche (profondeur = proche / distance devant la caméra)
    float4 size;     // xy : taille de rendu ; z : mètres par pixel à 1 m devant la caméra ; w : rayon (m)
    float4 settings; // x, y : atténuation (multiplicateur, ajout) ; z : puissance finale (intensité) ; w : rayon max (pixels)
    uint4 options;   // x : numéro de l'image (bruit) ; y : pas entre deux pixels calculés (1 ou 2) ; z : tranches ; w : pas
};
[[vk::push_constant]] ConstantBuffer<AmbientOcclusionParams> params;

[[vk::binding(0, 0)]] Sampler2D sceneDepth;
[[vk::binding(1, 0)]] Sampler2D gbufferBase;    // a : sorte de matière (0 : rien d'écrit, pas d'occlusion)
[[vk::binding(2, 0)]] Sampler2D gbufferNormals; // xy : normale de la carte (octaédrique)
[[vk::binding(3, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> target;
[[vk::binding(4, 0)]] Sampler2D source;         // computeDenoise : l'occlusion brute

static const uint GroupSize = 8; // AmbientOcclusionRenderer.cpp
static const float Pi = 3.14159265;
static const float HalfPi = 1.57079633;
// Distance rangée au plus loin (16 bits flottants : 65 504 au plus). Au-delà, le rayon tient sous un pixel.
static const float MaxStoredDepth = 60000.0;
// Rayon à l'écran (pixels) sous lequel rien n'est cherché, et au-dessus duquel l'occlusion compte en entier.
static const float MinScreenRadius = 1.5;
static const float FadeScreenRadius = 4.0;
// Écart le plus court entre le pixel et un échantillon, en pixels : plus près, on lirait le pixel lui-même.
static const float MinSampleDistance = 1.3;
// Sinus des angles au-dessus du plan tangent où un obstacle commence à compter, puis compte en entier (3° et 9°).
static const float AngleBiasStart = 0.05;
static const float AngleBiasEnd = 0.16;
// Plancher de la sortie : un coin fermé n'est jamais tout noir (XeGTAO garde 3 %).
static const float MinVisibility = 0.03;

// Distance devant la caméra du pixel, 0 pour le fond (reverse-Z infini : profondeur 0 à l'infini).
float ViewDepth(int2 pixel)
{
    const float depth = sceneDepth.Load(int3(pixel, 0)).r;
    return depth > 0.0 ? params.forward.w / depth : 0.0;
}

// Position relative à la caméra du pixel (à la taille de rendu), à `viewDepth` devant elle.
float3 PixelPosition(int2 pixel, float viewDepth)
{
    const float2 uv = (float2(pixel) + 0.5) / params.size.xy;
    const float2 clip = uv * 2.0 - 1.0 - float2(params.right.w, params.up.w); // y vers le bas, sans le sous-pixel
    return (params.forward.xyz + params.right.xyz * clip.x - params.up.xyz * clip.y) * viewDepth;
}

// Angle des tranches : bruit à gradient entrelacé, décalé à chaque image (comme dans RayShadows.slang).
float SliceNoise(float2 cell)
{
    const float2 p = cell + float(params.options.x % 64u) * 5.588238;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Départ des pas : suite R2 (Roberts 2018), décalée à chaque image par le nombre d'or. Un second bruit à gradient
// entrelacé (même lu en x et y échangés) suivait le premier sur les diagonales : hachures régulières sur les surfaces
// vues en biais et traits en étoile autour des contacts (vus sur la RX 7700 XT le 2026-11-18).
float StepNoise(float2 cell)
{
    return frac(0.5 + dot(cell, float2(0.7548776662, 0.5698402910)) + float(params.options.x % 64u) * 0.6180339887);
}

// Un obstacle à peine au-dessus du plan tangent ne compte pas, puis de plus en plus jusqu'à ~9° : une sphère ou un
// cylindre en facettes (profondeur plate par triangle, normale lissée) s'assombrissait en motifs suivant ses facettes
// (vu sur les sphères de la scène des matériaux le 2026-11-18). Même idée que l'« angle bias » de HBAO.
float AngleWeight(float3 horizon, float3 normal)
{
    return saturate((dot(horizon, normal) - AngleBiasStart) / (AngleBiasEnd - AngleBiasStart));
}

// acos à 0,2 % près, sans l'appel complet (XeGTAO_FastACos).
float FastAcos(float x)
{
    const float a = abs(x);
    const float r = (-0.156583 * a + HalfPi) * sqrt(1.0 - a);
    return x >= 0.0 ? r : Pi - r;
}

[shader("compute")]
[numthreads(GroupSize, GroupSize, 1)]
void computeAmbientOcclusion(uint3 id : SV_DispatchThreadID)
{
    const uint stride = params.options.y;
    const int2 size = int2(params.size.xy);
    const int2 pixel = int2(id.xy * stride);
    if (any(pixel >= size)) {
        return;
    }
    // Rien d'écrit (fond, océan et transparents dessinés après) : pas d'occlusion.
    const float viewDepth = ViewDepth(pixel);
    if (gbufferBase.Load(int3(pixel, 0)).a == 0.0 || viewDepth == 0.0) {
        target[id.xy] = float2(1.0, 0.0);
        return;
    }
    const float storedDepth = min(viewDepth, MaxStoredDepth);
    // Rayon à l'écran : sous un pixel et demi, rien de visible à chercher (le sol au loin, une planète vue d'orbite).
    float screenRadius = params.size.w / (viewDepth * params.size.z);
    if (screenRadius < MinScreenRadius) {
        target[id.xy] = float2(1.0, storedDepth);
        return;
    }
    // Fondu vers « rien ne cache » juste au-dessus : la limite ne trace plus de bande sur le sol au loin.
    const float fade = saturate((screenRadius - MinScreenRadius) / (FadeScreenRadius - MinScreenRadius));
    // Rayon borné à l'écran tout près de la caméra : les lectures trop loin les unes des autres ratent le cache.
    screenRadius = min(screenRadius, params.settings.w);

    const float3 position = PixelPosition(pixel, viewDepth);
    const float3 toEye = normalize(-position);
    float3 normal = DecodeOctahedron(gbufferNormals.Load(int3(pixel, 0)).xy);
    if (dot(normal, toEye) < 0.0) {
        normal = -normal; // face arrière (feuilles, brins à deux faces) : l'occlusion du côté vu
    }
    const float3 right = normalize(params.right.xyz);
    const float3 up = normalize(params.up.xyz);
    const uint slices = params.options.z;
    const uint steps = params.options.w;
    // Bruits tirés sur la grille des pixels calculés : en Moyenne, sur les seuls pixels pairs, le motif du bruit à
    // gradient entrelacé se répétait en bandes.
    const float sliceNoise = SliceNoise(float2(id.xy));
    const float stepNoise = StepNoise(float2(id.xy));
    const float minStep = MinSampleDistance / screenRadius;

    float visibility = 0.0;
    for (uint slice = 0; slice < slices; ++slice) {
        // Direction de la tranche à l'écran (y vers le bas) et la même dans le monde.
        const float angle = (float(slice) + sliceNoise) * Pi / float(slices);
        const float2 omega = float2(cos(angle), sin(angle));
        const float3 direction = right * omega.x - up * omega.y;
        // Plan de la tranche : la direction redressée face à l'œil, et son axe ; la normale projetée dans ce plan.
        const float3 orthoDirection = direction - dot(direction, toEye) * toEye;
        const float3 axis = normalize(cross(orthoDirection, toEye));
        const float3 projectedNormal = normal - axis * dot(normal, axis);
        const float projectedLength = length(projectedNormal);
        const float normalSign = dot(orthoDirection, projectedNormal) >= 0.0 ? 1.0 : -1.0;
        const float cosNormal = saturate(dot(projectedNormal, toEye) / max(projectedLength, 1.0e-6));
        const float n = normalSign * FastAcos(cosNormal);
        // Horizons de départ : le plan tangent, de chaque côté (rien ne cache le ciel).
        const float lowCos0 = cos(n + HalfPi);
        const float lowCos1 = cos(n - HalfPi);
        float horizonCos0 = lowCos0; // du côté de +omega
        float horizonCos1 = lowCos1; // du côté de -omega
        for (uint j = 0; j < steps; ++j) {
            // Pas plus serrés près du pixel (puissance 2), où l'occlusion change le plus.
            float s = (float(j) + stepNoise) / float(steps);
            s = s * s + minStep;
            const int2 offset = int2(round(omega * (s * screenRadius)));
            const int2 sample0 = clamp(pixel + offset, int2(0), size - 1);
            const int2 sample1 = clamp(pixel - offset, int2(0), size - 1);
            const float depth0 = ViewDepth(sample0);
            const float depth1 = ViewDepth(sample1);
            // Atténuation : un obstacle au-delà du rayon ne compte plus (fondu sur la fin du rayon).
            if (depth0 > 0.0) {
                const float3 delta = PixelPosition(sample0, depth0) - position;
                const float distance = length(delta);
                const float3 horizon = delta / max(distance, 1.0e-6);
                const float weight = saturate(distance * params.settings.x + params.settings.y) * AngleWeight(horizon, normal);
                horizonCos0 = max(horizonCos0, lerp(lowCos0, dot(horizon, toEye), weight));
            }
            if (depth1 > 0.0) {
                const float3 delta = PixelPosition(sample1, depth1) - position;
                const float distance = length(delta);
                const float3 horizon = delta / max(distance, 1.0e-6);
                const float weight = saturate(distance * params.settings.x + params.settings.y) * AngleWeight(horizon, normal);
                horizonCos1 = max(horizonCos1, lerp(lowCos1, dot(horizon, toEye), weight));
            }
        }
        // Angles des horizons depuis l'œil, gardés dans l'hémisphère de la normale ; intégrale du cosinus entre eux.
        float h0 = -FastAcos(horizonCos1);
        float h1 = FastAcos(horizonCos0);
        h0 = n + clamp(h0 - n, -HalfPi, HalfPi);
        h1 = n + clamp(h1 - n, -HalfPi, HalfPi);
        const float sinN = sin(n);
        const float arc0 = (cosNormal + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
        const float arc1 = (cosNormal + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
        visibility += projectedLength * (arc0 + arc1);
    }
    visibility = pow(saturate(visibility / float(slices)), params.settings.z);
    target[id.xy] = float2(lerp(1.0, max(visibility, MinVisibility), fade), storedDepth);
}

// Poids d'un voisin selon l'écart de distance (relatif) : nul au-delà de 5 %, une autre surface.
float DepthWeight(float depth, float center)
{
    return saturate(1.0 - abs(depth - center) / (0.05 * center));
}

[shader("compute")]
[numthreads(GroupSize, GroupSize, 1)]
void computeDenoise(uint3 id : SV_DispatchThreadID)
{
    const uint stride = params.options.y;
    const int2 size = (int2(params.size.xy) + int(stride) - 1) / int(stride); // pixels calculés
    const int2 pixel = int2(id.xy);
    if (any(pixel >= size)) {
        return;
    }
    const float2 center = source.Load(int3(pixel, 0)).xy;
    if (center.y == 0.0) {
        target[pixel] = center;
        return;
    }
    float sum = center.x;
    float total = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            const float2 neighbour = source.Load(int3(clamp(pixel + int2(x, y), int2(0), size - 1), 0)).xy;
            // Noyau 1-2-1 : les coins comptent moitié moins que les côtés.
            const float weight = DepthWeight(neighbour.y, center.y) * ((x == 0 || y == 0) ? 0.5 : 0.25);
            sum += neighbour.x * weight;
            total += weight;
        }
    }
    target[pixel] = float2(sum / total, center.y);
}
