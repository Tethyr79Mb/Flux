// Maillages (cubes et modèles glTF), opaques ou transparents : position relative à la caméra, matériaux PBR comme
// glTF (couleur de base, métal, rugosité, carte de normales, occlusion) éclairés par un soleil avec ses ombres
// (cascades), la lumière ambiante et les reflets du ciel (texture cube) et les lumières de la scène (ponctuelles et
// spots, sans ombre) ; plus la lumière propre du matériau (émissif). Puis exposition et courbe de tons (ACES) : la sortie est
// déjà la couleur affichée (en linéaire, la swapchain sRGB fait le reste).
// Modèles animés (vertexSkinned) : chaque sommet est d'abord déplacé par ses os (skinning).
// Cartes d'ombres (vertexShadow, vertexShadowSkinned, fragmentShadow) : profondeur vue du soleil, rien d'autre.

// Doit correspondre à InteriorData dans MeshRenderer.cpp : un volume intérieur (boîte), où le ciel n'éclaire pas.
struct InteriorData
{
    float4 axes[3];           // local[i] = dot(axes[i].xyz, position relative à la caméra) + axes[i].w
    float4 halfSize;          // xyz : demi-taille (m) ; w : fondu depuis les bords (m)
    float4 ambient;           // rgb : lumière ambiante dedans
};

static const uint MaxInteriors = 8;

// Doit correspondre à FrameData dans MeshRenderer.cpp. Matrices par colonnes : aucune ambiguïté de convention.
struct FrameData
{
    float4 viewProjection[4];
    float4 cascades[4 * 4];   // cascade c : colonnes [4c, 4c + 3] ; relatif caméra -> carte (xy dans [-1, 1], z dans [0, 1])
    float4 cascadeEnds;       // fin de chaque tranche, en mètres devant la caméra
    float4 cascadeTexels;     // côté d'un texel de chaque cascade, en mètres
    float4 cameraForward;     // xyz
    float4 toSun;             // xyz : direction du soleil (unitaire)
    float4 sunColor;          // rgb : couleur x intensité
    // x : multiplie la texture du ciel ; y : niveau de mip de la lumière diffuse ; z : niveau de mip des reflets les
    // plus flous ; w : exposition
    float4 environment;
    uint4 lightCount;         // x : nombre de lumières dans lights ; y : nombre de volumes intérieurs
    InteriorData interiors[MaxInteriors];
};

// Doit correspondre à LightData dans MeshRenderer.cpp : une lumière de la scène.
struct LightData
{
    float4 positionRange;     // xyz : position relative à la caméra ; w : portée (m)
    float4 color;             // rgb : couleur x intensité ; w : 1 spot, 0 ponctuelle
    float4 direction;         // xyz : axe du spot (unitaire) ; w : cosinus du demi-angle du cône
    float4 spot;              // x : 1 / (cosinus du début du fondu - w de direction)
};

// Doit correspondre à DrawData dans MeshRenderer.cpp : une entité dessinée.
struct DrawData
{
    float4 world[4];          // relatif à la caméra, par colonnes
    // Matrice des normales par colonnes dans xyz. normal[1].w : premier os du modèle dans jointColumns (bits d'un
    // entier), modèles animés seulement.
    float4 normal[3];
};

// Doit correspondre à DrawParams dans MeshRenderer.cpp : une primitive (un matériau) d'une entité.
struct DrawParams
{
    float4 color;             // linéaire : couleur de base du matériau x teinte de l'entité
    float4 emissive;          // linéaire : lumière propre du matériau (rgb), multipliée par l'image émissive
    uint draw;                // dans draws
    uint cascade;             // carte d'ombres en cours de dessin
    float alphaCutoff;        // 0 : pas de seuil
    uint transparent;         // 1 : l'alpha sort vers le mélange ; 0 : opaque (alpha 1)
    float metallic;           // x canal bleu de metallicRoughnessTexture
    float roughness;          // x canal vert
    float normalScale;        // 0 : pas de carte de normales
    float occlusionStrength;  // 0 : pas d'occlusion
    uint materialOverride;    // 1 : metallic et roughness tels quels, sans l'image métal-rugosité
};

