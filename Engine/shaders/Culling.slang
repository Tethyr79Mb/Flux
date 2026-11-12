// Objets cachés derrière d'autres, écartés par le GPU avant la passe principale (OcclusionCulling) : chaque entité
// d'un lot (et chaque bloc d'un grand modèle) est comparée à la profondeur de l'image précédente, réduite en pyramide
// (chaque niveau garde la profondeur la plus lointaine de 2 x 2 texels du précédent). Une sphère dont le point le plus
// proche est derrière tout ce que l'image précédente voyait à sa place n'est pas dessinée. Reverse-Z : 1 près, 0 à
// l'infini, la plus lointaine est la plus petite.

#include "DepthPyramid.slang"

static const uint GroupSize = 64;
static const uint MaxLevels = 16;

// Doit correspondre à CullFrame dans OcclusionCulling.cpp.
struct CullFrame
{
    float4 previousViewProjection[4]; // par colonnes, de l'image qui a fait la pyramide
    uint4 size;                       // x, y : profondeur source (pixels) ; z : niveaux ; w : entités à tester
    uint4 levelOffsets[MaxLevels / 4]; // premier texel de chaque niveau dans `pyramid`
};

// Doit correspondre à OcclusionCulling::Item.
struct CullItem
{
    float4 sphere; // xyz : centre dans le repère du modèle ; w : rayon dans le monde
    uint command;  // commande de dessin (5 uint dans `commands`) qui reçoit l'entité si elle est visible
    uint draw;     // DrawData de l'entité
    uint material;
    uint padding;
};

// Doit correspondre à DrawData dans shaders/Mesh.slang (seul previousWorld est lu).
struct DrawData
{
    float4 world[4];
    float4 normal[3];
    float4 rim;
    float4 ground;
    float4 previousWorld[4];
    float4 tint;
    float4 emissive;
    float4 surface;
};

[[vk::binding(0, 0)]] StructuredBuffer<CullFrame> frame;
[[vk::binding(1, 0)]] StructuredBuffer<CullItem> items;
[[vk::binding(2, 0)]] StructuredBuffer<DrawData> draws;
// DrawIndexedCommand : indexCount, instanceCount, firstIndex, vertexOffset, firstInstance. Les commandes testées
// arrivent avec instanceCount à 0 ; chaque entité visible s'y ajoute.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> commands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint2> instances; // comme dans Mesh.slang : entité, matériau
[[vk::binding(5, 0)]] StructuredBuffer<float> pyramid;     // celle de l'image précédente
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> stats;      // x : entités cachées (relu par le CPU)

bool Occluded(CullItem item)
{
    const DrawData draw = draws[item.draw];
    const float3 local = item.sphere.xyz;
    const float3 center = (draw.previousWorld[0] * local.x + draw.previousWorld[1] * local.y
                           + draw.previousWorld[2] * local.z + draw.previousWorld[3]).xyz;
    return OccludedInPyramid(pyramid, frame[0].previousViewProjection, frame[0].size, frame[0].levelOffsets, center,
                             item.sphere.w);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void clearMain()
{
    stats[0] = 0;
}

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void cullMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= frame[0].size.w) {
        return;
    }
    const CullItem item = items[id.x];
    if (Occluded(item)) {
        InterlockedAdd(stats[0], 1u);
        return;
    }
    uint slot;
    InterlockedAdd(commands[item.command * 5 + 1], 1u, slot);
    instances[commands[item.command * 5 + 4] + slot] = uint2(item.draw, item.material);
}

// Un niveau de la pyramide : chaque texel garde la plus lointaine de 2 x 2 texels de la source (la profondeur de la
// scène pour le premier niveau, le niveau précédent ensuite), le bord répété quand la source est impaire.
struct PyramidConstants
{
    uint sourceOffset;
    uint sourceWidth;
    uint sourceHeight;
    uint targetOffset;
    uint targetWidth;
    uint targetHeight;
    uint fromDepth; // 1 : la source est `depth`
    uint padding;
};

// Mêmes groupe et layout que le test (un seul groupe par image) : la pyramide de cette image, faite de sa profondeur.
[[vk::binding(7, 0)]] Sampler2D depth;
[[vk::binding(8, 0)]] RWStructuredBuffer<float> target;
[[vk::push_constant]] ConstantBuffer<PyramidConstants> constants;

float ReadSource(uint2 texel)
{
    texel = min(texel, uint2(constants.sourceWidth, constants.sourceHeight) - 1u);
    if (constants.fromDepth != 0) {
        const float2 uv = (float2(texel) + 0.5) / float2(constants.sourceWidth, constants.sourceHeight);
        return depth.SampleLevel(uv, 0.0).r;
    }
    return target[constants.sourceOffset + texel.y * constants.sourceWidth + texel.x];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pyramidMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.targetWidth || id.y >= constants.targetHeight) {
        return;
    }
    const uint2 source = id.xy * 2u;
    const float farthest = min(min(ReadSource(source), ReadSource(source + uint2(1, 0))),
                               min(ReadSource(source + uint2(0, 1)), ReadSource(source + uint2(1, 1))));
    target[constants.targetOffset + id.y * constants.targetWidth + id.x] = farthest;
}
