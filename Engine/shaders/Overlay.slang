// Overlay 2D (FluxUI) : rectangles et texte MSDF, puis images.
// Les rectangles échantillonnent un bloc plein de l'atlas, donc le même mode sert aux deux ;
// une image est une texture ordinaire (sRGB, décodée en linéaire par le GPU), multipliée par la couleur du sommet.

struct OverlayParams
{
    float2 screenSize;    // pixels
    float2 atlasSize;     // pixels
    float distanceRange;  // étendue du champ de distance, en pixels d'atlas
    float mode;           // 0 : atlas MSDF (texte, rectangles) ; 1 : image
};

[[vk::push_constant]] ConstantBuffer<OverlayParams> params;
[[vk::binding(0, 0)]] Sampler2D atlas; // l'atlas de la police, ou l'image selon le mode

// Doit correspondre à UI::Vertex (Flux/UI/DrawList.hpp).
struct VertexInput
{
    float2 position : POSITION;  // location 0, pixels, origine en haut à gauche
    float2 uv       : TEXCOORD0; // location 1
    float4 color    : COLOR;     // location 2, linéaire
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    // Pixels -> NDC ; en Vulkan y vers le bas, donc pas d'inversion.
    output.position = float4(input.position / params.screenSize * 2.0 - 1.0, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float median(float3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    if (params.mode > 0.5)
    {
        return atlas.Sample(input.uv) * input.color;
    }

    const float3 msdf = atlas.Sample(input.uv).rgb;
    const float signedDistance = median(msdf);

    // Nombre de pixels écran couverts par l'étendue du champ : règle la largeur de l'anticrénelage.
    const float2 unitRange = params.distanceRange / params.atlasSize;
    const float2 screenTexSize = 1.0 / max(fwidth(input.uv), 1e-6);
    const float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    const float alpha = saturate(screenPxRange * (signedDistance - 0.5) + 0.5);
    return float4(input.color.rgb, input.color.a * alpha);
}