[[vk::push_constant]] ConstantBuffer<DrawParams> params;

// Groupe 0 : le matériau, blanc 1 x 1 sans image. Couleurs en sRGB (le GPU les ramène en linéaire à la lecture),
// données (métal-rugosité, normales, occlusion) telles quelles.
[[vk::binding(0, 0)]] Sampler2D baseColorTexture;
[[vk::binding(1, 0)]] Sampler2D emissiveTexture;
[[vk::binding(2, 0)]] Sampler2D metallicRoughnessTexture; // vert : rugosité, bleu : métal
[[vk::binding(3, 0)]] Sampler2D normalTexture;            // repère tangent, comme glTF
[[vk::binding(4, 0)]] Sampler2D occlusionTexture;         // rouge

// Groupe 1 : l'image en cours.
// Matrices des os de tous les modèles animés de l'image (monde du modèle x inverse de liaison), 4 colonnes par os.
[[vk::binding(0, 1)]] StructuredBuffer<float4> jointColumns;
[[vk::binding(1, 1)]] StructuredBuffer<DrawData> draws;
[[vk::binding(2, 1)]] StructuredBuffer<FrameData> frame;
// Cartes des cascades, en 2 x 2 dans une seule texture (cascade c : colonne c % 2, ligne c / 2).
[[vk::binding(3, 1)]] Sampler2DShadow shadowMap;
// Lumières de la scène qui touchent le champ de la caméra (frame[0].lightCount.x).
[[vk::binding(4, 1)]] StructuredBuffer<LightData> lights;
// Le ciel tout autour (SkyRenderer), réduit à chaque niveau de mip : de plus en plus flou.
[[vk::binding(5, 1)]] SamplerCube environmentMap;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv; // (0, 0) en haut à gauche de l'image, comme en glTF
};

struct SkinnedVertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] uint4 joints;    // indices des os dans le modèle
    [[vk::location(4)]] float4 weights;  // somme 1
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

float4 Transform(float4 columns[4], float3 p)
{
    return columns[0] * p.x + columns[1] * p.y + columns[2] * p.z + columns[3];
}

// Position et normale du sommet dans le modèle, déplacées par ses os (moyenne pondérée de leurs matrices).
void Skin(SkinnedVertexInput input, uint firstJoint, out float3 position, out float3 normal)
{
    float4 column0 = float4(0.0);
    float4 column1 = float4(0.0);
    float4 column2 = float4(0.0);
    float4 column3 = float4(0.0);
    [unroll]
    for (int k = 0; k < 4; ++k) {
        const float weight = input.weights[k];
        const uint base = (firstJoint + input.joints[k]) * 4;
        column0 += jointColumns[base + 0] * weight;
        column1 += jointColumns[base + 1] * weight;
        column2 += jointColumns[base + 2] * weight;
        column3 += jointColumns[base + 3] * weight;
    }
    position = (column0 * input.position.x + column1 * input.position.y + column2 * input.position.z + column3).xyz;
    // Les os tournent sans étirer (en pratique) : leur 3x3 convient aux normales, renormalisées au pixel.
    normal = column0.xyz * input.normal.x + column1.xyz * input.normal.y + column2.xyz * input.normal.z;
}

VertexOutput Output(float3 position, float3 normal, float2 uv)
{
    const DrawData draw = draws[params.draw];
    VertexOutput output;
    output.worldPosition = Transform(draw.world, position).xyz;
    output.position = Transform(frame[0].viewProjection, output.worldPosition);
    output.normal = draw.normal[0].xyz * normal.x + draw.normal[1].xyz * normal.y + draw.normal[2].xyz * normal.z;
    output.uv = uv;
    return output;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    return Output(input.position, input.normal, input.uv);
}

[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[params.draw].normal[1].w), position, normal);
    return Output(position, normal, input.uv);
}

