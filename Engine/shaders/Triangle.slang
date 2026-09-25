// Premier shader : un triangle coloré, sommets lus depuis un vertex buffer.

struct VertexInput
{
    float2 position : POSITION; // location 0
    float3 color    : COLOR;    // location 1
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
