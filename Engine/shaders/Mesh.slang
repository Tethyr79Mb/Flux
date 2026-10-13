// Maillages (cubes et modèles glTF), opaques ou transparents : position relative à la caméra, matériaux PBR comme
// glTF (couleur de base, métal, rugosité, carte de normales, occlusion) éclairés par un soleil avec ses ombres
// (cascades), la lumière ambiante et les reflets du ciel (texture cube) et les lumières de la scène (ponctuelles et
// spots, sans ombre) ; plus la lumière propre du matériau (émissif). Puis exposition et courbe de tons (ACES) : la sortie est
// déjà la couleur affichée (en linéaire, la swapchain sRGB fait le reste).
// Modèles animés (vertexSkinned) : chaque sommet est d'abord déplacé par ses os (skinning).
// Tuiles de planète (vertexPlanet) : chaque sommet glisse vers la forme de la tuile parente en s'éloignant de la
// caméra, pour qu'un changement de niveau de détail ne fasse pas de saut (« morphing » façon CDLOD). Leur sol
// (fragmentPlanet) est choisi au pixel : roche dans les pentes, plage en bas, neige en haut, et ailleurs selon le climat
// (température et humidité) : herbe, terre nue, désert de sable, toundra, avec du bruit.
// Cartes d'ombres (vertexShadow, vertexShadowSkinned, vertexShadowPlanet, fragmentShadow) : profondeur vue du soleil, rien d'autre.
// Près d'une planète à atmosphère : le soleil rougi ou caché (nuit), la lumière du ciel, et la brume entre la caméra et
// la surface (shaders/Atmosphere.slang).

#include "Atmosphere.slang"

// Doit correspondre à InteriorData dans MeshRenderer.cpp : un volume intérieur (boîte), où le ciel n'éclaire pas.
struct InteriorData
{
    float4 axes[3];           // local[i] = dot(axes[i].xyz, position relative à la caméra) + axes[i].w
    float4 halfSize;          // xyz : demi-taille (m) ; w : fondu depuis les bords (m)
    float4 ambient;           // rgb : lumière ambiante dedans
};

static const uint MaxInteriors = 8;

// Doit correspondre à PlanetData dans MeshRenderer.cpp : le sol d'une planète (PlanetComponent).
struct PlanetData
{
    float4 shape;             // x : rayon (m) ; y : relief (m) ; z : altitude de la neige (m)
    float4 rock;              // rgb : couleur (linéaire ; avec les textures du sol : teinte) ; w : rugosité
    float4 grass;
    float4 sand;
    float4 snow;
    float4 water;             // rgb : couleur de l'eau (linéaire) ; w : rayon de la surface de l'océan (0 : aucun)
    float4 soil;
    float4 climate;           // x : température à l'équateur (°C) ; y : aux pôles ; z : humidité (0 à 1)
    float4 beach;             // x : altitude du niveau de la mer (m) ; y : hauteur de la plage au-dessus (m)
    float4 rim;               // rgb : couleur x intensité de la lumière de contour ; w : finesse (0 : aucune)
};

static const uint MaxPlanets = 4;

// Doit correspondre à FrameData dans MeshRenderer.cpp. Matrices par colonnes : aucune ambiguïté de convention.
struct FrameData
{
    float4 viewProjection[4];
    float4 cascades[4 * 4];   // cascade c : colonnes [4c, 4c + 3] ; relatif caméra -> carte (xy dans [-1, 1], z dans [0, 1])
    float4 cascadeEnds;       // fin de chaque tranche, en mètres devant la caméra
    float4 cascadeTexels;     // côté d'un texel de chaque cascade, en mètres
    float4 cameraForward;     // xyz ; w : secondes écoulées (vagues), repartent de 0 toutes les heures
    float4 toSun;             // xyz : direction du soleil (unitaire)
    float4 sunColor;          // rgb : couleur x intensité
    // x : multiplie la texture du ciel ; y : niveau de mip de la lumière diffuse ; z : niveau de mip des reflets les
    // plus flous ; w : exposition
    float4 environment;
    uint4 lightCount;         // x : nombre de lumières dans lights ; y : nombre de volumes intérieurs ; z : de planètes
    InteriorData interiors[MaxInteriors];
    PlanetData planets[MaxPlanets];
    AtmosphereData atmosphere;
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
    // entier), modèles animés seulement. Tuiles de planète : normal[0].w, distance où commence le glissement vers la
    // forme parente ; normal[2].w, 1 / sa longueur (0 : pas de glissement).
    float4 normal[3];
    // Lumière de contour : couleur x intensité (rgb), finesse (w ; 0 : aucune). Tuiles de planète : xyz, centre de la
    // tuile dans le repère de la planète ; w, indice dans FrameData::planets (bits d'un entier).
    float4 rim;
    float4 ground;            // tuile de planète : xyz, centre de rim modulo GroundPeriod (MeshRenderer.hpp)
};

// Doit correspondre à DrawParams dans MeshRenderer.cpp : une primitive (un matériau) d'une entité.
struct DrawParams
{
    float4 color;             // linéaire : couleur de base du matériau x teinte de l'entité
    float4 emissive;          // linéaire : lumière propre du matériau (rgb), multipliée par l'image émissive
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
// Tuiles de planète avec textures du sol (GroundTextures.hpp) : une couche par matière (GroundLayer). Images blanches
// pour les autres maillages, qui ne les lisent pas.
[[vk::binding(5, 0)]] Sampler2DArray groundColorTexture;
[[vk::binding(6, 0)]] Sampler2DArray groundRoughnessTexture; // rouge : occlusion, vert : rugosité
[[vk::binding(7, 0)]] Sampler2DArray groundNormalTexture;    // repère tangent (OpenGL : vert vers le haut)

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

// Doit correspondre à MorphVertex dans Flux/Graphics/Planet.hpp : où le sommet d'une tuile de planète arrive
// quand la tuile prend la forme de sa parente.
struct PlanetVertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(5)]] float3 morphOffset;  // ajouté à la position
    [[vk::location(6)]] float3 morphNormal;  // normale de la parente à cet endroit
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    nointerpolation uint draw : DRAW; // dans draws
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