// Part de la lumière du soleil qui atteint ce point (0 : dans l'ombre, 1 : éclairé).
float SunVisibility(float3 worldPosition, float3 normal, float sunFacing)
{
    const FrameData f = frame[0];
    const float depth = dot(worldPosition, f.cameraForward.xyz);
    int cascade = 0;
    while (cascade < 4 && depth > f.cascadeEnds[cascade]) {
        ++cascade;
    }
    if (cascade >= 4) {
        return 1.0; // trop loin : pas d'ombre
    }
    // Décalé le long de la normale (plus sur une face rasante) : une surface ne se fait pas d'ombre à elle-même.
    const float texel = f.cascadeTexels[cascade];
    const float3 offsetPosition = worldPosition + normal * texel * (1.5 + 2.0 * (1.0 - sunFacing));
    float4 m[4] = {f.cascades[cascade * 4 + 0], f.cascades[cascade * 4 + 1], f.cascades[cascade * 4 + 2],
                   f.cascades[cascade * 4 + 3]};
    const float3 light = Transform(m, offsetPosition).xyz;
    const float2 local = light.xy * 0.5 + 0.5;
    if (any(local < 0.0) || any(local > 1.0) || light.z > 1.0) {
        return 1.0;
    }
    // Carte de la cascade dans l'atlas 2 x 2, sans déborder sur la voisine (le filtre lit un texel autour).
    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    const float2 atlasTexel = 1.0 / float2(width, height);
    const float2 cell = float2(cascade % 2, cascade / 2);
    const float2 inside = clamp(local * 0.5, atlasTexel * 1.5, 0.5 - atlasTexel * 1.5);
    const float2 uv = cell * 0.5 + inside;
    // 4 lectures filtrées (chacune compare 2 x 2 texels) : bord doux sur 3 texels environ.
    float lit = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const float2 offset = float2((i & 1) != 0 ? 0.5 : -0.5, (i & 2) != 0 ? 0.5 : -0.5) * atlasTexel;
        lit += shadowMap.SampleCmpLevelZero(uv + offset, light.z);
    }
    lit *= 0.25;
    // La dernière cascade s'efface sur son dernier dixième : pas de limite nette au loin.
    if (cascade == 3) {
        const float fade = saturate((f.cascadeEnds[3] - depth) / (0.1 * f.cascadeEnds[3]));
        lit = lerp(1.0, lit, fade);
    }
    return lit;
}

// Surface vue en un pixel, de quoi l'éclairer (BRDF de Cook-Torrance : GGX, Smith, Schlick, comme glTF).
struct Surface
{
    float3 normal;     // unitaire, vers l'œil pour une face arrière double face
    float3 toEye;      // unitaire
    float3 diffuse;    // couleur diffuse (zéro pour un métal)
    float3 f0;         // reflet vu de face : 4 % pour un isolant, la couleur de base pour un métal
    float roughness;   // perçue, de 0 (miroir) à 1 (mat)
};

static const float Pi = 3.14159265;

// Lumière renvoyée vers l'œil par une lumière qui arrive de `toLight` avec l'éclairement `light` (perpendiculaire).
// Multipliée par pi : une surface blanche mate face à une lumière 1 renvoie 1, comme l'ancien éclairage diffus.
float3 Shade(Surface s, float3 toLight, float3 light)
{
    const float nl = dot(s.normal, toLight);
    if (nl <= 0.0) {
        return float3(0.0);
    }
    const float3 h = normalize(toLight + s.toEye);
    const float nv = max(dot(s.normal, s.toEye), 1.0e-4);
    const float nh = saturate(dot(s.normal, h));
    const float vh = saturate(dot(s.toEye, h));
    // Une rugosité nulle donnerait un reflet infiniment fin (et des pixels qui scintillent) : un plancher.
    const float alpha = max(s.roughness * s.roughness, 0.002);
    const float a2 = alpha * alpha;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float distribution = a2 / (Pi * d * d);
    // Visibilité de Smith corrélée (masquage de la lumière et du regard par les microfacettes), divisée par 4 nl nv.
    const float visibility = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2));
    const float3 fresnel = s.f0 + (1.0 - s.f0) * pow(1.0 - vh, 5.0);
    const float3 specular = fresnel * (distribution * visibility * Pi);
    return (s.diffuse * (1.0 - fresnel) + specular) * light * nl;
}

