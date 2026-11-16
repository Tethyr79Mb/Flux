// Relit une texture des vagues de l'océan (Tests/src/OceanWavesGpuTests.cpp) : output[i] = pixel i du niveau `level`,
// pris dans l'ordre x, puis y.

struct Constants
{
    uint size; // côté du niveau lu
    uint level;
};

[[vk::binding(0, 0)]] Sampler2D<float4> image;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> output;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.size * constants.size) {
        return;
    }
    output[id.x] = image.Load(int3(int(id.x % constants.size), int(id.x / constants.size), int(constants.level)));
}
