// Calcul de test du RHI (Tests/src/ComputeTests.cpp) : output[i] = input[i] * scale + i.

struct Constants
{
    uint count;
    float scale;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> input;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> output;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.count) { return; }
    output[id.x] = input[id.x] * constants.scale + float(id.x);
}

// Deuxième passe, après ComputeBarrier : relit ce que la première a écrit.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeDouble(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.count) { return; }
    output[id.x] = output[id.x] * 2.0;
}
