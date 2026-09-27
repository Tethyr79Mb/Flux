// Maillages opaques (cubes et modèles glTF) : position transformée par une matrice calculée sur le CPU, relative
// à la caméra ; couleur de base (facteur x image) éclairée par un soleil fixe et une lumière ambiante.
// Modèles animés (vertexSkinned) : chaque sommet est d'abord déplacé par ses os (skinning).

struct MeshParams
{
    // Matrice modèle-vue-projection par colonnes : évite toute ambiguïté de convention ligne/colonne.
    float4 mvpColumn0;
    float4 mvpColumn1;
    float4 mvpColumn2;
    float4 mvpColumn3;
    // Matrice des normales (repère du monde), par colonnes dans xyz. normalColumn0.w : seuil d'alpha (0 : opaque).
    // normalColumn1.w : premier os du modèle dans jointColumns (bits d'un entier), modèles animés seulement.
    float4 normalColumn0;
    float4 normalColumn1;
    float4 normalColumn2;
    float4 color; // linéaire : couleur de base du matériau x teinte de l'entité
};

[[vk::push_constant]] ConstantBuffer<MeshParams> params;

// Couleur de base du matériau (blanc 1 x 1 sans image), en sRGB : le GPU la ramène en linéaire à la lecture.
[[vk::binding(0, 0)]] Sampler2D baseColorTexture;

struct VertexInput
{
    float3 position : POSITION; // location 0
    float3 normal   : NORMAL;   // location 1
    float2 uv       : TEXCOORD; // location 2
};

// Matrices des os de tous les modèles animés de la frame (monde du modèle x inverse de liaison), 4 colonnes par os.
[[vk::binding(0, 1)]] StructuredBuffer<float4> jointColumns;

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
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = params.mvpColumn0 * input.position.x
                    + params.mvpColumn1 * input.position.y
                    + params.mvpColumn2 * input.position.z
                    + params.mvpColumn3;
    output.normal = params.normalColumn0.xyz * input.normal.x
                  + params.normalColumn1.xyz * input.normal.y
                  + params.normalColumn2.xyz * input.normal.z;
    output.uv = input.uv;
    return output;
}

[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input)
{
    // Moyenne des matrices des os, pondérée : une seule matrice appliquée ensuite à la position et à la normale.
    const uint firstJoint = asuint(params.normalColumn1.w);
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
    const float3 position = (column0 * input.position.x + column1 * input.position.y + column2 * input.position.z
                             + column3).xyz;
    // Les os tournent sans étirer (en pratique) : leur 3x3 convient aux normales, renormalisées au pixel.
    const float3 normal = column0.xyz * input.normal.x + column1.xyz * input.normal.y + column2.xyz * input.normal.z;

    VertexOutput output;
    output.position = params.mvpColumn0 * position.x
                    + params.mvpColumn1 * position.y
                    + params.mvpColumn2 * position.z
                    + params.mvpColumn3;
    output.normal = params.normalColumn0.xyz * normal.x
                  + params.normalColumn1.xyz * normal.y
                  + params.normalColumn2.xyz * normal.z;
    output.uv = input.uv;
    return output;
}

// Soleil fixe, vers le haut et un peu de côté : les faces du dessus claires, celles du dessous sombres.
static const float3 SunDirection = float3(0.36, 0.89, 0.27);

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    float4 base = baseColorTexture.Sample(input.uv) * params.color;
    const float alphaCutoff = params.normalColumn0.w;
    if (base.a < alphaCutoff) {
        discard;
    }
    // Face arrière d'un matériau double face : éclairée comme vue de son côté.
    float3 normal = normalize(input.normal) * (frontFacing ? 1.0 : -1.0);
    const float sun = max(dot(normal, SunDirection), 0.0);
    const float shade = 0.3 + 0.1 * normal.y + 0.6 * sun;
    return float4(base.rgb * shade, 1.0);
}
