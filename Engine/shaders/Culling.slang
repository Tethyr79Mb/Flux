// Objets cachés derrière d'autres, écartés par le GPU avant la passe principale (OcclusionCulling) : chaque entité
// d'un lot (et chaque bloc d'un grand modèle) est comparée à la profondeur de l'image précédente, réduite en pyramide
// (chaque niveau garde la profondeur la plus lointaine de 2 x 2 texels du précédent). Une sphère dont le point le plus
// proche est derrière tout ce que l'image précédente voyait à sa place n'est pas dessinée. Reverse-Z : 1 près, 0 à
// l'infini, la plus lointaine est la plus petite.

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

uint LevelOffset(uint level)
{
    return frame[0].levelOffsets[level / 4][level % 4];
}

// Taille du niveau `level` : la source divisée par 2^(level + 1), arrondie au-dessus.
uint2 LevelSize(uint level)
{
    const uint2 source = frame[0].size.xy;
    return (source + ((2u << level) - 1u)) >> (level + 1u);
}

bool Occluded(CullItem item)
{
    const DrawData draw = draws[item.draw];
    const float3 local = item.sphere.xyz;
    const float3 center = (draw.previousWorld[0] * local.x + draw.previousWorld[1] * local.y
                           + draw.previousWorld[2] * local.z + draw.previousWorld[3]).xyz;
    const float radius = item.sphere.w;
    // Les 8 coins de la boîte autour de la sphère, dans l'image précédente : rectangle à l'écran et point le plus
    // proche. Un coin devant le plan proche (caméra dans la boîte) : visible.
    float2 low = float2(1.0e30, 1.0e30);
    float2 high = float2(-1.0e30, -1.0e30);
    float nearest = 0.0;
    for (uint corner = 0; corner < 8; ++corner) {
        const float3 offset = float3((corner & 1u) != 0 ? radius : -radius, (corner & 2u) != 0 ? radius : -radius,
                                     (corner & 4u) != 0 ? radius : -radius);
        const float3 p = center + offset;
        const float4 clip = frame[0].previousViewProjection[0] * p.x + frame[0].previousViewProjection[1] * p.y
                          + frame[0].previousViewProjection[2] * p.z + frame[0].previousViewProjection[3];
        if (clip.w <= 0.0 || clip.z >= clip.w) {
            return false;
        }
        const float2 ndc = clip.xy / clip.w;
        low = min(low, ndc);
        high = max(high, ndc);
        nearest = max(nearest, clip.z / clip.w);
    }
    // Hors de l'image précédente, même en partie : rien ne dit ce qui la cachait là.
    const float2 uvLow = low * 0.5 + 0.5;
    const float2 uvHigh = high * 0.5 + 0.5;
    if (any(uvLow < 0.0) || any(uvHigh > 1.0)) {
        return false;
    }
    // Pixels couverts, élargis d'un pixel (l'image précédente était décalée d'un sous-pixel par l'anticrénelage).
    const uint2 source = frame[0].size.xy;
    const uint2 pixelLow = uint2(max(int2(uvLow * float2(source)) - 1, int2(0, 0)));
    const uint2 pixelHigh = min(uint2(uvHigh * float2(source)) + 1u, source - 1u);
    // Le niveau le plus fin où le rectangle tient dans 2 x 2 texels : le texel t du niveau k couvre les pixels
    // t x 2^(k+1) à (t + 1) x 2^(k+1) - 1.
    uint level = 0;
    while (level < frame[0].size.z) {
        const uint2 a = pixelLow >> (level + 1u);
        const uint2 b = pixelHigh >> (level + 1u);
        if (b.x - a.x <= 1u && b.y - a.y <= 1u) {
            break;
        }
        ++level;
    }
    if (level >= frame[0].size.z) {
        return false;
    }
    const uint2 size = LevelSize(level);
    const uint2 a = min(pixelLow >> (level + 1u), size - 1u);
    const uint2 b = min(pixelHigh >> (level + 1u), size - 1u);
    const uint offset = LevelOffset(level);
    const float farthest = min(min(pyramid[offset + a.y * size.x + a.x], pyramid[offset + a.y * size.x + b.x]),
                               min(pyramid[offset + b.y * size.x + a.x], pyramid[offset + b.y * size.x + b.x]));
    return nearest < farthest;
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
