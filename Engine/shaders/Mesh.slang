// Maillages (cubes et modèles glTF), opaques ou transparents : position relative à la caméra, couleur de base
// (facteur x image) éclairée par un soleil et une lumière ambiante, avec les ombres du soleil (cascades).
// Modèles animés (vertexSkinned) : chaque sommet est d'abord déplacé par ses os (skinning).
// Cartes d'ombres (vertexShadow, vertexShadowSkinned, fragmentShadow) : profondeur vue du soleil, rien d'autre.

// Doit correspondre à FrameData dans MeshRenderer.cpp. Matrices par colonnes : aucune ambiguïté de convention.
struct FrameData
{
    float4 viewProjection[4];
    float4 cascades[4 * 4];   // cascade c : colonnes [4c, 4c + 3] ; relatif caméra -> carte (xy dans [-1, 1], z dans [0, 1])
    float4 cascadeEnds;       // fin de chaque tranche, en mètres devant la caméra
    float4 cascadeTexels;     // côté d'un texel de chaque cascade, en mètres
    float4 cameraForward;     // xyz
    float4 toSun;             // xyz : direction du soleil (unitaire)
};

// Doit correspondre à DrawData dans MeshRenderer.cpp : une entité dessinée.
struct DrawData
{
    float4 world[4];          // relatif à la caméra, par colonnes
    // Matrice des normales par colonnes dans xyz. normal[1].w : premier os du modèle dans jointColumns (bits d'un
    // entier), modèles animés seulement.
    float4 normal[3];
};

// Doit correspondre à DrawParams dans MeshRenderer.cpp : une primitive (un matériau) d'une entité.
struct DrawParams
{
    float4 color;             // linéaire : couleur de base du matériau x teinte de l'entité
    uint draw;                // dans draws
    uint cascade;             // carte d'ombres en cours de dessin
    float alphaCutoff;        // 0 : pas de seuil
    uint transparent;         // 1 : l'alpha sort vers le mélange ; 0 : opaque (alpha 1)
};

[[vk::push_constant]] ConstantBuffer<DrawParams> params;

// Groupe 0 : le matériau. Couleur de base (blanc 1 x 1 sans image), en sRGB : le GPU la ramène en linéaire à la lecture.
[[vk::binding(0, 0)]] Sampler2D baseColorTexture;

// Groupe 1 : l'image en cours.
// Matrices des os de tous les modèles animés de l'image (monde du modèle x inverse de liaison), 4 colonnes par os.
[[vk::binding(0, 1)]] StructuredBuffer<float4> jointColumns;
[[vk::binding(1, 1)]] StructuredBuffer<DrawData> draws;
[[vk::binding(2, 1)]] StructuredBuffer<FrameData> frame;
// Cartes des cascades, en 2 x 2 dans une seule texture (cascade c : colonne c % 2, ligne c / 2).
[[vk::binding(3, 1)]] Sampler2DShadow shadowMap;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv; // (0, 0) en haut à gauche de l'image, comme en glTF
};

struct SkinnedVertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] uint4 joints;    // indices des os dans le modèle
    [[vk::location(4)]] float4 weights;  // somme 1
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

float4 Transform(float4 columns[4], float3 p)
{
    return columns[0] * p.x + columns[1] * p.y + columns[2] * p.z + columns[3];
}

// Position et normale du sommet dans le modèle, déplacées par ses os (moyenne pondérée de leurs matrices).
void Skin(SkinnedVertexInput input, uint firstJoint, out float3 position, out float3 normal)
{
    float4 column0 = float4(0.0);
    float4 column1 = float4(0.0);
    float4 column2 = float4(0.0);
    float4 column3 = float4(0.0);
    [unroll]
    for (int k = 0; k < 4; ++k) {
        const float weight = input.weights[k];
        const uint base = (firstJoint + input.joints[k]) * 4;
        column0 += jointColumns[base + 0] * weight;
        column1 += jointColumns[base + 1] * weight;
        column2 += jointColumns[base + 2] * weight;
        column3 += jointColumns[base + 3] * weight;
    }
    position = (column0 * input.position.x + column1 * input.position.y + column2 * input.position.z + column3).xyz;
    // Les os tournent sans étirer (en pratique) : leur 3x3 convient aux normales, renormalisées au pixel.
    normal = column0.xyz * input.normal.x + column1.xyz * input.normal.y + column2.xyz * input.normal.z;
}

