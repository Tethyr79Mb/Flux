// Éclairage différé (LightingRenderer), en compute, entre la passe « G-buffer » et celle de la scène : modèles opaques,
// arbres (de près et imposteurs), herbe et sol des planètes. Chaque pixel écrit (GBufferBase.a, la sorte de matière,
// non nulle) est éclairé par le même code que le rendu direct (LightSurface de Mesh.slang) : sa position retrouvée
// depuis la profondeur (reverse-Z infini, caméra à l'origine), sa surface lue dans le G-buffer. L'air, l'ombre des
// nuages et les volumes intérieurs y sont calculés une fois par pixel visible. La lumière propre (émissif, contour),
// écrite par la passe « G-buffer » dans l'image de la scène, y est reprise : elle traverse l'air comme le reste. Avec
// le ray tracing (LightingRays.slang), la lumière directe du soleil est écrite à part, comme le font les programmes de
// pixel (SceneResult), pour RayShadows.slang.
// Fichier à part de Mesh.slang : le groupe 3 y est déjà pris par les pages de géométrie des modules qui l'incluent
// (ModelMesh, PlanetMesh, Grass, Clouds).

#include "Mesh.slang"

// Groupe 3 : textures du graphe de rendu, à la taille de rendu (LightingRenderer::Bind).
[[vk::binding(0, 3)]] Sampler2D lightingDepth;
[[vk::binding(1, 3)]] Sampler2D gbufferBase;    // rgb : couleur de base ; a : sorte de matière (GBufferStandard, 0 : rien)
[[vk::binding(2, 3)]] Sampler2D gbufferNormals; // xy : normale de la carte ; zw : de la face (octaédriques, EncodeOctahedron)
[[vk::binding(3, 3)]] Sampler2D gbufferSurface; // selon la sorte de matière (GBufferStandard…, Mesh.slang)
[[vk::binding(4, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> lightingColor; // émissif et contour lus, puis tout
// Occlusion ambiante (AmbientOcclusion.slang) : x, part de la lumière ambiante qui arrive ; y, distance devant la caméra.
// Lue si params.color.y (pas entre deux pixels calculés : 1 ou 2) n'est pas nul.
[[vk::binding(6, 3)]] Sampler2D ambientOcclusion;
#ifdef FLUX_RAY_QUERY
[[vk::binding(5, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> lightingSun;
#endif

static const uint LightingGroupSize = 8; // LightingRenderer.cpp

// Occlusion ambiante de l'écran au pixel (1 sans elle). Demi-résolution : les quatre pixels calculés voisins, pondérés
// comme un filtrage bilinéaire et par l'écart de distance (le fond derrière un poteau ne l'assombrit pas).
float ScreenOcclusion(int2 pixel, float viewDepth)
{
    const uint stride = uint(params.color.y);
    if (stride == 0u) {
        return 1.0;
    }
    if (stride == 1u) {
        return ambientOcclusion.Load(int3(pixel, 0)).x;
    }
    uint2 size;
    ambientOcclusion.GetDimensions(size.x, size.y);
    const int2 last = int2((size + stride - 1u) / stride) - 1;
    // Le pixel calculé i est le pixel 2i de la taille de rendu.
    const float2 position = float2(pixel) / float(stride);
    const int2 corner = int2(floor(position));
    const float2 f = position - float2(corner);
    float sum = 0.0;
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        const int2 offset = int2(i & 1, i >> 1);
        const float2 tap = ambientOcclusion.Load(int3(clamp(corner + offset, int2(0), last), 0)).xy;
        const float2 axis = lerp(1.0 - f, f, float2(offset));
        const float bilinear = axis.x * axis.y;
        const float weight = bilinear * (saturate(1.0 - abs(tap.y - viewDepth) / (0.05 * viewDepth)) + 1.0e-3);
        sum += tap.x * weight;
        total += weight;
    }
    return total > 0.0 ? sum / total : 1.0;
}

// Les surfaces claires renvoient entre elles une partie de la lumière que l'occlusion retire : un coin blanc s'assombrit
// moins qu'un coin noir (Jimenez et al. 2016, ajusté sur des rendus de référence ; albédo en luminance).
float MultiBounce(float visibility, float albedo)
{
    const float a = 2.0404 * albedo - 0.3324;
    const float b = -4.7951 * albedo + 0.6417;
    const float c = 2.7552 * albedo + 0.6903;
    return saturate(max(visibility, ((visibility * a + b) * visibility + c) * visibility));
}

// Part des reflets de l'environnement gardée sous une occlusion `visibility` (Lagarde et de Rousiers 2014) : un reflet
// lisse vu de face passe encore, un reflet rugueux s'éteint comme le diffus.
float SpecularOcclusion(float nv, float visibility, float roughness)
{
    return saturate(pow(nv + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility);
}

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

// Part du soleil gardée côté à contre-jour d'un imposteur, malgré son ombre sur lui-même (computeLighting).
static const float ImpostorBacklitLeak = 0.5;

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
        return; // rien à éclairer : fond (ciel et océan dessinés ensuite), fond de mer caché, mesure « sans pixels »
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
    const float screenOcclusion = ScreenOcclusion(int2(id.xy), params.color.x / depth);
    if (params.cascade != 0) {
        // Vue « occlusion ambiante » (GBufferView::AmbientOcclusion) : l'occlusion de l'écran seule, en gris.
        const float3 shown = params.cascade == 4 ? float3(screenOcclusion) : GBufferDebug(params.cascade, base.rgb, normal, material);
        lightingColor[id.xy] = float4(shown, 1.0);
#ifdef FLUX_RAY_QUERY
        lightingSun[id.xy] = float4(0.0);
#endif
        return;
    }
    shadowPixel = float2(id.xy) + 0.5;
    // Sorte de matière (GBufferStandard et suivantes, Mesh.slang) : ce que contient `material`.
    const uint kind = uint(base.a * 255.0 + 0.5);
    Surface surface;
    surface.normal = normal;
    surface.toEye = normalize(-worldPosition);
    float clouds = CloudShadow(worldPosition);
    bool foliage = false;
    float shadowLift = 0.0;
    float occlusion = material.z;
    if (kind == 1) { // GBufferStandard : métal, rugosité, occlusion, reflet des non-métaux
        const float metallic = material.x;
        surface.diffuse = base.rgb * (1.0 - metallic);
        surface.f0 = lerp(float3(material.w * GBufferSpecular), base.rgb, metallic);
        surface.roughness = material.y;
    } else { // feuillage, brins, imposteurs : leaves en x et w (ShadeFoliage)
        foliage = true;
        surface.diffuse = base.rgb;
        surface.f0 = float3(0.04);
        surface.roughness = material.y;
        simpleShadow = kind == 3; // GBufferGrass
        if (kind == 4) { // GBufferImpostor
            // Le carré fait face à la caméra, son ombre (vertexImpostorShadow) face au soleil : lue sur le carré, la
            // moitié de l'arbre derrière ce second carré était dans sa propre ombre, et les forêts au loin bien plus
            // sombres que de près. Lue à 1,2 rayon vers le soleil, devant son ombre ; celle des voisins reste.
            // L'ombre de l'arbre sur lui-même, lue dans ses vues capturées (ImpostorSunVisibility), retire le soleil
            // comme les cascades le font pour le modèle détaillé. Le ciel n'est pas éteint : le détaillé ne l'est pas non
            // plus (pas d'occlusion de son propre feuillage).
            surface.roughness = 0.8;
            shadowLift = 1.2 * material.y * ImpostorRadiusScale;
            // Côté à contre-jour, la lumière traverse pourtant les aiguilles : le détaillé la garde (ses cascades, à
            // cette distance, ne l'éteignent pas tout à fait). Mesuré le 2026-11-17 sur les mêmes conifères à 150-300 m :
            // sans cette part, l'imposteur était 15 % plus sombre à contre-jour, 11 % avec 0,25, 6 % avec 0,5 ; de côté,
            // aucun changement.
            const float backlit = saturate(-4.0 * dot(geometryNormal, f.toSun.xyz));
            clouds *= lerp(material.z, 1.0, ImpostorBacklitLeak * backlit);
            occlusion = 1.0;
        }
    }
    // Occlusion de l'écran mêlée à celle de la matière (la plus forte des deux), pour la seule lumière ambiante : le
    // soleil et les lampes ont leurs ombres. Reflets à part, selon l'angle de vue et la rugosité. Sans occlusion de
    // l'écran (1), l'image est celle d'avant.
    const float screen = MultiBounce(screenOcclusion, dot(surface.diffuse, float3(0.2126, 0.7152, 0.0722)));
    const float visibility = min(occlusion, screen);
    const float specularOcclusion = min(occlusion, SpecularOcclusion(saturate(dot(surface.normal, surface.toEye)), screen,
                                                                     surface.roughness));
    float inside;
    const float3 lit = LightSurface(surface, geometryNormal, worldPosition, visibility, clouds, inside, foliage, material.xw,
                                    shadowLift, specularOcclusion);
    float3 transmittance;
    const float3 scattered = AirTo(worldPosition, transmittance);
    // Lumière propre déjà multipliée par l'exposition (Radiance) : seulement voilée par l'air ici.
    const float3 unlit = lightingColor[id.xy].rgb;
    lightingColor[id.xy] = float4(min(unlit * transmittance + Radiance(lit * transmittance + scattered), float3(60000.0)), 1.0);
#ifdef FLUX_RAY_QUERY
    lightingSun[id.xy] = float4(Radiance(sunDirect * transmittance), 0.0);
#endif
}