// drawIndex : l'indice de l'instance (SV_VulkanInstanceID), que MeshRenderer donne à chaque dessin, direct ou indirect.
VertexOutput Output(uint drawIndex, float3 position, float3 normal, float2 uv)
{
    const DrawData draw = draws[drawIndex];
    VertexOutput output;
    output.draw = drawIndex;
    output.worldPosition = Transform(draw.world, position).xyz;
    output.position = Transform(frame[0].viewProjection, output.worldPosition);
    output.normal = draw.normal[0].xyz * normal.x + draw.normal[1].xyz * normal.y + draw.normal[2].xyz * normal.z;
    output.uv = uv;
    return output;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint draw : SV_VulkanInstanceID)
{
    return Output(draw, input.position, input.normal, input.uv);
}

[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input, uint draw : SV_VulkanInstanceID)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[draw].normal[1].w), position, normal);
    return Output(draw, position, normal, input.uv);
}

// Glissement du sommet vers la forme parente : 0 près de la caméra, 1 au-delà de la distance où la parente prend
// le relais (calculé sur le sommet de la tuile, pour que deux tuiles voisines glissent pareil au même endroit).
float PlanetMorph(uint drawIndex, float3 position)
{
    const DrawData draw = draws[drawIndex];
    const float distance = length(Transform(draw.world, position).xyz);
    return saturate((distance - draw.normal[0].w) * draw.normal[2].w);
}

struct PlanetVertexOutput
{
    VertexOutput base;
    float3 planetPosition : PLANET; // dans le repère de la planète (son centre à l'origine)
    // Le même modulo GroundPeriod : coordonnées des textures du sol, précises même loin du centre de la planète.
    float3 groundPosition : GROUND;
    // Sol : température (°C) et humidité (0 à 1) sans les taches de moins de 400 m (voir PlanetClimate).
    float2 climate : CLIMATE;
    // L'air entre la caméra et le sommet (voir AirTo) : calculé par sommet, pas par pixel. Il varie doucement avec la
    // distance, et les tuiles gardent à peu près la même taille à l'écran : la différence ne se voit pas, et le pixel
    // n'a plus ses 16 pas de marche dans l'air.
    float3 airTransmittance : AIR_TRANSMITTANCE;
    float3 airScattered : AIR_SCATTERED;
    // Océan : le ciel de l'atmosphère que reflète une mer plate (sans les vaguelettes), et la part du ciel de l'espace
    // qui le traverse. Par sommet, comme l'air.
    float3 seaSky : SEA_SKY;
    float3 seaSkyThrough : SEA_SKY_THROUGH;
};

float3 AirTo(float3 worldPosition, out float3 transmittance);

// Climat d'un point de la planète (local : repère de la planète), aux grandes échelles seulement : calculé par sommet,
// pas par pixel, car il varie sur des kilomètres (les taches de 20 à 400 m s'ajoutent au pixel). Température : de
// l'équateur aux pôles (axe y de la planète ; froide surtout au-delà de 50°), 6,5 °C de moins par km d'altitude, plus
// des écarts de quelques degrés. Humidité : bruit de 300 km et de 30 km autour de celle de la planète, plus forte dans
// les 2 % du relief au-dessus de la mer. Ces échelles ne s'effacent pas au loin : vues de l'espace, les zones restent.
// Lumière de contour d'une planète (RimLightComponent) : selon sa sphère, pas son relief, pour un liseré net au bord
// du disque sans faire briller chaque montagne de profil. Ajoutée après l'air : elle reste visible à travers lui.
float3 PlanetRim(PlanetData planet, float3 worldUp, float3 toEye)
{
    if (planet.rim.w <= 0.0) {
        return float3(0.0);
    }
    return planet.rim.rgb * pow(1.0 - saturate(dot(worldUp, toEye)), planet.rim.w);
}

float2 PlanetClimate(PlanetData planet, float3 local)
{
    const float radius = planet.shape.x;
    const float relief = max(planet.shape.y, 1.0);
    const float distance = length(local);
    const float altitude = distance - radius;
    const float latitude = abs(local.y) / max(distance, 1.0);
    const float temperature = lerp(planet.climate.x, planet.climate.y, (latitude * latitude) * (latitude * latitude))
                            - 0.0065 * max(altitude, 0.0) + (PlanetNoise(local * (100.0 / radius)) - 0.5) * 8.0;
    const float coast = planet.water.w > 0.0 ? smoothstep(0.02 * relief, 0.0, distance - planet.water.w) : 0.0;
    const float humidity = planet.climate.z + (PlanetNoise(local * (2.0 / radius) + 17.0) - 0.5) * 0.9
                         + (PlanetNoise(local * (1.0 / 30000.0) + 5.0) - 0.5) * 0.4 + coast * 0.3;
    return float2(temperature, humidity);
}

