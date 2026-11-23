// Litière au pied des arbres de près des planètes (LitterRenderer), en décalque projeté : entre la passe « G-buffer »
// et l'éclairage, une boîte par arbre de la liste Near (instances de VegetationRenderer, comme vertexTree) ; chacun de
// ses pixels retrouve le point de la scène depuis la profondeur et, s'il est dans la boîte, au pied de l'arbre, sur une
// surface tournée vers le haut et pas un brin d'herbe, y peint la couleur de base du G-buffer. Elle épouse ainsi la
// pente et les bosses du sol. L'alpha du G-buffer (sorte de matière) n'est pas touché (RHI::BlendMode::Decal).
// Fichier à part de Mesh.slang, comme Lighting.slang : le groupe 3 y est pris par les pages de géométrie.

#include "Mesh.slang"

// Groupe 3 : textures du graphe de rendu, à la taille de rendu (LitterRenderer::Bind).
[[vk::binding(0, 3)]] Sampler2D litterDepth;
[[vk::binding(1, 3)]] Sampler2D litterNormals; // xy : normale de la carte ; zw : de la face (EncodeOctahedron)
[[vk::binding(2, 3)]] Sampler2D litterSurface; // selon la sorte de matière : x nul pour le sol, pas pour l'herbe

// Côté (m) de l'image de la litière, répétée sans raccord (Modeles/Coniferes/conifere.py, LITTER_TILE).
static const float LitterTile = 1.5;

struct LitterVertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint instance : INSTANCE;
};

// Demi-hauteur de la boîte (à la taille du modèle) pour un rayon donné : le sol peut monter ou descendre de 1,1 rayon
// au bord sur la plus forte pente gardée (TreeGround), plus ses bosses.
float LitterHalfHeight(float radius)
{
    return 1.2 * radius + 1.0;
}

// Coin `corner` (bits 0, 1, 2 : x, y, z ; LitterRenderer::m_indices) de la boîte de l'arbre.
[shader("vertex")]
LitterVertexOutput vertexLitter(uint corner : SV_VertexID, uint instance : SV_VulkanInstanceID)
{
    const float4 placement = treeInstances[instance * 2];
    float4 rotation = treeInstances[instance * 2 + 1];
    rotation.w = sqrt(saturate(1.0 - dot(rotation.xyz, rotation.xyz))); // w porte teinte et pente (PackTree)
    const float radius = params.color.y;
    const float3 side = float3(float(corner & 1u), float((corner >> 1) & 1u), float((corner >> 2) & 1u)) * 2.0 - 1.0;
    const float3 local = side * float3(radius, LitterHalfHeight(radius), radius);
    LitterVertexOutput output;
    output.position = Transform(frame[0].viewProjection, placement.xyz + TreeRotate(rotation, local * placement.w));
    output.instance = instance;
    return output;
}

[shader("fragment")]
float4 fragmentLitter(LitterVertexOutput input) : SV_Target0
{
    const int3 texel = int3(int2(input.position.xy), 0);
    const float depth = litterDepth.Load(texel).r;
    const FrameData f = frame[0];
    // Point de la scène au pixel, comme computeLighting (Lighting.slang).
    uint2 size;
    litterDepth.GetDimensions(size.x, size.y);
    const float2 clip = (floor(input.position.xy) + 0.5) / float2(size) * 2.0 - 1.0 - float2(f.cameraRight.w, f.cameraUp.w);
    const float3 worldPosition = (f.cameraForward.xyz + f.cameraRight.xyz * clip.x - f.cameraUp.xyz * clip.y)
                               * (params.color.x / max(depth, 1.0e-12));

    // Dans le repère de l'arbre (à la taille du modèle) : y vers son haut, x et z tournés de son angle.
    const float4 placement = treeInstances[input.instance * 2];
    float4 rotation = treeInstances[input.instance * 2 + 1];
    const float2 ground = TreeGround(rotation.w);
    rotation.w = sqrt(saturate(1.0 - dot(rotation.xyz, rotation.xyz)));
    const float3 local = TreeRotate(float4(-rotation.xyz, rotation.w), worldPosition - placement.xyz) / placement.w;
    const float radius = params.color.y;
    const float2 flat = local.xz / radius;

    // Bord irrégulier, propre à chaque arbre (sa taille, tirée au hasard, donne la phase) : la litière s'éclaircit
    // vers le bord au lieu de finir en disque.
    const float seed = frac(placement.w * 53.7) * 6.2831853;
    const float angle = atan2(flat.y, flat.x);
    const float edge = 0.78 + 0.12 * sin(3.0 * angle + seed) + 0.07 * sin(5.0 * angle + 2.1 * seed)
                     + 0.04 * sin(11.0 * angle + 3.7 * seed);
    float alpha = 1.0 - smoothstep(edge - 0.35, edge, length(flat));
    // Près du plan du sol au pied de l'arbre (celui que suit le bas du modèle, TreePosition) : pas sur une branche
    // basse ni sur un rocher plus haut.
    const float groundY = sqrt(max(1.0 - dot(ground, ground), 0.25));
    const float height = abs(local.y + dot(ground, local.xz) / groundY);
    const float halfHeight = LitterHalfHeight(radius);
    alpha *= 1.0 - smoothstep(0.6 * halfHeight, halfHeight, height);
    // Surface tournée vers le haut de l'arbre (pas son tronc, pas un flanc de rocher), et pas un brin d'herbe ni une
    // feuille (surface.x non nul : GBufferGrass, GBufferFoliage).
    const float3 geometryNormal = DecodeOctahedron(litterNormals.Load(texel).zw);
    alpha *= smoothstep(0.55, 0.8, dot(geometryNormal, TreeRotate(rotation, float3(0.0, 1.0, 0.0))));
    alpha *= litterSurface.Load(texel).x < 0.01 ? 1.0 : 0.0;
    // Effacée avant la fin de la liste Near, où l'arbre passe au modèle simplifié, sans litière.
    alpha *= 1.0 - saturate((length(worldPosition) - params.color.z) * params.color.w);
    // Rien derrière (fond) : profondeur nulle.
    alpha *= depth > 0.0 ? 1.0 : 0.0;

    // Image en mètres, décalée pour chaque arbre ; lue avant de décider (dérivées des pixels voisins).
    const MaterialData material = materials[params.material];
    const float2 uv = local.xz * placement.w / LitterTile + float2(seed, seed * 0.37);
    const float4 litter = SampleMaterial(material.baseColorImage, uv) * material.color;
    return float4(litter.rgb, saturate(alpha * litter.a));
}
