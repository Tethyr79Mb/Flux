// Relit les textures de l'environnement (Tests/src/EnvironmentGpuTests.cpp) : output[i] = la texture cube vue dans la
// direction i (+X, -X, +Y, -Y, +Z, -Z), au niveau de mip `level`.

struct Constants
{
    float level;
};

[[vk::binding(0, 0)]] SamplerCube<float4> cube;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> output;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[shader("compute")]
[numthreads(8, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 6) {
        return;
    }
    float3 direction = float3(0.0);
    direction[id.x / 2] = (id.x & 1) == 0 ? 1.0 : -1.0;
    output[id.x] = cube.SampleLevel(direction, constants.level);
}