[shader("vertex")]
PlanetVertexOutput vertexPlanet(PlanetVertexInput input, uint draw : SV_VulkanInstanceID)
{
    const float morph = PlanetMorph(draw, input.position);
    const float3 position = input.position + input.morphOffset * morph;
    PlanetVertexOutput output;
    output.base = Output(draw, position, lerp(input.normal, input.morphNormal, morph), input.uv);
    output.planetPosition = draws[draw].rim.xyz + position;
    output.groundPosition = draws[draw].ground.xyz + position;
    output.climate = PlanetClimate(frame[0].planets[asuint(draws[draw].rim.w)], output.planetPosition);
    output.airScattered = AirTo(output.base.worldPosition, output.airTransmittance);
    output.seaSky = float3(0.0);
    output.seaSkyThrough = float3(1.0);
    return output;
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
// Volume intérieur où se trouve ce point : son poids (0 dehors, 1 bien dedans, fondu près des bords) et sa lumière.
float InteriorAt(float3 worldPosition, out float3 interiorLight)
{
    const FrameData f = frame[0];
    float inside = 0.0;
    interiorLight = float3(0.0);
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
    return inside;
}

float3 Ambient(Surface s, float occlusion, float inside, float3 interiorLight, float skyScale)
{
    const FrameData f = frame[0];
    const float sky = f.environment.x * skyScale;
    // Dans un volume intérieur, la lumière du volume remplace celle du ciel (fondue près des bords).
    const float3 diffuseLight = lerp(environmentMap.SampleLevel(s.normal, f.environment.y).rgb * sky,
                                     interiorLight, inside);
    const float3 reflected = reflect(-s.toEye, s.normal);
    // Rugueux : la direction du reflet glisse vers la normale (le lobe s'élargit autour d'elle).
    const float3 reflectDirection = normalize(lerp(reflected, s.normal, s.roughness * s.roughness));
    const float3 specularLight = lerp(environmentMap.SampleLevel(reflectDirection, s.roughness * f.environment.z).rgb * sky,
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

// Lumière renvoyée vers l'œil par une surface : soleil (ombres, rougi par l'air), ambiante du ciel ou du volume
// intérieur, lumières de la scène. inside : poids du volume intérieur où elle se trouve.
float3 LightSurface(Surface surface, float3 geometryNormal, float3 worldPosition, float occlusion, out float inside)
{
    const FrameData f = frame[0];
    const float3 toSun = f.toSun.xyz;
    // Ombre lue avec la vraie face (le relief de la carte ne déplace pas les ombres).
    const float sunFacing = dot(geometryNormal, toSun);
    const float sun = sunFacing > 0.0 ? SunVisibility(worldPosition, geometryNormal, sunFacing) : 0.0;
    float3 interiorLight;
    inside = InteriorAt(worldPosition, interiorLight);
    float3 sunColor = f.sunColor.rgb;
    float skyScale = 1.0;
    if (HasAtmosphere(f.atmosphere)) {
        skyScale = AtmosphereNightSky(f.atmosphere, worldPosition - f.atmosphere.center.xyz, toSun);
    }
    float3 ambient = Ambient(surface, occlusion, inside, interiorLight, skyScale);
    if (HasAtmosphere(f.atmosphere)) {
        // Le soleil traverse l'air jusqu'ici ; le ciel éclaire aussi (pas dans un volume intérieur).
        const float3 local = worldPosition - f.atmosphere.center.xyz;
        sunColor *= AtmosphereSunTransmittance(f.atmosphere, local, toSun);
        const float3 skyLight = AtmosphereAmbient(f.atmosphere, local, surface.normal, toSun) * ((1.0 - inside) * occlusion);
        ambient += (surface.diffuse + surface.f0 * (1.0 - surface.roughness)) * skyLight;
    }
    return Shade(surface, toSun, sunColor * sun) + ambient + SceneLights(worldPosition, surface);
}

// L'air entre la caméra et la surface (près d'une planète à atmosphère) : il voile ce qui est loin et ajoute sa
// propre lumière. 16 pas fixes, sans bruit par pixel : ce bruit dessinait un damier fin sur le sol, et 8 pas fixes
// des bandes (le ciel, lui, garde son bruit). Renvoie la lumière ajoutée ; transmittance : la part de celle de la
// surface qui passe.
float3 AirTo(float3 worldPosition, out float3 transmittance)
{
    const FrameData f = frame[0];
    transmittance = float3(1.0);
    if (!HasAtmosphere(f.atmosphere)) {
        return float3(0.0);
    }
    const float distance = length(worldPosition);
    return AtmosphereScatter(f.atmosphere, worldPosition / max(distance, 1.0e-4), distance, f.toSun.xyz, f.sunColor.rgb,
                             16, 0.5, transmittance);
}

float3 ThroughAir(float3 lit, float3 worldPosition)
{
    float3 transmittance;
    const float3 scattered = AirTo(worldPosition, transmittance);
    return lit * transmittance + scattered;
}

// Bruit de valeur 3D (de 0 à 1), lisse : hachage des coins entiers, interpolés.
float PlanetHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1) * 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float PlanetNoise(float3 x)
{
    const float3 i = floor(x);
    float3 t = frac(x);
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(lerp(PlanetHash(i), PlanetHash(i + float3(1, 0, 0)), t.x),
                     lerp(PlanetHash(i + float3(0, 1, 0)), PlanetHash(i + float3(1, 1, 0)), t.x), t.y),
                lerp(lerp(PlanetHash(i + float3(0, 0, 1)), PlanetHash(i + float3(1, 0, 1)), t.x),
                     lerp(PlanetHash(i + float3(0, 1, 1)), PlanetHash(i + float3(1, 1, 1)), t.x), t.y), t.z);
}

// --- Textures du sol (GroundTextures) ---
// Matières, dans l'ordre des couches (GroundLayer dans GroundTextures.hpp).
static const uint GroundRock = 0;
static const uint GroundGrass = 1;
static const uint GroundSoil = 2;
static const uint GroundSand = 3;
static const uint GroundSnow = 4;
static const uint GroundLayers = 5;
// Côté d'une image sur le sol (m), de près et de loin (la répétition se voit moins). GroundPeriod (1024 m) en est un
// multiple.
static const float GroundNearScale = 4.0;
static const float GroundFarScale = 32.0;
static const float GroundPeriod = 1024.0; // même valeur que MeshRenderer::GroundPeriod (C++)
// Contre la répétition de l'image (un quadrillage visible tous les GroundNearScale mètres) : pavage en triangles
// presque équilatéraux (« hex tiling », Mikkelsen 2022), chaque sommet lit l'image décalée et tournée au hasard, et
// chaque point mêle les trois sommets de son triangle, poids durcis (le plus proche l'emporte vite). Pas de carrés :
// les zones forment des hexagones aux bords fondus. Triangles de côté GroundTriangleSide images, hauteur 1/2 : choisi
// pour que le pavage se répète exactement sur GroundPeriod, comme les coordonnées du sol (pas de couture entre tuiles).
static const float GroundTriangleSide = 4.0 / 7.0;
static const float GroundTriangleHeight = 0.5;
static const float GroundBlendSharpness = 4.0;

struct GroundSample
{
    float3 color;
    float3 normal;    // repère de la planète, non normalisée
    float2 occlusionRoughness;
};

// Hachage entier (lowbias32).
uint GroundHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float2 GroundRandom(uint x)
{
    const uint h = GroundHash(x);
    return float2(float(h & 0xffffU), float(h >> 16)) * (1.0 / 65536.0);
}

// Une version de l'image (décalage et rotation tirés de `variant`) ; normale ramenée dans le repère du plan de
// projection (rotation inverse).
void SampleGroundVariant(out float3 color, out float3 tangentNormal, out float2 occlusionRoughness, uint layer,
                         float2 uv, float2 gx, float2 gy, uint variant)
{
    const float2 random = GroundRandom(variant * 0x68e31da4U + 17u);
    const float angle = random.x * 6.2831853;
    const float c = cos(angle);
    const float s = sin(angle);
    const float2x2 rotation = float2x2(c, -s, s, c);
    const float3 at = float3(mul(rotation, uv) + GroundRandom(variant * 0xb5297a4dU + 71u), float(layer));
    const float2 rgx = mul(rotation, gx);
    const float2 rgy = mul(rotation, gy);
    color = groundColorTexture.SampleGrad(at, rgx, rgy).rgb;
    occlusionRoughness = groundRoughnessTexture.SampleGrad(at, rgx, rgy).rg;
    tangentNormal = groundNormalTexture.SampleGrad(at, rgx, rgy).xyz * 2.0 - 1.0;
    tangentNormal.xy = mul(tangentNormal.xy, rotation); // transposée : rotation inverse
}

// Une matière dans un plan de projection, sans répétition visible : les trois sommets du triangle du point (voir
// GroundTriangleSide), une à trois lectures selon les poids.
void SampleGroundNoRepeat(out float3 color, out float3 tangentNormal, out float2 occlusionRoughness, uint layer,
                          float2 uv, float2 gx, float2 gy, int period)
{
    // Repère du pavage : a le long d'un côté, b le long de l'autre (60° environ). Décaler uv d'une période change a et
    // b d'entiers ; la clé (2a + b, b), ramenée dans sa période, donne le même tirage aux deux points.
    const float bf = uv.y / GroundTriangleHeight;
    const float af = uv.x / GroundTriangleSide - 0.5 * bf;
    const int2 base = int2(floor(float2(af, bf)));
    const float2 f = float2(af, bf) - float2(base);
    // Triangle du bas (f.x + f.y < 1) : sommets (0,0), (1,0), (0,1) ; du haut : (1,1), (0,1), (1,0).
    const bool upper = f.x + f.y > 1.0;
    const int2 v0 = base + (upper ? int2(1, 1) : int2(0, 0));
    const int2 v1 = base + (upper ? int2(0, 1) : int2(1, 0));
    const int2 v2 = base + (upper ? int2(1, 0) : int2(0, 1));
    float3 w = upper ? float3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y) : float3(1.0 - f.x - f.y, f.x, f.y);
    w = pow(max(w, 0.0), GroundBlendSharpness);
    w /= w.x + w.y + w.z;

    const int periodU = int(2.0 * float(period) / GroundTriangleSide + 0.5); // 2a + b sur une période en x
    const int periodB = int(float(period) / GroundTriangleHeight + 0.5);
    color = float3(0.0);
    tangentNormal = float3(0.0);
    occlusionRoughness = float2(0.0);
    [unroll]
    for (int i = 0; i < 3; ++i) {
        if (w[i] > 0.01) {
            const int2 v = i == 0 ? v0 : i == 1 ? v1 : v2;
            const int u = ((2 * v.x + v.y) % periodU + periodU) % periodU;
            const int b = (v.y % periodB + periodB) % periodB;
            float3 c, n;
            float2 surface;
            SampleGroundVariant(c, n, surface, layer, uv, gx, gy, uint(u) * 0x9e3779b1U ^ GroundHash(uint(b) + 101u));
            color += c * w[i];
            tangentNormal += n * w[i];
            occlusionRoughness += surface * w[i];
        }
    }
    const float kept = (w.x > 0.01 ? w.x : 0.0) + (w.y > 0.01 ? w.y : 0.0) + (w.z > 0.01 ? w.z : 0.0);
    color /= kept;
    tangentNormal /= kept;
    occlusionRoughness /= kept;
}

