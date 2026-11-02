// Lumière tirée du ciel pour les maillages (EnvironmentRenderer.cpp), refaite seulement quand le ciel change.
// 1) sourceFragment : le ciel dans une texture cube HDR de 128 pixels (fond généré, panorama ou dégradé), puis ses mips
//    (réductions du GPU).
// 2) prefilterFragment : chaque niveau de mip des reflets est ce cube vu à travers un reflet GGX de plus en plus rugueux
//    (rugosité = niveau / dernier niveau, comme Mesh.slang le lit). Échantillonnage d'importance filtré (Křivánek et
//    Colbert, GPU Gems 3 ch. 20) : chaque tirage lit le mip dont un texel couvre son angle solide, 64 tirages suffisent
//    sans points brillants qui scintillent.
// 3) irradianceFragment : la lumière diffuse reçue par une surface de chaque orientation (moyenne pondérée par le
//    cosinus, divisée par pi : ce que multiplie la couleur diffuse), dans un petit cube.

#include "CubeMap.slang"

[[vk::binding(0, 0)]] Sampler2D panorama;     // sRGB, ramené en linéaire à la lecture, avec ses mips
[[vk::binding(1, 0)]] SamplerCube background; // fond généré (Sky.slang, bakeFragment), un seul niveau
[[vk::binding(0, 1)]] SamplerCube source;     // le ciel de l'étape 1 avec ses mips

// Doit correspondre à EnvironmentParams dans EnvironmentRenderer.cpp.
struct EnvironmentParams
{
    float4 top;    // dégradé : couleur du haut ; w : taille d'un texel du fond généré (en unités de face, 2 / taille)
    float4 bottom; // dégradé : couleur du bas ; w : mip du panorama
    uint face;     // 0 à 5 : +X, -X, +Y, -Y, +Z, -Z
    uint mode;     // sourceFragment : 0 dégradé, 1 fond généré, 2 panorama
    float roughness;
    float sourceSize; // côté du niveau 0 de source, en pixels
};
[[vk::push_constant]] ConstantBuffer<EnvironmentParams> settings;

static const float Pi = 3.14159265;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 clip     : TEXCOORD; // -1 à 1 sur la face, y vers le bas
};

// Un triangle qui couvre toute la face.
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.clip = clip;
    return output;
}

// Un texel de 128 pixels couvre 8 x 8 texels du fond généré (1024) : 16 lectures bilinéaires placées entre quatre
// texels chacune les moyennent tous, sans en sauter (une étoile de la nébuleuse compte pour sa part).
float3 GeneratedTexel(float2 uv)
{
    const float texel = settings.top.w;
    float3 sum = float3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float2 offset = (float2(x, y) * 2.0 - 3.0) * texel;
            sum += background.SampleLevel(CubeFaceDirection(settings.face, uv.x + offset.x, uv.y + offset.y), 0.0).rgb;
        }
    }
    return sum * (1.0 / 16.0);
}

// 4 x 4 points du panorama par texel, au mip dont un pixel couvre un de ces points.
float3 PanoramaTexel(float2 uv)
{
    const float step = 0.5 / settings.sourceSize; // un quart de texel, en unités de face (2 / taille / 4)
    float3 sum = float3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float2 offset = (float2(x, y) - 1.5) * step;
            const float3 direction = CubeFaceDirection(settings.face, uv.x + offset.x, uv.y + offset.y);
            sum += panorama.SampleLevel(PanoramaUv(direction), settings.bottom.w).rgb;
        }
    }
    return sum * (1.0 / 16.0);
}

[shader("fragment")]
float4 sourceFragment(VertexOutput input) : SV_Target
{
    float3 color;
    if (settings.mode == 1) {
        color = GeneratedTexel(input.clip);
    } else if (settings.mode == 2) {
        color = PanoramaTexel(input.clip);
    } else {
        const float t = CubeFaceDirection(settings.face, input.clip.x, input.clip.y).y * 0.5 + 0.5;
        color = lerp(settings.bottom.rgb, settings.top.rgb, t);
    }
    return float4(min(color, float3(60000.0)), 1.0);
}

// Suite de Hammersley : points bien répartis sur le carré unité, les mêmes pour tous les texels.
float2 Hammersley(uint i, uint count)
{
    return float2((float(i) + 0.5) / float(count), float(reversebits(i)) * 2.3283064365386963e-10);
}

// Repère autour de la normale n.
void Basis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

// Niveau de source dont un texel couvre l'angle solide d'un tirage de densité pdf parmi count.
float SourceMip(float pdf, uint count)
{
    const float texelSolidAngle = 4.0 * Pi / (6.0 * settings.sourceSize * settings.sourceSize);
    const float sampleSolidAngle = 1.0 / (float(count) * pdf + 1.0e-6);
    // +1 : un peu plus flou, contre le bruit des tirages communs à tous les texels.
    return max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0);
}

static const uint PrefilterSamples = 64;

// Comme Karis (« Real Shading in Unreal Engine 4 », 2013) : la vue est prise dans l'axe du reflet (n = v = r), ce
// qui rend le lobe indépendant de l'angle de vue et tient en une texture cube.
[shader("fragment")]
float4 prefilterFragment(VertexOutput input) : SV_Target
{
    const float3 n = CubeFaceDirection(settings.face, input.clip.x, input.clip.y);
    const float roughness = settings.roughness;
    if (roughness <= 0.0) {
        return float4(source.SampleLevel(n, 0.0).rgb, 1.0); // miroir
    }
    const float a = roughness * roughness;
    const float a2 = a * a;
    float3 tangent;
    float3 bitangent;
    Basis(n, tangent, bitangent);
    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint i = 0; i < PrefilterSamples; ++i) {
        const float2 xi = Hammersley(i, PrefilterSamples);
        // Demi-vecteur tiré selon la distribution GGX.
        const float phi = 2.0 * Pi * xi.x;
        const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        const float3 h = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + n * cosTheta;
        const float3 l = 2.0 * dot(n, h) * h - n;
        const float nl = dot(n, l);
        if (nl <= 0.0) {
            continue;
        }
        // Densité du tirage vu depuis l : D(h) cos(h) / (4 v.h), avec v = n.
        const float d = cosTheta * cosTheta * (a2 - 1.0) + 1.0;
        const float distribution = a2 / (Pi * d * d);
        const float pdf = distribution * 0.25;
        sum += source.SampleLevel(l, SourceMip(pdf, PrefilterSamples)).rgb * nl;
        weight += nl;
    }
    return float4(sum / max(weight, 1.0e-4), 1.0);
}

static const uint IrradianceSamples = 128;

[shader("fragment")]
float4 irradianceFragment(VertexOutput input) : SV_Target
{
    const float3 n = CubeFaceDirection(settings.face, input.clip.x, input.clip.y);
    float3 tangent;
    float3 bitangent;
    Basis(n, tangent, bitangent);
    // Tirages selon le cosinus : leur simple moyenne est l'éclairement divisé par pi.
    float3 sum = float3(0.0);
    for (uint i = 0; i < IrradianceSamples; ++i) {
        const float2 xi = Hammersley(i, IrradianceSamples);
        const float phi = 2.0 * Pi * xi.x;
        const float cosTheta = sqrt(1.0 - xi.y);
        const float sinTheta = sqrt(xi.y);
        const float3 l = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + n * cosTheta;
        sum += source.SampleLevel(l, SourceMip(cosTheta / Pi, IrradianceSamples)).rgb;
    }
    return float4(sum / float(IrradianceSamples), 1.0);
}
