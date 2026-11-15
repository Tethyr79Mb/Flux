// Éclairage différé des modèles opaques (LightingRenderer), en compute, entre la passe « G-buffer » et celle de la
// scène. Chaque pixel où un modèle a été écrit (GBufferBase.a non nul) est éclairé par le même code que le rendu direct
// (LightSurface de Mesh.slang) : sa position retrouvée depuis la profondeur (reverse-Z infini, caméra à l'origine), sa
// surface lue dans le G-buffer. La lumière propre (émissif et contour), écrite par la passe « G-buffer » dans l'image
// de la scène, y est reprise : elle traverse l'air comme le reste. Avec le ray tracing (LightingRays.slang), la lumière
// directe du soleil est écrite à part, comme le font les programmes de pixel (SceneResult), pour RayShadows.slang.
// Fichier à part de Mesh.slang : le groupe 3 y est déjà pris par les pages de géométrie des modules qui l'incluent
// (ModelMesh, PlanetMesh, Grass, Clouds).

#include "Mesh.slang"

// Groupe 3 : textures du graphe de rendu, à la taille de rendu (LightingRenderer::Bind).
[[vk::binding(0, 3)]] Sampler2D lightingDepth;
[[vk::binding(1, 3)]] Sampler2D gbufferBase;    // rgb : couleur de base ; a : sorte de matière (GBufferStandard, 0 : rien)
[[vk::binding(2, 3)]] Sampler2D gbufferNormals; // xy : normale de la carte ; zw : de la face (octaédriques, EncodeOctahedron)
[[vk::binding(3, 3)]] Sampler2D gbufferSurface; // métal, rugosité, occlusion
[[vk::binding(4, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> lightingColor; // émissif et contour lus, puis tout
#ifdef FLUX_RAY_QUERY
[[vk::binding(5, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> lightingSun;
#endif

static const uint LightingGroupSize = 8; // LightingRenderer.cpp

// Vue de débogage (params.cascade, GBufferView) : une donnée du G-buffer telle quelle, à la place de la lumière.
float3 GBufferDebug(uint view, float3 base, float3 normal, float4 surface)
{
    if (view == 1) {
        return base;
    }
    if (view == 2) {
        return normal * 0.5 + 0.5;
    }
    return surface.xyz;
}

[shader("compute")]
[numthreads(LightingGroupSize, LightingGroupSize, 1)]
void computeLighting(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    lightingColor.GetDimensions(size.x, size.y);
    if (any(id.xy >= size)) {
        return;
    }
    const int3 texel = int3(int2(id.xy), 0);
    const float4 base = gbufferBase.Load(texel);
    if (base.a == 0.0) {
        return; // pas un modèle opaque : fond effacé, ou sol, arbres et ciel dessinés ensuite
    }
    const FrameData f = frame[0];
    // Rayon du pixel sans le décalage d'un sous-pixel (FrameData::cameraRight), à la distance donnée par la profondeur.
    const float depth = lightingDepth.Load(texel).r;
    const float2 clip = (float2(id.xy) + 0.5) / float2(size) * 2.0 - 1.0 - float2(f.cameraRight.w, f.cameraUp.w);
    const float3 worldPosition = (f.cameraForward.xyz + f.cameraRight.xyz * clip.x - f.cameraUp.xyz * clip.y) * (params.color.x / depth);
    const float4 normals = gbufferNormals.Load(texel);
    const float3 normal = DecodeOctahedron(normals.xy);
    const float3 geometryNormal = DecodeOctahedron(normals.zw);
    const float4 material = gbufferSurface.Load(texel);
    if (params.cascade != 0) {
        lightingColor[id.xy] = float4(GBufferDebug(params.cascade, base.rgb, normal, material), 1.0);
#ifdef FLUX_RAY_QUERY
        lightingSun[id.xy] = float4(0.0);
#endif
        return;
    }
    shadowPixel = float2(id.xy) + 0.5;
    const float metallic = material.x;
    Surface surface;
    surface.normal = normal;
    surface.toEye = normalize(-worldPosition);
    surface.diffuse = base.rgb * (1.0 - metallic);
    surface.f0 = lerp(float3(0.04), base.rgb, metallic);
    surface.roughness = material.y;
    float inside;
    const float3 lit = LightSurface(surface, geometryNormal, worldPosition, material.z, CloudShadow(worldPosition), inside);
    float3 transmittance;
    const float3 scattered = AirTo(worldPosition, transmittance);
    // Lumière propre déjà multipliée par l'exposition (Radiance) : seulement voilée par l'air ici.
    const float3 unlit = lightingColor[id.xy].rgb;
    lightingColor[id.xy] = float4(min(unlit * transmittance + Radiance(lit * transmittance + scattered), float3(60000.0)), 1.0);
#ifdef FLUX_RAY_QUERY
    lightingSun[id.xy] = float4(Radiance(sunDirect * transmittance), 0.0);
#endif
}