// Lumière ambiante et reflets, venus du ciel : la lumière diffuse est la texture du ciel très réduite (une moyenne
// du côté vers lequel la surface est tournée) ; le reflet la lit dans la direction réfléchie, d'autant plus floue
// que la surface est rugueuse. La part du reflet vient de l'approximation de Karis (pré-intégration de la BRDF).
float3 Ambient(Surface s, float occlusion, float3 worldPosition)
{
    const FrameData f = frame[0];
    // Dans un volume intérieur, la lumière du volume remplace celle du ciel (fondue près des bords).
    float inside = 0.0;
    float3 interiorLight = float3(0.0);
    for (uint i = 0; i < f.lightCount.y; ++i) {
        const InteriorData volume = f.interiors[i];
        const float3 local = float3(dot(volume.axes[0].xyz, worldPosition) + volume.axes[0].w,
                                    dot(volume.axes[1].xyz, worldPosition) + volume.axes[1].w,
                                    dot(volume.axes[2].xyz, worldPosition) + volume.axes[2].w);
        // Distance au bord le plus proche, positive dedans.
        const float3 margin = volume.halfSize.xyz - abs(local);
        const float weight = saturate(min(margin.x, min(margin.y, margin.z)) / volume.halfSize.w);
        if (weight > inside) {
            inside = weight;
            interiorLight = volume.ambient.rgb;
        }
    }
    const float3 diffuseLight = lerp(environmentMap.SampleLevel(s.normal, f.environment.y).rgb * f.environment.x,
                                     interiorLight, inside);
    const float3 reflected = reflect(-s.toEye, s.normal);
    // Rugueux : la direction du reflet glisse vers la normale (le lobe s'élargit autour d'elle).
    const float3 reflectDirection = normalize(lerp(reflected, s.normal, s.roughness * s.roughness));
    const float3 specularLight = lerp(environmentMap.SampleLevel(reflectDirection, s.roughness * f.environment.z).rgb * f.environment.x,
                                      interiorLight, inside);
    const float nv = saturate(dot(s.normal, s.toEye));
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = s.roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    const float2 scaleBias = float2(-1.04, 1.04) * a004 + r.zw;
    const float3 specular = s.f0 * scaleBias.x + scaleBias.y;
    return (s.diffuse * diffuseLight + specular * specularLight) * occlusion;
}

// Lumière renvoyée vers l'œil par les lumières de la scène (ponctuelles et spots). Affaiblies comme 1 / (1 + d²)
// (d en mètres : la moitié à 1 m, sans l'infini d'une vraie source ponctuelle collée à la surface), puis éteintes en
// douceur à leur portée.
float3 SceneLights(float3 worldPosition, Surface s)
{
    float3 sum = float3(0.0);
    const uint count = frame[0].lightCount.x;
    for (uint i = 0; i < count; ++i) {
        const LightData light = lights[i];
        const float3 toLight = light.positionRange.xyz - worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float range = light.positionRange.w;
        if (distanceSquared >= range * range) {
            continue;
        }
        const float distance = sqrt(distanceSquared);
        const float3 direction = toLight / max(distance, 1.0e-4);
        if (dot(s.normal, direction) <= 0.0) {
            continue;
        }
        const float ratio = distanceSquared / (range * range);
        const float window = saturate(1.0 - ratio * ratio);
        float attenuation = window * window / (1.0 + distanceSquared);
        if (light.color.w > 0.5) {
            // Spot : plein dans le cœur du cône, fondu vers son bord.
            const float cosine = dot(-direction, light.direction.xyz);
            const float edge = saturate((cosine - light.direction.w) * light.spot.x);
            attenuation *= edge * edge;
        }
        sum += Shade(s, direction, light.color.rgb * attenuation);
    }
    return sum;
}