// Une matière plaquée selon les trois axes de la planète (triplanaire), chaque projection pesée par l'orientation de
// la surface ; les projections presque de profil ne sont pas lues. Normales mêlées façon « whiteout » (Ben Golus).
// Pentes données (SampleGrad) : lu dans une branche qui varie d'un pixel à l'autre, le mip reste juste.
GroundSample SampleGround(uint layer, float3 p, float3 dpdx, float3 dpdy, float3 n, float3 blend, float scale)
{
    GroundSample result;
    result.color = float3(0.0);
    result.normal = float3(0.0);
    result.occlusionRoughness = float2(0.0);
    const float inverse = 1.0 / scale;
    const int period = int(GroundPeriod / scale + 0.5); // images par période
    const float3 axisSign = sign(n);
    [unroll]
    for (int axis = 0; axis < 3; ++axis) {
        if (blend[axis] > 0.0) {
            // Plan de projection : (z, y) pour x, (x, z) pour y, (x, y) pour z.
            const float2 uv = (axis == 0 ? p.zy : axis == 1 ? p.xz : p.xy) * inverse;
            const float2 gx = (axis == 0 ? dpdx.zy : axis == 1 ? dpdx.xz : dpdx.xy) * inverse;
            const float2 gy = (axis == 0 ? dpdy.zy : axis == 1 ? dpdy.xz : dpdy.xy) * inverse;
            float3 color;
            float3 t;
            float2 occlusionRoughness;
            SampleGroundNoRepeat(color, t, occlusionRoughness, layer, uv, gx, gy, period);
            result.color += color * blend[axis];
            result.occlusionRoughness += occlusionRoughness * blend[axis];
            t.z *= axisSign[axis];
            const float2 base = axis == 0 ? n.zy : axis == 1 ? n.xz : n.xy;
            const float3 w = float3(t.xy + base, abs(t.z) * n[axis]);
            result.normal += (axis == 0 ? w.zyx : axis == 1 ? w.xzy : w) * blend[axis];
        }
    }
    return result;
}

