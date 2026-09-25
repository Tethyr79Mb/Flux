// Maillages opaques : position transformée par une matrice calculée sur le CPU, relative à la caméra.

struct MeshParams
{
    // Matrice modèle-vue-projection par colonnes : évite toute ambiguïté de convention ligne/colonne.
    float4 mvpColumn0;
    float4 mvpColumn1;
    float4 mvpColumn2;
    float4 mvpColumn3;
    float4 color; // linéaire
};

[[vk::push_constant]] ConstantBuffer<MeshParams> params;

struct VertexInput
{
    float3 position : POSITION; // location 0
    float shade     : COLOR;    // location 1, éclairage fixe par face
};

struct VertexOutput
{
    float4 position : SV_Position;
    float shade     : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = params.mvpColumn0 * input.position.x
                    + params.mvpColumn1 * input.position.y
                    + params.mvpColumn2 * input.position.z
                    + params.mvpColumn3;
    output.shade = input.shade;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(params.color.rgb * input.shade, params.color.a);
}
