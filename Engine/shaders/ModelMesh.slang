// Modèles fixes en mesh shaders (VK_EXT_mesh_shader), à la place de vertexMain quand le GPU les a : même image, mais
// le GPU écarte lui-même les paquets de triangles (ModelMeshlet, 124 triangles au plus) qu'il ne verra pas. Une tâche
// (taskModel) teste jusqu'à 32 paquets d'une primitive d'une entité : hors du champ, tournés vers l'arrière (cône des
// normales), ou cachés derrière ce que voyait l'image précédente (MeshCulling.slang) ; meshModel dessine chacun des
// restants avec les mêmes sommets que vertexMain (MovingOutput) et les mêmes programmes de pixel.
// Limite : ce qu'une porte ou un mur découvre d'une image à l'autre apparaît une image plus tard, comme pour les objets
// (OcclusionCulling).
// Compilé à part de Mesh.slang (qu'il inclut) : un module qui déclare les mesh shaders ne se charge pas sur un GPU qui
// ne les a pas.

#include "MeshCulling.slang"

static const uint TaskMeshlets = 32;   // paquets testés par une tâche (MeshRenderer::MeshTaskMeshlets)
static const uint MaxVertices = 64;    // ModelMeshletMaxVertices
static const uint MaxTriangles = 124;  // ModelMeshletMaxTriangles
static const uint MeshGroupSize = 64;

// Doit correspondre à GpuMeshlet dans ModelCache.cpp.
struct Meshlet
{
    float4 sphere;      // centre dans le repère du modèle ; rayon
    float4 cone;        // axe des normales ; coupure (au-dessus de 1 : jamais de dos)
    uint firstVertex;   // dans meshletData : indices des sommets dans la page
    uint firstTriangle; // dans meshletData : trois octets par triangle, quatre par mot
    uint vertexCount;
    uint triangleCount;
};

// Doit correspondre à MeshRenderer::MeshTask : jusqu'à TaskMeshlets paquets d'une primitive d'une entité.
struct MeshTask
{
    uint draw;         // DrawData de l'entité
    uint material;     // GpuMaterial::index
    uint firstMeshlet; // dans pageMeshlets
    uint countAndFlags; // bits 0-7 : paquets ; bit 8 : test des faces de dos permis (échelle uniforme et positive)
};

[[vk::binding(12, 1)]] StructuredBuffer<MeshTask> meshTasks; // de l'image ; params.draw : première du lot
// Paquets dessinés, ajoutés à chaque image (jamais remis à zéro : le CPU compare deux lectures).
[[vk::binding(13, 1)]] RWStructuredBuffer<uint> meshletStats;
// Page de géométrie (ModelCache::GetPage) : ModelVertex (8 floats), paquets, et leurs sommets et triangles.
[[vk::binding(0, 3)]] StructuredBuffer<float> pageVertices;
[[vk::binding(1, 3)]] StructuredBuffer<Meshlet> pageMeshlets;
[[vk::binding(2, 3)]] StructuredBuffer<uint> meshletData;

struct ModelPayload
{
    uint draw;
    uint material;
    uint meshlets[TaskMeshlets];
};

groupshared ModelPayload payload;
groupshared uint visibleCount;

// Le paquet tourne entièrement le dos à la caméra (à l'origine) : toutes ses normales s'éloignent d'elle (cône de
// meshoptimizer, avec la sphère du paquet au lieu de la pointe du cône).
bool BackFacing(float3 center, float radius, float3 axis, float cutoff)
{
    return dot(center, axis) >= cutoff * length(center) + radius;
}

[shader("amplification")]
[numthreads(TaskMeshlets, 1, 1)]
void taskModel(uint thread : SV_GroupIndex, uint3 group : SV_GroupID)
{
    const MeshTask task = meshTasks[params.draw + group.x];
    if (thread == 0) {
        visibleCount = 0;
        payload.draw = task.draw;
        payload.material = task.material;
    }
    GroupMemoryBarrierWithGroupSync();

    bool visible = thread < (task.countAndFlags & 0xffu);
    if (visible) {
        const Meshlet meshlet = pageMeshlets[task.firstMeshlet + thread];
        const DrawData draw = draws[task.draw];
        // Rayon à la plus grande échelle de l'entité.
        const float scale = max(length(draw.world[0].xyz), max(length(draw.world[1].xyz), length(draw.world[2].xyz)));
        const float radius = meshlet.sphere.w * scale;
        const float3 center = Transform(draw.world, meshlet.sphere.xyz).xyz;
        visible = InView(center, radius) && !Occluded(Transform(draw.previousWorld, meshlet.sphere.xyz).xyz, radius);
        if (visible && (task.countAndFlags & 0x100u) != 0 && meshlet.cone.w <= 1.0) {
            const float3 axis = normalize(draw.world[0].xyz * meshlet.cone.x + draw.world[1].xyz * meshlet.cone.y
                                          + draw.world[2].xyz * meshlet.cone.z);
            visible = !BackFacing(center, radius, axis, meshlet.cone.w);
        }
    }
    if (visible) {
        uint slot;
        InterlockedAdd(visibleCount, 1u, slot);
        payload.meshlets[slot] = task.firstMeshlet + thread;
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0 && visibleCount > 0) {
        InterlockedAdd(meshletStats[0], visibleCount);
    }
    DispatchMesh(visibleCount, 1, 1, payload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MeshGroupSize, 1, 1)]
void meshModel(uint thread : SV_GroupIndex, uint3 group : SV_GroupID, in payload ModelPayload task,
               OutputVertices<VertexOutput, MaxVertices> vertices, OutputIndices<uint3, MaxTriangles> triangles)
{
    const Meshlet meshlet = pageMeshlets[task.meshlets[group.x]];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);
    if (thread < meshlet.vertexCount) {
        const uint v = meshletData[meshlet.firstVertex + thread] * 8;
        const float3 position = float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
        const float3 normal = float3(pageVertices[v + 3], pageVertices[v + 4], pageVertices[v + 5]);
        const float2 uv = float2(pageVertices[v + 6], pageVertices[v + 7]);
        VertexOutput output = MovingOutput(task.draw, position, normal, uv);
        output.material = task.material;
        vertices[thread] = output;
    }
    for (uint t = thread; t < meshlet.triangleCount; t += MeshGroupSize) {
        uint3 corners;
        [unroll]
        for (uint c = 0; c < 3; ++c) {
            const uint byte = t * 3 + c;
            corners[c] = (meshletData[meshlet.firstTriangle + byte / 4] >> ((byte % 4) * 8)) & 0xffu;
        }
        triangles[t] = corners;
    }
}