// La matière à deux échelles, fondues entre 60 et 150 m de la caméra.
GroundSample SampleGroundScales(uint layer, float3 p, float3 dpdx, float3 dpdy, float3 n, float3 blend, float distance)
{
    const float far = smoothstep(60.0, 150.0, distance);
    GroundSample result = SampleGround(layer, p, dpdx, dpdy, n, blend, far < 1.0 ? GroundNearScale : GroundFarScale);
    if (far > 0.0 && far < 1.0) {
        const GroundSample other = SampleGround(layer, p, dpdx, dpdy, n, blend, GroundFarScale);
        result.color = lerp(result.color, other.color, far);
        result.normal = lerp(result.normal, other.normal, far);
        result.occlusionRoughness = lerp(result.occlusionRoughness, other.occlusionRoughness, far);
    }
    return result;
}

// Teinte de la texture d'une matière (PlanetComponent::rockTint et suivantes, à la place des couleurs unies),
// ramenée à une luminance de 1 : elle change la couleur sans assombrir (un jaune clair dore un sable gris).
float3 GroundTint(PlanetData planet, uint layer)
{
    float3 tint;
    switch (layer) {
        case GroundRock: tint = planet.rock.rgb; break;
        case GroundGrass: tint = planet.grass.rgb; break;
        case GroundSoil: tint = planet.soil.rgb; break;
        case GroundSand: tint = planet.sand.rgb; break;
        default: tint = planet.snow.rgb; break;
    }
    return tint / max(dot(tint, float3(0.2126, 0.7152, 0.0722)), 0.01);
}

// Recouvre les matières déjà posées par `layer` à hauteur de `amount` (comme une couche de peinture).
void CoverGround(inout float weights[GroundLayers], uint layer, float amount)
{
    [unroll]
    for (uint i = 0; i < GroundLayers; ++i) {
        weights[i] *= 1.0 - amount;
    }
    weights[layer] += amount;
}