VertexOutput Output(float3 position, float3 normal, float2 uv)
{
    const DrawData draw = draws[params.draw];
    VertexOutput output;
    output.worldPosition = Transform(draw.world, position).xyz;
    output.position = Transform(frame[0].viewProjection, output.worldPosition);
    output.normal = draw.normal[0].xyz * normal.x + draw.normal[1].xyz * normal.y + draw.normal[2].xyz * normal.z;
    output.uv = uv;
    return output;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    return Output(input.position, input.normal, input.uv);
}

[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[params.draw].normal[1].w), position, normal);
    return Output(position, normal, input.uv);
}

// Part de la lumière du soleil qui atteint ce point (0 : dans l'ombre, 1 : éclairé).
float SunVisibility(float3 worldPosition, float3 normal, float sunFacing)
{
    const FrameData f = frame[0];
    const float depth = dot(worldPosition, f.cameraForward.xyz);
    int cascade = 0;
    while (cascade < 4 && depth > f.cascadeEnds[cascade]) {
        ++cascade;
    }
    if (cascade >= 4) {
        return 1.0; // trop loin : pas d'ombre
    }
    // Décalé le long de la normale (plus sur une face rasante) : une surface ne se fait pas d'ombre à elle-même.
    const float texel = f.cascadeTexels[cascade];
    const float3 offsetPosition = worldPosition + normal * texel * (1.5 + 2.0 * (1.0 - sunFacing));
    float4 m[4] = {f.cascades[cascade * 4 + 0], f.cascades[cascade * 4 + 1], f.cascades[cascade * 4 + 2],
                   f.cascades[cascade * 4 + 3]};
    const float3 light = Transform(m, offsetPosition).xyz;
    const float2 local = light.xy * 0.5 + 0.5;
    if (any(local < 0.0) || any(local > 1.0) || light.z > 1.0) {
        return 1.0;
    }
    // Carte de la cascade dans l'atlas 2 x 2, sans déborder sur la voisine (le filtre lit un texel autour).
    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    const float2 atlasTexel = 1.0 / float2(width, height);
    const float2 cell = float2(cascade % 2, cascade / 2);
    const float2 inside = clamp(local * 0.5, atlasTexel * 1.5, 0.5 - atlasTexel * 1.5);
    const float2 uv = cell * 0.5 + inside;
    // 4 lectures filtrées (chacune compare 2 x 2 texels) : bord doux sur 3 texels environ.
    float lit = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const float2 offset = float2((i & 1) != 0 ? 0.5 : -0.5, (i & 2) != 0 ? 0.5 : -0.5) * atlasTexel;
        lit += shadowMap.SampleCmpLevelZero(uv + offset, light.z);
    }
    lit *= 0.25;
    // La dernière cascade s'efface sur son dernier dixième : pas de limite nette au loin.
    if (cascade == 3) {
        const float fade = saturate((f.cascadeEnds[3] - depth) / (0.1 * f.cascadeEnds[3]));
        lit = lerp(1.0, lit, fade);
    }
    return lit;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    float4 base = baseColorTexture.Sample(input.uv) * params.color;
    if (base.a < params.alphaCutoff) {
        discard;
    }
    // Face arrière d'un matériau double face : éclairée comme vue de son côté.
    float3 normal = normalize(input.normal) * (frontFacing ? 1.0 : -1.0);
    const float3 toSun = frame[0].toSun.xyz;
    const float sunFacing = dot(normal, toSun);
    const float sun = sunFacing > 0.0 ? sunFacing * SunVisibility(input.worldPosition, normal, sunFacing) : 0.0;
    const float shade = 0.3 + 0.1 * normal.y + 0.6 * sun;
    return float4(base.rgb * shade, params.transparent != 0 ? base.a : 1.0);
}

// Cartes d'ombres : la position vue du soleil (cascade params.cascade), et l'image seulement pour le seuil d'alpha.
struct ShadowOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

ShadowOutput ShadowVertex(float3 position, float2 uv)
{
    const FrameData f = frame[0];
    const uint c = params.cascade;
    float4 m[4] = {f.cascades[c * 4 + 0], f.cascades[c * 4 + 1], f.cascades[c * 4 + 2], f.cascades[c * 4 + 3]};
    ShadowOutput output;
    output.position = Transform(m, Transform(draws[params.draw].world, position).xyz);
    output.uv = uv;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadow(VertexInput input)
{
    return ShadowVertex(input.position, input.uv);
}

[shader("vertex")]
ShadowOutput vertexShadowSkinned(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[params.draw].normal[1].w), position, normal);
    return ShadowVertex(position, input.uv);
}

[shader("fragment")]
void fragmentShadow(ShadowOutput input)
{
    if (params.alphaCutoff > 0.0 && baseColorTexture.Sample(input.uv).a * params.color.a < params.alphaCutoff) {
        discard;
    }
}
