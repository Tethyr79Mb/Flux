// Mesure de la lumière de l'image pour l'exposition automatique (ExposureRenderer.cpp) : un histogramme de la
// luminance en log2, pondéré vers le centre de l'image (ce que le joueur regarde compte plus que les bords). Le CPU le
// relit deux images plus tard et en tire l'exposition (Exposure.cpp) : 2 Ko relus par image, rien d'autre à attendre.
// Une lecture bilinéaire par bloc de 2 x 2 pixels (leur moyenne) : quatre fois moins de travail, la même mesure à
// une fraction de stop près. Coût en 1080p : environ 520 000 lectures et autant d'additions en mémoire partagée.

// Doit correspondre à Exposure.hpp (Bins, MinLog2, MaxLog2).
static const uint Bins = 128;
static const float MinLog2 = -10.0; // en dessous : case 0, le noir de l'espace, ignoré par la mesure
static const float MaxLog2 = 10.0;
static const uint GroupSize = 16;

struct Constants
{
    float2 texel;  // 1 / taille de la scène, en pixels
    uint2 samples; // blocs de 2 x 2 pixels mesurés, en largeur et en hauteur
    uint2 extent;  // taille de la scène, en pixels
    uint skipSky;  // 1 : le ciel (profondeur infinie) n'est pas mesuré, ni même compté comme noir
    uint padding;
};

[[vk::binding(0, 0)]] Sampler2D scene;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> histogram; // Bins poids
[[vk::binding(2, 0)]] Sampler2D sceneDepth; // reverse-Z : 0 là où rien n'a été dessiné (le ciel)
[[vk::push_constant]] ConstantBuffer<Constants> constants;

groupshared uint localBins[Bins];

[shader("compute")]
[numthreads(Bins, 1, 1)]
void clearMain(uint3 id : SV_DispatchThreadID)
{
    histogram[id.x] = 0;
}

uint BinOf(float luminance)
{
    if (!(luminance >= exp2(MinLog2))) { // NaN compris
        return 0;
    }
    const float t = (log2(luminance) - MinLog2) / (MaxLog2 - MinLog2);
    return 1 + uint(min(t, 0.99999) * float(Bins - 1));
}

[shader("compute")]
[numthreads(GroupSize, GroupSize, 1)]
void histogramMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < Bins) {
        localBins[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    bool measured = all(id.xy < constants.samples);
    if (measured && constants.skipSky != 0) {
        // Bloc de ciel seul (les quatre pixels) : écarté. Un bord d'objet sur le ciel reste mesuré.
        const int2 last = int2(constants.extent) - 1;
        const int2 corner = int2(id.xy) * 2;
        float depth = 0.0;
        for (int i = 0; i < 4; ++i) {
            depth = max(depth, sceneDepth.Load(int3(min(corner + int2(i & 1, i >> 1), last), 0)).r);
        }
        measured = depth > 0.0;
    }
    if (measured) {
        // Au coin commun de quatre pixels : le filtre bilinéaire en rend la moyenne.
        const float2 uv = (float2(id.xy) * 2.0 + 1.0) * constants.texel;
        const float3 color = scene.SampleLevel(uv, 0.0).rgb;
        const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
        // Poids de 8 au centre à 1 sur un cercle qui touche les bords, 1 au-delà (coins).
        const float2 fromCenter = (uv - 0.5) * 2.0;
        const uint weight = 1 + uint(7.0 * saturate(1.0 - dot(fromCenter, fromCenter)) + 0.5);
        InterlockedAdd(localBins[BinOf(luminance)], weight);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < Bins && localBins[index] != 0) {
        InterlockedAdd(histogram[index], localBins[index]);
    }
}
