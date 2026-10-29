// Relecture d'une texture envoyée au GPU (Tests/src/TransferTests.cpp) : texels[y * width + x] = niveau `level`.

struct Constants
{
    uint width;
    uint level;
};

[[vk::binding(0, 0)]] Sampler2D image;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> texels;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeReadTexture(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.width || id.y >= constants.width) { return; }
    texels[id.y * constants.width + id.x] = image.Load(int3(id.xy, constants.level));
}