// Normale de la carte de normales, sans tangentes dans les sommets : le repère tangent est retrouvé au pixel par
// les dérivées de la position et des UV (« cotangent frame » de Schüler). Marche aussi avec des UV en miroir.
float3 PerturbNormal(float3 normal, float3 worldPosition, float2 uv)
{
    float3 mapped = normalTexture.Sample(uv).xyz * 2.0 - 1.0;
    mapped.xy *= params.normalScale;
    const float3 dp1 = ddx(worldPosition);
    const float3 dp2 = ddy(worldPosition);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, normal);
    const float3 dp1perp = cross(normal, dp1);
    // glTF : v des UV vers le bas de l'image, la bitangente de la carte vers son haut.
    const float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 bitangent = -(dp2perp * duv1.y + dp1perp * duv2.y);
    const float lengthSquared = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (lengthSquared <= 1.0e-20) {
        return normal; // UV dégénérées (toutes égales) : pas de relief
    }
    const float scale = rsqrt(lengthSquared);
    return normalize(tangent * (mapped.x * scale) + bitangent * (mapped.y * scale) + normal * mapped.z);
}

// Courbe de tons ACES (approximation de Narkowicz) : les couleurs trop fortes sont adoucies vers le blanc au lieu
// d'être coupées net. La même dans shaders/Sky.slang.
float3 ToneMap(float3 color)
{
    color *= frame[0].environment.w * 0.6;
    return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    float4 base = baseColorTexture.Sample(input.uv) * params.color;
    if (base.a < params.alphaCutoff) {
        discard;
    }
    // Face arrière d'un matériau double face : éclairée comme vue de son côté.
    const float3 geometryNormal = normalize(input.normal) * (frontFacing ? 1.0 : -1.0);
    float3 normal = geometryNormal;
    if (params.normalScale != 0.0) { // le même pour toute la primitive
        normal = PerturbNormal(geometryNormal, input.worldPosition, input.uv);
    }
    const float4 metallicRoughness = metallicRoughnessTexture.Sample(input.uv);
    const bool fixed = params.materialOverride != 0;
    const float metallic = saturate(params.metallic * (fixed ? 1.0 : metallicRoughness.b));
    Surface surface;
    surface.normal = normal;
    surface.toEye = normalize(-input.worldPosition); // la caméra est à l'origine
    surface.diffuse = base.rgb * (1.0 - metallic);
    surface.f0 = lerp(float3(0.04), base.rgb, metallic);
    surface.roughness = saturate(params.roughness * (fixed ? 1.0 : metallicRoughness.g));
    const float occlusion = 1.0 + params.occlusionStrength * (occlusionTexture.Sample(input.uv).r - 1.0);

    const FrameData f = frame[0];
    const float3 toSun = f.toSun.xyz;
    // Ombre lue avec la vraie face (le relief de la carte ne déplace pas les ombres).
    const float sunFacing = dot(geometryNormal, toSun);
    const float sun = sunFacing > 0.0 ? SunVisibility(input.worldPosition, geometryNormal, sunFacing) : 0.0;
    float3 lit = Shade(surface, toSun, f.sunColor.rgb * sun) + Ambient(surface, occlusion, input.worldPosition)
               + SceneLights(input.worldPosition, surface);
    if (any(params.emissive.rgb > 0.0)) { // le même pour toute la primitive : pas d'image lue sans émissif
        lit += params.emissive.rgb * emissiveTexture.Sample(input.uv).rgb;
    }
    return float4(ToneMap(lit), params.transparent != 0 ? base.a : 1.0);
}

// Cartes d'ombres : la position vue du soleil (cascade params.cascade), et l'image seulement pour le seuil d'alpha.
struct ShadowOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

ShadowOutput ShadowVertex(float3 position, float2 uv)
{
    const FrameData f = frame[0];
    const uint c = params.cascade;
    float4 m[4] = {f.cascades[c * 4 + 0], f.cascades[c * 4 + 1], f.cascades[c * 4 + 2], f.cascades[c * 4 + 3]};
    ShadowOutput output;
    output.position = Transform(m, Transform(draws[params.draw].world, position).xyz);
    output.uv = uv;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadow(VertexInput input)
{
    return ShadowVertex(input.position, input.uv);
}

[shader("vertex")]
ShadowOutput vertexShadowSkinned(SkinnedVertexInput input)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[params.draw].normal[1].w), position, normal);
    return ShadowVertex(position, input.uv);
}

[shader("fragment")]
void fragmentShadow(ShadowOutput input)
{
    if (params.alphaCutoff > 0.0 && baseColorTexture.Sample(input.uv).a * params.color.a < params.alphaCutoff) {
        discard;
    }
}
