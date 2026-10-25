// Relit le bruit des nuages (Tests/src/CloudNoiseTests.cpp) : output[i] = pixel i du niveau `level`, pris dans l'ordre
// x, puis y, puis z.

struct Constants
{
    uint size; // côté du niveau lu
    uint level;
};

[[vk::binding(0, 0)]] Sampler3D<float4> volume;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> output;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    const uint count = constants.size * constants.size * constants.size;
    if (id.x >= count) {
        return;
    }
    const uint3 texel = uint3(id.x % constants.size, (id.x / constants.size) % constants.size, id.x / (constants.size * constants.size));
    output[id.x] = volume.Load(int4(int3(texel), int(constants.level)));
}