// Sol des planètes : roche dans les pentes, plage en bas, neige en haut ou quand il gèle, ailleurs herbe, terre ou
// désert selon le climat, avec des taches à grande échelle (les zones ne suivent pas des lignes droites). Chaque
// matière est une image (GroundTextures) ; sans images, une couleur unie avec un grain de bruit de près (couleur et
// petit relief, effacés au loin où ils scintilleraient).
[shader("fragment")]
float4 fragmentPlanet(PlanetVertexOutput input) : SV_Target
{
    const VertexOutput v = input.base;
    const DrawData draw = draws[v.draw];
    const PlanetData planet = frame[0].planets[asuint(draw.rim.w)];
    const float3 local = input.planetPosition;
    const float3 up = normalize(local);
    const float altitude = length(local) - planet.shape.x;
    // Fond sous plus de 30 m d'eau : l'eau le cache (opaque à 0,1 % près), inutile de l'éclairer. Seulement si la
    // caméra est au-dessus de la mer : de dessous, l'eau n'est pas dessinée.
    const float sea = planet.water.w;
    if (length(local) < sea - 30.0) {
        const float3 toCamera = float3(dot(v.worldPosition, draw.normal[0].xyz), dot(v.worldPosition, draw.normal[1].xyz),
                                       dot(v.worldPosition, draw.normal[2].xyz));
        if (length(local - toCamera) > sea) {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
    }
    float3 normal = normalize(v.normal);
    // Pente : la normale ramenée dans le repère de la planète (colonnes de sa rotation), contre la verticale.
    const float3 localNormal = float3(dot(normal, draw.normal[0].xyz), dot(normal, draw.normal[1].xyz), dot(normal, draw.normal[2].xyz));
    const float flatness = dot(localNormal, up);

    // Taches de 20 à 400 m (déplacent les limites des zones), grain de 30 cm à 3 m.
    // Chaque octave s'efface au loin, avant de scintiller (plus petite qu'un pixel).
    const float distance = length(v.worldPosition);
    const float macro = (PlanetNoise(local * (1.0 / 397.0)) - 0.5) * 0.5 * saturate(2.0 - distance / 40000.0)
                      + (PlanetNoise(local * (1.0 / 97.0)) - 0.5) * 0.3 * saturate(2.0 - distance / 10000.0)
                      + (PlanetNoise(local * (1.0 / 23.0)) - 0.5) * 0.2 * saturate(2.0 - distance / 2500.0);

    const float rock = smoothstep(0.84, 0.72, flatness + macro * 0.1);
    // Plage : jusqu'à beach.y mètres au-dessus de la mer (qu'il y ait un océan ou non), limite fondue sur un
    // cinquième de sa hauteur et déplacée par les taches de 30 % au plus : elle suit la côte sans faire de bande droite.
    const float relief = max(planet.shape.y, 1.0);
    const float beachHeight = max(planet.beach.y, 0.0);
    const float beachTop = planet.beach.x + beachHeight;
    const float beachBlur = min(max(0.2 * beachHeight, 0.3), 0.0015 * relief);
    const float beach = smoothstep(beachTop + beachBlur, beachTop - beachBlur, altitude + macro * 0.6 * beachHeight) * (1.0 - rock);

    // Climat des grandes échelles (par sommet), déplacé par les taches.
    const float temperature = input.climate.x + macro * 4.0;
    const float humidity = input.climate.y + macro * 0.3;

    // Biomes : terre nue entre désert et prairie ; sable là où il fait chaud et sec ; herbe là où c'est humide, avec
    // des taches de terre ; l'herbe jaunit dans le froid (toundra) ; neige quand il gèle, comme au-dessus de snowLevel.
    const float grassy = smoothstep(0.38, 0.55, humidity);
    const float desert = smoothstep(0.3, 0.15, humidity) * smoothstep(8.0, 18.0, temperature);
    const float patches = smoothstep(0.6, 0.72, PlanetNoise(local * (1.0 / 31.0) + 3.0) + macro * 0.3) * 0.85
                        * saturate(2.0 - distance / 4000.0);
    // Terre nue aux pieds des pentes, avant la roche.
    const float foot = smoothstep(0.93, 0.86, flatness + macro * 0.05) * (1.0 - beach) * 0.8;
    const float frozen = smoothstep(-2.0, -8.0, temperature);
    const float snow = max(smoothstep(planet.shape.z - 0.0375 * relief, planet.shape.z + 0.0375 * relief, altitude + macro * 0.19 * relief), frozen)
                     * smoothstep(0.5, 0.65, flatness);

    Surface surface;
    surface.toEye = normalize(-v.worldPosition);
    surface.f0 = float3(0.04);
    float occlusion = 1.0;
    if (planet.climate.w > 0.0) {
        float weights[GroundLayers] = { 0.0, 0.0, 1.0, 0.0, 0.0 };
        CoverGround(weights, GroundGrass, grassy * (1.0 - patches));
        CoverGround(weights, GroundSand, max(desert, beach));
        CoverGround(weights, GroundSoil, foot);
        CoverGround(weights, GroundRock, rock);
        CoverGround(weights, GroundSnow, snow);
        // Projections : pesées par l'orientation (puissance 4 : transitions courtes), sans celles de profil.
        float3 blend = pow(abs(localNormal), float3(4.0));
        blend /= blend.x + blend.y + blend.z;
        blend = saturate((blend - 0.05) / 0.95);
        blend /= blend.x + blend.y + blend.z;
        const float3 p = input.groundPosition;
        const float3 dpdx = ddx(p);
        const float3 dpdy = ddy(p);
        float3 color = float3(0.0);
        float3 groundNormal = float3(0.0);
        float2 occlusionRoughness = float2(0.0);
        float total = 0.0;
        for (uint layer = 0; layer < GroundLayers; ++layer) {
            const float weight = weights[layer];
            if (weight > 0.02) {
                GroundSample sample = SampleGroundScales(layer, p, dpdx, dpdy, localNormal, blend, distance);
                sample.color *= GroundTint(planet, layer);
                if (layer == GroundGrass) {
                    // L'herbe jaunit et roussit dans le froid (toundra) et par taches sèches.
                    const float cold = smoothstep(6.0, -4.0, temperature) * 0.7;
                    sample.color *= lerp(float3(1.0), float3(1.5, 1.2, 0.6), max(cold, saturate(macro * 1.6) * 0.4));
                }
                color += sample.color * weight;
                groundNormal += sample.normal * weight;
                occlusionRoughness += sample.occlusionRoughness * weight;
                total += weight;
            }
        }
        color /= max(total, 1.0e-4);
        occlusionRoughness /= max(total, 1.0e-4);
        // Taches de 20 à 400 m plus ou moins claires : la répétition des images se voit moins.
        color *= 0.9 + macro * 0.4;
        groundNormal = normalize(groundNormal);
        // Retour du repère de la planète au monde (colonnes de sa rotation).
        normal = normalize(draw.normal[0].xyz * groundNormal.x + draw.normal[1].xyz * groundNormal.y + draw.normal[2].xyz * groundNormal.z);
        surface.diffuse = color * params.color.rgb;
        surface.roughness = saturate(occlusionRoughness.y);
        occlusion = occlusionRoughness.x;
    } else {
        surface.diffuse = UntexturedGround(v, planet, local, normal, distance, macro, rock, beach, grassy, desert, patches,
                                           temperature, foot, snow, surface.roughness);
    }
    surface.normal = normal;
    float inside;
    const float3 lit = LightSurface(surface, normalize(v.normal), v.worldPosition, occlusion, inside);
    const float3 worldUp = normalize(draw.normal[0].xyz * up.x + draw.normal[1].xyz * up.y + draw.normal[2].xyz * up.z);
    const float3 rim = PlanetRim(planet, worldUp, normalize(-v.worldPosition));
    return float4(ToneMap(lit * input.airTransmittance + input.airScattered + rim), 1.0);
}

// Sol sans images : couleurs unies de la planète, avec un grain de bruit (couleur et petit relief) de près. Renvoie la
// couleur diffuse ; `normal` reçoit le petit relief.
float3 UntexturedGround(VertexOutput v, PlanetData planet, float3 local, inout float3 normal, float distance, float macro,
                        float rock, float beach, float grassy, float desert, float patches, float temperature, float foot,
                        float snow, out float roughness)
{
    const float near = saturate(1.0 - distance / 120.0);
    const float fine = PlanetNoise(local * (1.0 / 3.1)) * 0.55 + PlanetNoise(local * (1.0 / 0.93)) * 0.3 * near
                     + PlanetNoise(local * (1.0 / 0.29)) * 0.15 * saturate(1.0 - distance / 30.0) - 0.5;
    const float dry = saturate(macro * 1.6 + 0.5);
    float3 grassColor = planet.grass.rgb * lerp(float3(0.8, 0.85, 0.9), float3(1.35, 1.2, 0.85), dry);
    grassColor = lerp(grassColor, planet.soil.rgb * float3(1.6, 1.45, 1.1), smoothstep(6.0, -4.0, temperature) * 0.7);
    float4 ground = planet.soil * float4(float3(0.9 + macro * 0.4), 1.0);
    ground = lerp(ground, float4(grassColor, planet.grass.w), grassy * (1.0 - patches));
    ground = lerp(ground, planet.sand, max(desert, beach));
    ground = lerp(ground, planet.soil, foot);
    ground = lerp(ground, planet.rock * float4(float3(0.85 + macro * 0.5), 1.0), rock);
    ground = lerp(ground, planet.snow, snow);
    const float3 color = ground.rgb * (1.0 + fine * lerp(0.3, 0.5, rock) * saturate(1.0 - distance / 400.0));

    // Petit relief (bosses, cailloux) par les dérivées à l'écran du grain, plus marqué dans la roche.
    const float bump = fine * lerp(0.04, 0.12, rock) * (1.0 - snow * 0.7) * saturate(1.0 - distance / 200.0);
    const float3 dp1 = ddx(v.worldPosition);
    const float3 dp2 = ddy(v.worldPosition);
    const float3 r1 = cross(dp2, normal);
    const float3 r2 = cross(normal, dp1);
    const float determinant = dot(dp1, r1);
    if (abs(determinant) > 1.0e-12) {
        const float3 gradient = sign(determinant) * (ddx(bump) * r1 + ddy(bump) * r2);
        normal = normalize(abs(determinant) * normal - gradient);
    }
    roughness = saturate(ground.w);
    return color * params.color.rgb;
}

// Océan : les tuiles de la planète posées sur la sphère du niveau de la mer, le long de la verticale de chaque
// sommet. uv.x : profondeur de l'eau au sommet (négative sur la terre ferme, que le sol cache de toute façon).
[shader("vertex")]
PlanetVertexOutput vertexOcean(PlanetVertexInput input, uint drawIndex : SV_VulkanInstanceID)
{
    const DrawData draw = draws[drawIndex];
    const PlanetData planet = frame[0].planets[asuint(draw.rim.w)];
    const float morph = PlanetMorph(drawIndex, input.position);
    const float3 ground = input.position + input.morphOffset * morph;
    const float3 local = draw.rim.xyz + ground;
    const float3 up = normalize(local);
    const float depth = planet.water.w - length(local);
    PlanetVertexOutput output;
    output.base = Output(drawIndex, ground + up * depth, up, float2(depth, 0.0));
    output.planetPosition = up * planet.water.w;
    output.climate = float2(0.0); // l'eau n'en a pas besoin
    output.airScattered = AirTo(output.base.worldPosition, output.airTransmittance);
    output.seaSky = float3(0.0);
    output.seaSkyThrough = float3(1.0);
    const FrameData f = frame[0];
    if (HasAtmosphere(f.atmosphere)) {
        // Vu depuis l'eau : l'air devant le ciel de l'espace.
        const float3 worldUp = normalize(draw.normal[0].xyz * up.x + draw.normal[1].xyz * up.y + draw.normal[2].xyz * up.z);
        float3 reflected = reflect(normalize(output.base.worldPosition), worldUp);
        reflected = normalize(reflected + worldUp * max(0.02 - dot(reflected, worldUp), 0.0)); // pas sous l'horizon
        output.seaSky = AtmosphereScatterFrom(f.atmosphere, output.base.worldPosition - f.atmosphere.center.xyz, reflected,
                                              1.0e9, f.toSun.xyz, f.sunColor.rgb, 6, 0.5, output.seaSkyThrough);
    }
    return output;
}

// Pente du bruit (gradient), pour les vaguelettes.
float3 WaveSlope(float3 x)
{
    const float e = 0.15;
    return float3(PlanetNoise(x + float3(e, 0, 0)) - PlanetNoise(x - float3(e, 0, 0)),
                  PlanetNoise(x + float3(0, e, 0)) - PlanetNoise(x - float3(0, e, 0)),
                  PlanetNoise(x + float3(0, 0, e)) - PlanetNoise(x - float3(0, 0, e))) / (2.0 * e);
}

// Surface de l'océan : reflet du ciel et du soleil (de plus en plus fort en regardant de biais), couleur de l'eau au
// large ; là où elle est peu profonde, transparente (on voit le fond). Vaguelettes de près (pente d'un bruit qui
// glisse avec le temps), effacées au loin où elles scintilleraient.
[shader("fragment")]
float4 fragmentOcean(PlanetVertexOutput input) : SV_Target
{
    const VertexOutput v = input.base;
    const DrawData draw = draws[v.draw];
    const PlanetData planet = frame[0].planets[asuint(draw.rim.w)];
    const float depth = v.uv.x;
    if (depth <= 0.0) {
        discard;
    }
    const float3 local = input.planetPosition;
    const float3 up = normalize(local);
    const float distance = length(v.worldPosition);
    const float time = frame[0].cameraForward.w;
    float3 n = up;
    const float waves = saturate(1.0 - distance / 2000.0);
    if (waves > 0.0) {
        float3 slope = WaveSlope(local * (1.0 / 9.0) + float3(time * 0.21, 0.0, time * 0.13)) * 0.6
                     + WaveSlope(local * (1.0 / 2.3) + float3(-time * 0.37, time * 0.29, 0.0)) * 0.4;
        slope -= up * dot(slope, up);
        n = normalize(up - slope * (0.25 * waves));
    }
    const float3 normal = normalize(draw.normal[0].xyz * n.x + draw.normal[1].xyz * n.y + draw.normal[2].xyz * n.z);
    const float3 worldUp = normalize(draw.normal[0].xyz * up.x + draw.normal[1].xyz * up.y + draw.normal[2].xyz * up.z);
    const FrameData f = frame[0];
    Surface surface;
    surface.normal = normal;
    surface.toEye = normalize(-v.worldPosition);
    surface.diffuse = float3(0.0); // la couleur de l'eau vient de dessous (plus bas), pas de sa surface
    surface.f0 = float3(0.02);
    // De loin, les vagues qu'on ne dessine plus élargissent le reflet du soleil.
    surface.roughness = lerp(0.06, 0.22, saturate(distance / 20000.0));
    const float nv = saturate(dot(normal, surface.toEye));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - nv, 5.0);

    // Soleil : reflet (ombres des objets, rougi par l'air) et lumière qui entre dans l'eau.
    const float sunFacing = dot(worldUp, f.toSun.xyz);
    float3 sunLight = f.sunColor.rgb * (sunFacing > 0.0 ? SunVisibility(v.worldPosition, worldUp, sunFacing) : 0.0);
    // Ce qu'on voit en reflet : le ciel de l'atmosphère (depuis l'eau) devant le ciel de l'espace, sinon l'espace seul.
    float3 reflected = reflect(-surface.toEye, normal);
    reflected = normalize(reflected + worldUp * max(0.02 - dot(reflected, worldUp), 0.0)); // pas sous l'horizon
    float3 sky = environmentMap.SampleLevel(reflected, surface.roughness * f.environment.z).rgb * f.environment.x;
    float3 skyLight = float3(0.0);
    if (HasAtmosphere(f.atmosphere)) {
        const float3 center = v.worldPosition - f.atmosphere.center.xyz;
        sunLight *= AtmosphereSunTransmittance(f.atmosphere, center, f.toSun.xyz);
        sky = input.seaSky + sky * input.seaSkyThrough;
        skyLight = AtmosphereAmbient(f.atmosphere, center, worldUp, f.toSun.xyz);
    }
    // Sous la surface : la couleur de l'eau, éclairée par le soleil et le ciel.
    const float3 body = planet.water.rgb * (sunLight * saturate(sunFacing) + skyLight + f.environment.x * 0.05);
    const float3 lit = Shade(surface, f.toSun.xyz, sunLight) + sky * fresnel + body * (1.0 - fresnel)
                     + SceneLights(v.worldPosition, surface);

    // Opaque à partir de quelques mètres de fond ; le reflet compte d'autant plus qu'on regarde de biais (Fresnel).
    const float alpha = saturate(max(1.0 - exp(-depth / 4.0), fresnel));
    const float3 rim = PlanetRim(planet, worldUp, surface.toEye);
    return float4(ToneMap(lit * input.airTransmittance + input.airScattered + rim), alpha);
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

    float inside;
    float3 lit = LightSurface(surface, geometryNormal, input.worldPosition, occlusion, inside);
    if (any(params.emissive.rgb > 0.0)) { // le même pour toute la primitive : pas d'image lue sans émissif
        lit += params.emissive.rgb * emissiveTexture.Sample(input.uv).rgb;
    }
    // Lumière de contour : d'autant plus forte que la surface est vue de profil (terme de Fresnel), avec le relief.
    // Éteinte dans les volumes intérieurs : les murs du cockpit et de la soute ne s'allument pas.
    const float4 rim = draws[input.draw].rim;
    if (rim.w > 0.0 && inside < 1.0) {
        lit += rim.rgb * (pow(1.0 - saturate(dot(normal, surface.toEye)), rim.w) * (1.0 - inside));
    }
    lit = ThroughAir(lit, input.worldPosition);
    return float4(ToneMap(lit), params.transparent != 0 ? base.a : 1.0);
}

// Cartes d'ombres : la position vue du soleil (cascade params.cascade), et l'image seulement pour le seuil d'alpha.
struct ShadowOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

ShadowOutput ShadowVertex(uint draw, float3 position, float2 uv)
{
    const FrameData f = frame[0];
    const uint c = params.cascade;
    float4 m[4] = {f.cascades[c * 4 + 0], f.cascades[c * 4 + 1], f.cascades[c * 4 + 2], f.cascades[c * 4 + 3]};
    ShadowOutput output;
    output.position = Transform(m, Transform(draws[draw].world, position).xyz);
    output.uv = uv;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadow(VertexInput input, uint draw : SV_VulkanInstanceID)
{
    return ShadowVertex(draw, input.position, input.uv);
}

[shader("vertex")]
ShadowOutput vertexShadowSkinned(SkinnedVertexInput input, uint draw : SV_VulkanInstanceID)
{
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[draw].normal[1].w), position, normal);
    return ShadowVertex(draw, position, input.uv);
}

[shader("vertex")]
ShadowOutput vertexShadowPlanet(PlanetVertexInput input, uint draw : SV_VulkanInstanceID)
{
    return ShadowVertex(draw, input.position + input.morphOffset * PlanetMorph(draw, input.position), input.uv);
}

[shader("fragment")]
void fragmentShadow(ShadowOutput input)
{
    if (params.alphaCutoff > 0.0 && baseColorTexture.Sample(input.uv).a * params.color.a < params.alphaCutoff) {
        discard;
    }
}
