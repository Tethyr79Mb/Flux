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
#include "PlanetClimate.slang"

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
    float4 clouds;            // xyz : centre de la planète relatif à la caméra ; w : rayon de la couche de nuages (0 : aucune)
    float4 cloudRotation;     // rotation de la planète (quaternion xyzw)
    float4 cloudShape;        // x : couverture ; y : opacité ; z : 1 / taille des formations (1/m) ; w : angle de dérive
    float4 cloudColor;        // rgb : couleur linéaire ; w : épaisseur de la couche (m)
    // Forêts (VegetationComponent), vues comme une couleur du sol là où les arbres s'espacent puis s'arrêtent :
    // x : part couverte ; y : densité (0 : pas de forêts) ; z : cosinus de la pente maximale ; w : distance où la
    // couleur commence (m), entière au double.
    float4 forest;
    float4 forestColor;       // rgb : couleur linéaire de la cime des arbres
};

static const uint MaxPlanets = 4;

// Doit correspondre à FrameData dans MeshRenderer.cpp. Matrices par colonnes : aucune ambiguïté de convention.
struct FrameData
{
    float4 viewProjection[4];  // décalée d'un sous-pixel pour l'anticrénelage temporel (cameraRight.w, cameraUp.w)
    float4 previousViewProjection[4]; // de l'image précédente, sans décalage, relative à sa caméra
    float4 cascades[4 * 4];   // cascade c : colonnes [4c, 4c + 3] ; relatif caméra -> carte (xy dans [-1, 1], z dans [0, 1])
    float4 cascadeEnds;       // fin de chaque tranche, en mètres devant la caméra
    float4 cascadeTexels;     // côté d'un texel de chaque cascade, en mètres
    float4 cameraForward;     // xyz ; w : secondes écoulées (vagues), repartent de 0 toutes les heures
    float4 cameraDelta;       // xyz : caméra moins celle de l'image précédente (relatif caméra -> relatif à l'ancienne)
    float4 toSun;             // xyz : direction du soleil (unitaire)
    float4 sunColor;          // rgb : couleur x intensité
    // x : multiplie la texture du ciel ; y : niveau de mip de la lumière diffuse ; z : niveau de mip des reflets les
    // plus flous ; w : exposition (appliquée après le bloom, shaders/Post.slang : inutilisée ici)
    float4 environment;
    // Rayon d'un pixel (nuages) : cameraForward + cameraRight * clip.x - cameraUp * clip.y (à l'échelle du champ), le
    // clip du pixel moins le décalage d'un sous-pixel (w de cameraRight et de cameraUp).
    float4 cameraRight;
    float4 cameraUp;
    // x : nombre de lumières dans lights ; y : nombre de volumes intérieurs ; z : de planètes ; w : numéro de l'image
    // pour les bruits qui changent à chaque image (0 : fixes, sans anticrénelage temporel)
    uint4 lightCount;
    // Rotation maximale de l'image de chaque matière du sol (radians, GroundRock et suivantes : [0].xyzw puis [1].x) :
    // pi, tournée au hasard ; moins, toutes dans à peu près le même sens (rides du sable). Voir GroundTextures.hpp.
    float4 groundRotations[2];
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
    float4 previousWorld[4];  // world à l'image précédente, relative à sa caméra (tuiles de planète : non lue)
    // De l'entité (pas des tuiles de planète), combinés à ceux du matériau (DrawParams) : un même appel dessine des
    // entités de teintes différentes.
    float4 tint;              // multiplie la couleur du matériau
    float4 emissive;          // ajouté à celui du matériau ; w : 1 si surface remplace métal et rugosité
    float4 surface;           // x : métal, y : rugosité (multiplient ceux du matériau, ou les remplacent)
};

// Doit correspondre à DrawParams dans MeshRenderer.cpp : réglages de l'appel. Les modèles n'y lisent que cascade et
// transparent (leur matériau est dans la table, MaterialData) ; le sol et l'eau des planètes, tout.
struct DrawParams
{
    float4 color;             // linéaire : couleur de base
    float4 emissive;          // linéaire : lumière propre (rgb)
    uint cascade;             // carte d'ombres en cours de dessin
    float alphaCutoff;        // 0 : pas de seuil
    uint transparent;         // 1 : l'alpha sort vers le mélange ; 0 : opaque (alpha 1)
    float metallic;
    float roughness;
    float normalScale;        // 0 : pas de carte de normales
    float occlusionStrength;  // 0 : pas d'occlusion
    // Arbres des planètes (vertexTree) : matériau de la primitive, et entité neutre (teinte blanche, sans contour) dont
    // MeshColor lit les réglages.
    uint material;
    uint draw;
};

[[vk::push_constant]] ConstantBuffer<DrawParams> params;

// Doit correspondre à MaterialData dans ModelCache.cpp : une case de la table des matériaux de tous les modèles.
struct MaterialData
{
    float4 color;             // linéaire : couleur de base du matériau
    float4 emissive;          // linéaire : lumière propre du matériau (rgb), multipliée par l'image émissive
    float metallic;           // x canal bleu de l'image métal-rugosité
    float roughness;          // x canal vert
    float normalScale;        // 0 : pas de carte de normales
    float occlusionStrength;  // 0 : pas d'occlusion
    float alphaCutoff;        // 0 : pas de seuil
    // Cases de materialImages (0 : blanche) : couleur de base, émissive, métal-rugosité (vert : rugosité, bleu : métal),
    // normales (repère tangent, comme glTF), occlusion (rouge).
    uint baseColorImage;
    uint emissiveImage;
    uint metallicRoughnessImage;
    uint normalImage;
    uint occlusionImage;
    uint2 padding;
};

// Groupe 0 : tuiles de planète avec textures du sol (GroundTextures.hpp), une couche par matière (GroundLayer). Images
// blanches pour les autres maillages, qui ne les lisent pas.
[[vk::binding(5, 0)]] Sampler2DArray groundColorTexture;  // alpha : rugosité
[[vk::binding(6, 0)]] Sampler2DArray groundNormalTexture; // rouge-vert : repère tangent (OpenGL : vert vers le haut), bleu : occlusion

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
// Entités des lots de modèles, lot après lot : l'instance i d'un appel dessine l'entité instances[i].x avec le
// matériau instances[i].y (les appels des tuiles de planète donnent directement l'indice dans draws).
[[vk::binding(6, 1)]] StructuredBuffer<uint2> instances;
// Arbres des planètes retenus pour l'image (shaders/Vegetation.slang) : deux float4 chacun, position relative à la
// caméra et taille, puis rotation (quaternion).
[[vk::binding(7, 1)]] StructuredBuffer<float4> treeInstances;

// Groupe 2 : tables de tous les modèles chargés (ModelCache). Images des matériaux, choisies par indice : couleurs en
// sRGB (le GPU les ramène en linéaire à la lecture), données (métal-rugosité, normales, occlusion) telles quelles.
[[vk::binding(0, 2)]] Sampler2D materialImages[];
[[vk::binding(1, 2)]] StructuredBuffer<MaterialData> materials;

// Un même appel dessine des matériaux différents : l'indice peut changer d'un pixel voisin à l'autre.
float4 SampleMaterial(uint image, float2 uv)
{
    return materialImages[NonUniformResourceIndex(image)].Sample(uv);
}

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

// Doit correspondre à MorphVertex dans Flux/Planetology/Planet.hpp : où le sommet d'une tuile de planète arrive
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
    nointerpolation uint material : MATERIAL; // dans materials (modèles seulement)
    // Le point à l'image précédente (clip space, sans décalage), là où était l'entité et là où il serait resté
    // immobile : leur écart est le mouvement propre de l'entité (voir Motion). Nuls pour les tuiles de planète.
    float4 previousClip : PREVIOUS_CLIP;
    float4 staticClip   : STATIC_CLIP;
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
    output.material = 0;
    output.worldPosition = Transform(draw.world, position).xyz;
    output.position = Transform(frame[0].viewProjection, output.worldPosition);
    output.normal = draw.normal[0].xyz * normal.x + draw.normal[1].xyz * normal.y + draw.normal[2].xyz * normal.z;
    output.uv = uv;
    output.previousClip = float4(0.0);
    output.staticClip = float4(0.0);
    return output;
}

// Output, plus où était le sommet à l'image précédente (entités seulement : les planètes ne bougent pas). La pose de
// l'image précédente n'est pas gardée : le mouvement des os d'un modèle animé n'est pas compté, seulement celui de
// l'entité.
VertexOutput MovingOutput(uint drawIndex, float3 position, float3 normal, float2 uv)
{
    VertexOutput output = Output(drawIndex, position, normal, uv);
    const FrameData f = frame[0];
    output.previousClip = Transform(f.previousViewProjection, Transform(draws[drawIndex].previousWorld, position).xyz);
    output.staticClip = Transform(f.previousViewProjection, output.worldPosition + f.cameraDelta.xyz);
    return output;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instance : SV_VulkanInstanceID)
{
    const uint2 entry = instances[instance];
    VertexOutput output = MovingOutput(entry.x, input.position, input.normal, input.uv);
    output.material = entry.y;
    return output;
}

[shader("vertex")]
VertexOutput vertexSkinned(SkinnedVertexInput input, uint instance : SV_VulkanInstanceID)
{
    const uint2 entry = instances[instance];
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[entry.x].normal[1].w), position, normal);
    VertexOutput output = MovingOutput(entry.x, position, normal, input.uv);
    output.material = entry.y;
    return output;
}

// Sommet d'un arbre des planètes (VegetationRenderer) : tourné et agrandi selon son arbre, et balancé par le vent
// d'autant plus qu'il est haut (rien au pied du tronc). Les arbres sont immobiles : leur mouvement propre est nul.
float3 TreeRotate(float4 q, float3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float3 TreePosition(float4 placement, float3 position, out float4 rotation, uint instance)
{
    rotation = treeInstances[instance * 2 + 1];
    // Phase propre à chaque arbre (sa taille est tirée au hasard) : la forêt ne bat pas en mesure.
    const float phase = frac(placement.w * 97.31) * 6.2831853;
    const float time = frame[0].cameraForward.w;
    const float bend = max(position.y, 0.0) * 0.08;
    const float2 sway = float2(sin(time * 1.1 + phase), sin(time * 0.9 + phase * 1.7)) * 0.12
                      + float2(sin(time * 2.7 + phase * 2.3), sin(time * 3.1 + phase)) * 0.03;
    position.xz += sway * (bend * bend);
    return placement.xyz + TreeRotate(rotation, position * placement.w);
}

// Arbres : l'air entre la caméra et le sommet est calculé par sommet (comme les tuiles de planète), pas par pixel :
// le feuillage à seuil d'alpha se recouvre beaucoup, et ses 16 pas de marche dans l'air par pixel coûtaient plus que
// tout le reste de l'arbre.
struct TreeVertexOutput
{
    VertexOutput base;
    float3 airTransmittance : AIR_TRANSMITTANCE;
    float3 airScattered : AIR_SCATTERED;
};

[shader("vertex")]
TreeVertexOutput vertexTree(VertexInput input, uint instance : SV_VulkanInstanceID)
{
    const float4 placement = treeInstances[instance * 2];
    float4 rotation;
    TreeVertexOutput tree;
    VertexOutput output;
    output.draw = params.draw;
    output.material = params.material;
    output.worldPosition = TreePosition(placement, input.position, rotation, instance);
    output.position = Transform(frame[0].viewProjection, output.worldPosition);
    output.normal = TreeRotate(rotation, input.normal);
    output.uv = input.uv;
    const FrameData f = frame[0];
    output.previousClip = Transform(f.previousViewProjection, output.worldPosition + f.cameraDelta.xyz);
    output.staticClip = output.previousClip;
    tree.base = output;
    tree.airScattered = AirTo(output.worldPosition, tree.airTransmittance);
    return tree;
}

// Glissement du sommet vers la forme parente : 0 près de la caméra, 1 au-delà de la distance où la parente prend
// le relais (calculé sur le sommet de la tuile, pour que deux tuiles voisines glissent pareil au même endroit).
float PlanetMorph(uint drawIndex, float3 position)
{
    const DrawData draw = draws[drawIndex];
    const float distance = length(Transform(draw.world, position).xyz);
    return saturate((distance - draw.normal[0].w) * draw.normal[2].w);
}

// Ce que les tuiles de planète gardent de VertexOutput : ni matériau ni points de l'image précédente (le sol ne
// bouge pas, son mouvement est nul), ni coordonnées de texture, qui seraient des sorties inutilisées par
// fragmentPlanet (la validation de Vulkan les signale).
struct PlanetSurface
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float3 normal   : NORMAL;
    nointerpolation uint draw : DRAW; // dans draws
};

PlanetSurface ToPlanetSurface(VertexOutput full)
{
    PlanetSurface surface;
    surface.position = full.position;
    surface.worldPosition = full.worldPosition;
    surface.normal = full.normal;
    surface.draw = full.draw;
    return surface;
}

// Pour l'océan : la profondeur de l'eau (uv.x) au lieu de la normale, que fragmentOcean recalcule.
struct OceanSurface
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float2 uv       : TEXCOORD;
    nointerpolation uint draw : DRAW; // dans draws
};

OceanSurface ToOceanSurface(VertexOutput full)
{
    OceanSurface surface;
    surface.position = full.position;
    surface.worldPosition = full.worldPosition;
    surface.uv = full.uv;
    surface.draw = full.draw;
    return surface;
}

struct PlanetVertexOutput
{
    PlanetSurface base;
    float3 planetPosition : PLANET; // dans le repère de la planète (son centre à l'origine)
    // Le même modulo GroundPeriod : coordonnées des textures du sol, précises même loin du centre de la planète.
    float3 groundPosition : GROUND;
    // Sol : température (°C) et humidité (0 à 1) sans les taches de moins de 400 m (voir PlanetClimate).
    float2 climate : CLIMATE;
    // Forêt (0 à 1) selon ses taches et le climat, sans la pente ni l'altitude (vues au pixel). Par sommet : ses
    // taches font des centaines de mètres.
    float forest : FOREST;
    // L'air entre la caméra et le sommet (voir AirTo) : calculé par sommet, pas par pixel. Il varie doucement avec la
    // distance, et les tuiles gardent à peu près la même taille à l'écran : la différence ne se voit pas, et le pixel
    // n'a plus ses 16 pas de marche dans l'air.
    float3 airTransmittance : AIR_TRANSMITTANCE;
    float3 airScattered : AIR_SCATTERED;
};

// Ce que l'océan garde des tuiles de planète : ni normale, ni position du sol, ni climat, ni forêt, que fragmentOcean
// ne lit pas (des sorties inutilisées, signalées par la validation de Vulkan).
struct OceanVertexOutput
{
    OceanSurface base;
    float3 planetPosition : PLANET;
    float3 airTransmittance : AIR_TRANSMITTANCE;
    float3 airScattered : AIR_SCATTERED;
    // Le ciel de l'atmosphère que reflète une mer plate (sans les vaguelettes), et la part du ciel de l'espace qui le
    // traverse. Par sommet, comme l'air.
    float3 seaSky : SEA_SKY;
    float3 seaSkyThrough : SEA_SKY_THROUGH;
};

float3 AirTo(float3 worldPosition, out float3 transmittance);
float CloudShadow(float3 worldPosition);

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
    return ClimateAt(planet.shape.x, planet.shape.y, planet.climate.xyz, planet.water.w, local);
}

[shader("vertex")]
PlanetVertexOutput vertexPlanet(PlanetVertexInput input, uint draw : SV_VulkanInstanceID)
{
    const float morph = PlanetMorph(draw, input.position);
    const float3 position = input.position + input.morphOffset * morph;
    PlanetVertexOutput output;
    output.base = ToPlanetSurface(Output(draw, position, lerp(input.normal, input.morphNormal, morph), float2(0.0)));
    output.planetPosition = draws[draw].rim.xyz + position;
    output.groundPosition = draws[draw].ground.xyz + position;
    const PlanetData planet = frame[0].planets[asuint(draws[draw].rim.w)];
    output.climate = PlanetClimate(planet, output.planetPosition);
    output.forest = 0.0;
    if (planet.forest.y > 0.0) {
        // Les sommets s'espacent avec la distance (quelques pixels à l'écran) : les taches de 150 m puis de 600 m
        // s'effacent avant de tomber entre deux sommets.
        const float distance = length(output.base.worldPosition);
        const float2 blur = saturate(float2(distance / 3000.0, distance / 12000.0) - 1.0);
        output.forest = ForestAt(normalize(output.planetPosition) * planet.shape.x, planet.forest.x, blur) * TreeClimate(output.climate);
    }
    output.airScattered = AirTo(output.base.worldPosition, output.airTransmittance);
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
float3 PerturbNormal(MaterialData material, float3 normal, float3 worldPosition, float2 uv)
{
    float3 mapped = SampleMaterial(material.normalImage, uv).xyz * 2.0 - 1.0;
    mapped.xy *= material.normalScale;
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

// Couleur écrite dans l'image HDR de la scène : la lumière linéaire, sans exposition ni courbe de tons (appliquées
// après le bloom, shaders/Post.slang), bornée à ce que tient un float 16 bits.
float3 Radiance(float3 color)
{
    return min(color, float3(60000.0));
}

// Lumière renvoyée vers l'œil par une surface : soleil (ombres, rougi par l'air), ambiante du ciel ou du volume
// intérieur, lumières de la scène. inside : poids du volume intérieur où elle se trouve.
float3 LightSurface(Surface surface, float3 geometryNormal, float3 worldPosition, float occlusion, out float inside)
{
    const FrameData f = frame[0];
    const float3 toSun = f.toSun.xyz;
    // Ombre lue avec la vraie face (le relief de la carte ne déplace pas les ombres).
    const float sunFacing = dot(geometryNormal, toSun);
    const float sun = sunFacing > 0.0 ? SunVisibility(worldPosition, geometryNormal, sunFacing) * CloudShadow(worldPosition) : 0.0;
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

// --- Nuages des planètes (CloudsComponent) : une couche épaisse entre deux sphères autour du centre de la planète. ---

// v ramené dans le repère de la rotation q (quaternion xyzw) : q* v q.
float3 RotateInverse(float4 q, float3 v)
{
    const float3 t = 2.0 * cross(-q.xyz, v);
    return v + q.w * t + cross(-q.xyz, t);
}

// Entrée et sortie d'une sphère de centre `center` (relatif à l'origine du rayon) : x > y si manquée. Distance au
// centre par la perpendiculaire au rayon : plus précis que b² - c quand la sphère est loin.
float2 RaySphere(float3 center, float3 direction, float radius)
{
    const float b = dot(center, direction);
    const float3 closest = center - direction * b;
    const float discriminant = radius * radius - dot(closest, closest);
    if (discriminant < 0.0) {
        return float2(1.0, -1.0);
    }
    const float root = sqrt(discriminant);
    return float2(b - root, b + root);
}

// Direction (repère de la planète, unitaire) entraînée par la dérive : la couche tourne autour de l'axe y.
float3 CloudDrift(PlanetData planet, float3 direction)
{
    const float c = cos(planet.cloudShape.w);
    const float s = sin(planet.cloudShape.w);
    return float3(c * direction.x + s * direction.z, direction.y, c * direction.z - s * direction.x);
}

// Carte de couverture : où sont les nuages, vu d'au-dessus. Positive dans un nuage (d'autant plus qu'on va vers son
// cœur), négative dans le ciel dégagé. p : la direction (dérivée) en tailles de formation. Des régions deux fois plus
// grandes séparent ciel dégagé et ciel couvert ; le domaine est tordu (`warp`) pour des bords en volutes plutôt qu'en
// taches rondes. `octaves` : 1 à 4, la dernière en partie ; une octave effacée vaut sa moyenne.
// `minimum` : en dessous, l'appelant n'en fait rien (densité nulle). Dès que les octaves restantes, même toutes à 1,
// ne peuvent plus l'atteindre, on rend -1 sans les lire : la plupart des pas en ciel dégagé s'arrêtent après un ou
// deux bruits au lieu de cinq, pour la même image.
float CloudCoverage(PlanetData planet, float3 p, float3 warp, float octaves, float minimum)
{
    p += warp;
    // Comparé avant la division par la somme des amplitudes (0,96875), avec une marge contre les arrondis.
    const float needed = (minimum + lerp(0.72, 0.28, planet.cloudShape.x)) * 0.96875 - 1.0e-4;
    float value = PlanetNoise(p * 0.5 + 31.0) * 0.5;
    // Le plus que peuvent encore ajouter les octaves pas encore lues : une octave effacée n'ajoute que sa moyenne.
    float remaining = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        remaining += 0.25 / float(1 << i) * (0.5 + 0.5 * saturate(octaves - float(i)));
    }
    float amplitude = 0.25;
    float frequency = 1.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        if (value + remaining <= needed) {
            return -1.0;
        }
        const float weight = saturate(octaves - float(i));
        const float noise = weight > 0.0 ? PlanetNoise(p * frequency + float(i) * 7.31) : 0.5;
        value += lerp(0.5, noise, weight) * amplitude;
        remaining -= amplitude * (0.5 + 0.5 * weight);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    value /= 0.96875; // somme des amplitudes : de 0 à 1
    return value - lerp(0.72, 0.28, planet.cloudShape.x);
}

// Couverture sous laquelle CloudShape est nulle à la hauteur h : au-dessus du quart de la couche, seul un cœur assez
// épais monte jusque-là (top = 0,25 + 0,75 · cover / 0,14).
float CloudMinimumCover(float h)
{
    return max(h - 0.25, 0.0) * (0.14 / 0.75);
}

// Torsion du domaine, une fois par pixel : les volutes à l'échelle des formations.
float3 CloudWarp(float3 p)
{
    return (float3(PlanetNoise(p * 0.7 + 3.1), PlanetNoise(p * 0.7 + 11.7), PlanetNoise(p * 0.7 + 23.3)) - 0.5) * 0.9;
}

// Densité (0 à 1) à la hauteur relative h (0 : base de la couche, 1 : son sommet) pour une couverture `cover` : base
// plate, sommet en dôme, d'autant plus haut que le nuage est épais (un cumulus monte en son centre).
float CloudShape(float cover, float h)
{
    const float core = saturate(cover / 0.14);
    const float top = 0.25 + 0.75 * core;
    return smoothstep(0.0, 0.08, h) * saturate((top - h) / 0.3) * core;
}

// Bruit « en chou-fleur » (bosses arrondies) qui ronge les bords d'un nuage : 0 à 1.
float CloudBillow(float3 q)
{
    const float a = abs(PlanetNoise(q) * 2.0 - 1.0);
    const float b = abs(PlanetNoise(q * 2.3 + 5.1) * 2.0 - 1.0);
    return a * 0.65 + b * 0.35;
}

// Part du soleil qui passe à travers les nuages jusqu'à ce point (1 : aucun nuage entre les deux). La couverture seule
// au tiers de la couche, peu d'octaves : une ombre de nuage est floue.
float CloudShadow(float3 worldPosition)
{
    const FrameData f = frame[0];
    float light = 1.0;
    for (uint i = 0; i < f.lightCount.z; ++i) {
        const PlanetData planet = f.planets[i];
        if (planet.clouds.w <= 0.0) {
            continue;
        }
        const float3 origin = worldPosition - planet.clouds.xyz;
        const float radius = planet.clouds.w + planet.cloudColor.w * 0.3;
        const float2 hit = AtmosphereRaySphere(origin, f.toSun.xyz, radius);
        const float t = hit.x > 0.0 ? hit.x : hit.y; // sous la couche : la sortie ; au-dessus : l'entrée
        if (hit.x > hit.y || t <= 0.0) {
            continue;
        }
        const float3 direction = CloudDrift(planet, RotateInverse(planet.cloudRotation, normalize(origin + f.toSun.xyz * t)));
        const float3 p = direction * (planet.clouds.w * planet.cloudShape.z);
        const float density = CloudShape(CloudCoverage(planet, p, float3(0.0), 2.0, CloudMinimumCover(0.3)), 0.3);
        light *= 1.0 - (1.0 - exp(-density * 4.0)) * planet.cloudShape.y * 0.85;
    }
    return light;
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

// Une version de l'image (décalage et rotation tirés de `variant`, rotation dans [-maxAngle, maxAngle]) ; normale
// ramenée dans le repère du plan de projection (rotation inverse).
void SampleGroundVariant(out float3 color, out float3 tangentNormal, out float2 occlusionRoughness, uint layer,
                         float2 uv, float2 gx, float2 gy, uint variant, float maxAngle)
{
    const float2 random = GroundRandom(variant * 0x68e31da4U + 17u);
    const float angle = (random.x * 2.0 - 1.0) * maxAngle;
    const float c = cos(angle);
    const float s = sin(angle);
    const float2x2 rotation = float2x2(c, -s, s, c);
    const float3 at = float3(mul(rotation, uv) + GroundRandom(variant * 0xb5297a4dU + 71u), float(layer));
    const float2 rgx = mul(rotation, gx);
    const float2 rgy = mul(rotation, gy);
    const float4 colorRoughness = groundColorTexture.SampleGrad(at, rgx, rgy);
    const float3 normalOcclusion = groundNormalTexture.SampleGrad(at, rgx, rgy).xyz;
    color = colorRoughness.rgb;
    occlusionRoughness = float2(normalOcclusion.z, colorRoughness.a);
    const float2 xy = normalOcclusion.xy * 2.0 - 1.0;
    tangentNormal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    tangentNormal.xy = mul(tangentNormal.xy, rotation); // transposée : rotation inverse
}

// Une matière dans un plan de projection, sans répétition visible : les sommets du triangle du point (voir
// GroundTriangleSide), une ou deux lectures selon les poids. Le plus faible des trois est retiré des autres : il ne
// compte jamais, et le passage d'un sommet à l'autre reste continu.
void SampleGroundNoRepeat(out float3 color, out float3 tangentNormal, out float2 occlusionRoughness, uint layer,
                          float2 uv, float2 gx, float2 gy, int period, float maxAngle)
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
    w = max(w - min(w.x, min(w.y, w.z)), 0.0) + 1.0e-6; // au centre exact, les trois
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
            SampleGroundVariant(c, n, surface, layer, uv, gx, gy, uint(u) * 0x9e3779b1U ^ GroundHash(uint(b) + 101u),
                                maxAngle);
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
    const float maxAngle = frame[0].groundRotations[layer / 4][layer % 4];
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
            SampleGroundNoRepeat(color, t, occlusionRoughness, layer, uv, gx, gy, period, maxAngle);
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

// La matière à deux échelles, fondues entre 60 et 150 m de la caméra, puis sa couleur moyenne (plus petit mip, sol
// lisse) au-delà de GroundAverageEnd : de haut, les cellules du pavage, chacune avec son propre tirage de l'image,
// dessinaient un motif régulier de points sombres. Les taches (macro) gardent le sol varié ; moins de lectures au loin.
static const float GroundAverageStart = 600.0;
static const float GroundAverageEnd = 2000.0;

GroundSample SampleGroundScales(uint layer, float3 p, float3 dpdx, float3 dpdy, float3 n, float3 blend, float distance)
{
    GroundSample average;
    const float3 center = float3(0.5, 0.5, float(layer));
    const float4 colorRoughness = groundColorTexture.SampleLevel(center, 16.0);
    average.color = colorRoughness.rgb;
    average.occlusionRoughness = float2(groundNormalTexture.SampleLevel(center, 16.0).z, colorRoughness.a);
    average.normal = n;
    const float faded = smoothstep(GroundAverageStart, GroundAverageEnd, distance);
    if (faded >= 1.0) {
        return average;
    }
    const float far = smoothstep(60.0, 150.0, distance);
    GroundSample result = SampleGround(layer, p, dpdx, dpdy, n, blend, far < 1.0 ? GroundNearScale : GroundFarScale);
    if (far > 0.0 && far < 1.0) {
        const GroundSample other = SampleGround(layer, p, dpdx, dpdy, n, blend, GroundFarScale);
        result.color = lerp(result.color, other.color, far);
        result.normal = lerp(result.normal, other.normal, far);
        result.occlusionRoughness = lerp(result.occlusionRoughness, other.occlusionRoughness, far);
    }
    if (faded > 0.0) {
        result.color = lerp(result.color, average.color, faded);
        result.normal = lerp(result.normal, average.normal, faded);
        result.occlusionRoughness = lerp(result.occlusionRoughness, average.occlusionRoughness, faded);
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
// Couleur de la scène, et mouvement propre de ce qu'on voit (l'anticrénelage temporel y ajoute celui de la caméra).
struct SceneOutput
{
    float4 color  : SV_Target0;
    float2 motion : SV_Target1;
};

// Mouvement propre à l'écran (en fraction de l'image, comme les uv) : où le point était à l'image précédente, moins
// où il aurait été s'il n'avait pas bougé dans le monde. Nul pour tout ce qui est immobile, cockpit autour de la
// caméra compris : il bouge avec elle, son mouvement propre annule celui de la caméra.
float2 Motion(VertexOutput input)
{
    if (input.previousClip.w <= 0.0 || input.staticClip.w <= 0.0) {
        return float2(0.0); // derrière l'ancienne caméra : rien de mieux que la reprojection par la profondeur
    }
    return (input.previousClip.xy / input.previousClip.w - input.staticClip.xy / input.staticClip.w) * 0.5;
}

[shader("fragment")]
SceneOutput fragmentPlanet(PlanetVertexOutput input)
{
    SceneOutput output;
    output.color = PlanetColor(input);
    output.motion = float2(0.0);
    return output;
}

float4 PlanetColor(PlanetVertexOutput input)
{
    const PlanetSurface v = input.base;
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
        // Projections : pesées par l'orientation (puissance 4 : transitions courtes), deux au plus (la plus faible
        // est retirée des autres, comme les sommets du pavage), sans celles de profil.
        float3 blend = pow(abs(localNormal), float3(4.0));
        blend = max(blend - min(blend.x, min(blend.y, blend.z)), 0.0) + 1.0e-6;
        blend /= blend.x + blend.y + blend.z;
        blend = saturate((blend - 0.05) / 0.95);
        blend /= blend.x + blend.y + blend.z;
        const float3 p = input.groundPosition;
        const float3 dpdx = ddx(p);
        const float3 dpdy = ddy(p);
        float3 color = float3(0.0);
        float3 groundNormal = float3(0.0);
        float2 occlusionRoughness = float2(0.0);
        // Les deux matières les plus présentes seulement, moins la troisième : elle passe devant la deuxième sans
        // saut, et un pixel lit deux matières au plus au lieu de cinq.
        // Poids gardés à part du tableau : pas d'accès au tableau par un indice variable (mémoire lente du GPU).
        uint first = 0;
        uint second = 1;
        float firstWeight = weights[0];
        float secondWeight = weights[1];
        if (secondWeight > firstWeight) {
            first = 1;
            second = 0;
            firstWeight = weights[1];
            secondWeight = weights[0];
        }
        float third = 0.0;
        [unroll]
        for (uint layer = 2; layer < GroundLayers; ++layer) {
            const float weight = weights[layer];
            if (weight > firstWeight) {
                third = secondWeight;
                second = first;
                secondWeight = firstWeight;
                first = layer;
                firstWeight = weight;
            } else if (weight > secondWeight) {
                third = secondWeight;
                second = layer;
                secondWeight = weight;
            } else {
                third = max(third, weight);
            }
        }
        float total = 0.0;
        for (uint k = 0; k < 2; ++k) {
            const uint layer = k == 0 ? first : second;
            // La première toujours : trois matières à égalité ne laissent rien aux deux premières.
            const float weight = k == 0 ? max(firstWeight - third, 1.0e-3) : secondWeight - third;
            if (k == 0 || weight > 0.02) {
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
    // Forêts au loin : la cime des arbres là où ils s'espacent (VegetationRenderer garde moins d'arbres au loin) puis
    // s'arrêtent. Mêmes limites que computeTrees : pente, plage, neige, roche.
    if (planet.forest.y > 0.0 && input.forest > 0.0) {
        const float treeLine = planet.shape.z - 0.1 * relief;
        const float canopy = input.forest * saturate(planet.forest.y * 1.5) * smoothstep(planet.forest.w, planet.forest.w * 2.0, distance)
                           * smoothstep(planet.forest.z - 0.03, planet.forest.z + 0.03, flatness)
                           * smoothstep(treeLine, treeLine - 0.02 * relief, altitude) * (1.0 - beach) * (1.0 - snow) * (1.0 - rock);
        surface.diffuse = lerp(surface.diffuse, planet.forestColor.rgb, canopy);
        surface.roughness = lerp(surface.roughness, 0.8, canopy);
    }
    surface.normal = normal;
    float inside;
    const float3 lit = LightSurface(surface, normalize(v.normal), v.worldPosition, occlusion, inside);
    const float3 worldUp = normalize(draw.normal[0].xyz * up.x + draw.normal[1].xyz * up.y + draw.normal[2].xyz * up.z);
    const float3 rim = PlanetRim(planet, worldUp, normalize(-v.worldPosition));
    return float4(Radiance(lit * input.airTransmittance + input.airScattered + rim), 1.0);
}

// Sol sans images : couleurs unies de la planète, avec un grain de bruit (couleur et petit relief) de près. Renvoie la
// couleur diffuse ; `normal` reçoit le petit relief.
float3 UntexturedGround(PlanetSurface v, PlanetData planet, float3 local, inout float3 normal, float distance, float macro,
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
OceanVertexOutput vertexOcean(PlanetVertexInput input, uint drawIndex : SV_VulkanInstanceID)
{
    const DrawData draw = draws[drawIndex];
    const PlanetData planet = frame[0].planets[asuint(draw.rim.w)];
    const float morph = PlanetMorph(drawIndex, input.position);
    const float3 ground = input.position + input.morphOffset * morph;
    const float3 local = draw.rim.xyz + ground;
    const float3 up = normalize(local);
    const float depth = planet.water.w - length(local);
    OceanVertexOutput output;
    output.base = ToOceanSurface(Output(drawIndex, ground + up * depth, up, float2(depth, 0.0)));
    output.planetPosition = up * planet.water.w;
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
float4 fragmentOcean(OceanVertexOutput input) : SV_Target
{
    const OceanSurface v = input.base;
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
    float3 sunLight = f.sunColor.rgb
                    * (sunFacing > 0.0 ? SunVisibility(v.worldPosition, worldUp, sunFacing) * CloudShadow(v.worldPosition) : 0.0);
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
    return float4(Radiance(lit * input.airTransmittance + input.airScattered + rim), alpha);
}

// Matériau à seuil d'alpha (feuillage, grillage) et transparents : les pixels sous le seuil sont jetés.
[shader("fragment")]
SceneOutput fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace)
{
    SceneOutput output;
    output.color = MeshColor(input, frontFacing, true);
    output.motion = Motion(input);
    return output;
}

// Matériau opaque sans seuil : aucun pixel jeté. Un programme qui peut jeter des pixels oblige le GPU à attendre sa
// fin pour tester la profondeur ; sans, il écarte avant tout calcul les pixels cachés par un objet plus proche.
[shader("fragment")]
SceneOutput fragmentOpaque(VertexOutput input, bool frontFacing : SV_IsFrontFace)
{
    SceneOutput output;
    output.color = MeshColor(input, frontFacing, false);
    output.motion = Motion(input);
    return output;
}

// Feuillage des arbres (vertexTree) : ses normales pointent vers l'extérieur de la couronne des deux côtés d'une carte
// (éclairage doux, en volume) ; les retourner sur la face arrière assombrirait une moitié de l'arbre.
[shader("fragment")]
SceneOutput fragmentTree(TreeVertexOutput input)
{
    SceneOutput output;
    output.color = MeshColor(input.base, true, true, true, input.airTransmittance, input.airScattered);
    output.motion = Motion(input.base);
    return output;
}

// Écorce des arbres : opaque, sans seuil.
[shader("fragment")]
SceneOutput fragmentTreeOpaque(TreeVertexOutput input, bool frontFacing : SV_IsFrontFace)
{
    SceneOutput output;
    output.color = MeshColor(input.base, frontFacing, false, true, input.airTransmittance, input.airScattered);
    output.motion = Motion(input.base);
    return output;
}

// alphaTest et airGiven connus à chaque appel : les branches disparaissent, et avec elles le discard.
// airGiven : l'air jusqu'au point est déjà calculé (par sommet), sinon il l'est ici.
[ForceInline]
float4 MeshColor(VertexOutput input, bool frontFacing, bool alphaTest, bool airGiven = false,
                 float3 airTransmittance = float3(1.0), float3 airScattered = float3(0.0))
{
    const DrawData draw = draws[input.draw];
    const MaterialData material = materials[input.material];
    float4 base = SampleMaterial(material.baseColorImage, input.uv) * material.color * draw.tint;
    if (alphaTest && base.a < material.alphaCutoff) {
        discard;
    }
    // Face arrière d'un matériau double face : éclairée comme vue de son côté.
    const float3 geometryNormal = normalize(input.normal) * (frontFacing ? 1.0 : -1.0);
    float3 normal = geometryNormal;
    if (material.normalScale != 0.0) { // le même pour toute la primitive
        normal = PerturbNormal(material, geometryNormal, input.worldPosition, input.uv);
    }
    const float4 metallicRoughness = SampleMaterial(material.metallicRoughnessImage, input.uv);
    // Métal et rugosité de l'entité : à la place de ceux du matériau et de son image (ModelComponent), ou en facteur.
    const bool fixed = draw.emissive.w != 0.0;
    const float metallic = saturate(fixed ? draw.surface.x : material.metallic * draw.surface.x * metallicRoughness.b);
    Surface surface;
    surface.normal = normal;
    surface.toEye = normalize(-input.worldPosition); // la caméra est à l'origine
    surface.diffuse = base.rgb * (1.0 - metallic);
    surface.f0 = lerp(float3(0.04), base.rgb, metallic);
    surface.roughness = saturate(fixed ? draw.surface.y : material.roughness * draw.surface.y * metallicRoughness.g);
    const float occlusion = 1.0 + material.occlusionStrength * (SampleMaterial(material.occlusionImage, input.uv).r - 1.0);

    float inside;
    float3 lit = LightSurface(surface, geometryNormal, input.worldPosition, occlusion, inside);
    const float3 emissive = material.emissive.rgb + draw.emissive.rgb;
    if (any(emissive > 0.0)) { // le même pour toute l'entité : pas d'image lue sans émissif
        lit += emissive * SampleMaterial(material.emissiveImage, input.uv).rgb;
    }
    // Lumière de contour : d'autant plus forte que la surface est vue de profil (terme de Fresnel), avec le relief.
    // Éteinte dans les volumes intérieurs : les murs du cockpit et de la soute ne s'allument pas.
    const float4 rim = draw.rim;
    if (rim.w > 0.0 && inside < 1.0) {
        lit += rim.rgb * (pow(1.0 - saturate(dot(normal, surface.toEye)), rim.w) * (1.0 - inside));
    }
    lit = airGiven ? lit * airTransmittance + airScattered : ThroughAir(lit, input.worldPosition);
    return float4(Radiance(lit), params.transparent != 0 ? base.a : 1.0);
}

// Cartes d'ombres : la position vue du soleil (cascade params.cascade), et l'image seulement pour le seuil d'alpha.
struct ShadowOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    nointerpolation uint material : MATERIAL;
};

ShadowOutput ShadowVertex(uint draw, float3 position, float2 uv)
{
    const FrameData f = frame[0];
    const uint c = params.cascade;
    float4 m[4] = {f.cascades[c * 4 + 0], f.cascades[c * 4 + 1], f.cascades[c * 4 + 2], f.cascades[c * 4 + 3]};
    ShadowOutput output;
    output.position = Transform(m, Transform(draws[draw].world, position).xyz);
    output.uv = uv;
    output.material = 0;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadow(VertexInput input, uint instance : SV_VulkanInstanceID)
{
    const uint2 entry = instances[instance];
    ShadowOutput output = ShadowVertex(entry.x, input.position, input.uv);
    output.material = entry.y;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadowSkinned(SkinnedVertexInput input, uint instance : SV_VulkanInstanceID)
{
    const uint2 entry = instances[instance];
    float3 position;
    float3 normal;
    Skin(input, asuint(draws[entry.x].normal[1].w), position, normal);
    ShadowOutput output = ShadowVertex(entry.x, position, input.uv);
    output.material = entry.y;
    return output;
}

[shader("vertex")]
ShadowOutput vertexTreeShadow(VertexInput input, uint instance : SV_VulkanInstanceID)
{
    const FrameData f = frame[0];
    const uint c = params.cascade;
    float4 m[4] = {f.cascades[c * 4 + 0], f.cascades[c * 4 + 1], f.cascades[c * 4 + 2], f.cascades[c * 4 + 3]};
    float4 rotation;
    ShadowOutput output;
    output.position = Transform(m, TreePosition(treeInstances[instance * 2], input.position, rotation, instance));
    output.uv = input.uv;
    output.material = params.material;
    return output;
}

[shader("vertex")]
ShadowOutput vertexShadowPlanet(PlanetVertexInput input, uint draw : SV_VulkanInstanceID)
{
    return ShadowVertex(draw, input.position + input.morphOffset * PlanetMorph(draw, input.position), input.uv);
}

[shader("fragment")]
void fragmentShadow(ShadowOutput input)
{
    const MaterialData material = materials[input.material];
    if (material.alphaCutoff > 0.0 && SampleMaterial(material.baseColorImage, input.uv).a * material.color.a < material.alphaCutoff) {
        discard;
    }
}

// Nuages des planètes : un triangle plein écran. Chaque pixel cherche la couche de nuages la plus proche le long de
// son rayon et la traverse en quelques pas (4 à 8) : à chaque pas, la densité (carte de couverture, forme en dôme,
// bords rongés), la lumière du soleil qui lui arrive (ombre du nuage sur lui-même, lue juste au-dessus vers le
// soleil), sa diffusion vers l'œil (plus forte en contre-jour : bords argentés) et la lumière du ciel. La profondeur
// écrite (SV_Depth) est celle du premier nuage touché : le sol, les montagnes et les vaisseaux devant la couche la
// cachent (test de profondeur). Coût : une dizaine de bruits par pas et par pixel couvert, moins au loin.
struct CloudVertexOutput
{
    float4 position : SV_Position;
    float2 clip     : TEXCOORD; // -1 à 1, y vers le bas comme le clip space de Vulkan
};

[shader("vertex")]
CloudVertexOutput vertexClouds(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    // Le triangle est placé à la profondeur du nuage le plus proche possible : la distance jusqu'à la couche, ramenée
    // au plan de vue par le cosinus du coin de l'écran (le pixel le plus de biais). Le fragment n'écrit jamais plus
    // près (SV_DepthLessEqual) : le GPU peut écarter d'avance les pixels cachés par le relief ou un vaisseau plus
    // proche, sans lancer le shader. Dans la couche : distance nulle, rien n'est écarté.
    const FrameData f = frame[0];
    float nearest = 3.0e38;
    for (uint i = 0; i < f.lightCount.z; ++i) {
        const PlanetData planet = f.planets[i];
        if (planet.clouds.w <= 0.0) {
            continue;
        }
        const float distance = length(planet.clouds.xyz);
        const float top = planet.clouds.w + max(planet.cloudColor.w, 1.0);
        nearest = min(nearest, max(max(distance - top, planet.clouds.w - distance), 0.0));
    }
    const float viewDepth = nearest / length(f.cameraForward.xyz + abs(f.cameraRight.xyz) + abs(f.cameraUp.xyz));
    float depth = 1.0;
    if (viewDepth > 1.0e-3 && viewDepth < 1.0e30) {
        const float4 plane = Transform(f.viewProjection, f.cameraForward.xyz * viewDepth);
        depth = saturate(plane.z / plane.w);
    }
    CloudVertexOutput output;
    output.position = float4(clip, depth, 1.0);
    output.clip = clip;
    return output;
}

struct CloudOutput
{
    float4 color : SV_Target;
    float depth  : SV_DepthLessEqual; // jamais plus près que le triangle : rejet anticipé permis (vertexClouds)
};

// Angle d'un pixel (radians), à peu près : 60° sur un millier de lignes. Règle les octaves au loin.
static const float CloudPixelAngle = 0.0015;
// Longueur traversée au plus (m) : de biais, la couche peut faire des dizaines de km ; au-delà, l'air voile tout.
static const float CloudMaxPath = 40000.0;

// Henyey-Greenstein, rapportée à une diffusion égale dans toutes les directions (1).
float CloudPhase(float g, float mu)
{
    const float d = 1.0 + g * g - 2.0 * g * mu;
    return (1.0 - g * g) / (d * sqrt(d));
}

[shader("fragment")]
CloudOutput fragmentClouds(CloudVertexOutput input)
{
    const FrameData f = frame[0];
    const float2 pixel = input.clip - float2(f.cameraRight.w, f.cameraUp.w); // sans le décalage d'un sous-pixel
    const float3 direction = normalize(f.cameraForward.xyz + f.cameraRight.xyz * pixel.x - f.cameraUp.xyz * pixel.y);
    // La couche la plus proche sur le rayon : de son entrée à sa sortie, sans traverser la sphère de sa base.
    float start = 3.0e38;
    float end = 0.0;
    uint index = MaxPlanets;
    for (uint i = 0; i < f.lightCount.z; ++i) {
        const PlanetData candidate = f.planets[i];
        if (candidate.clouds.w <= 0.0) {
            continue;
        }
        const float2 outer = RaySphere(candidate.clouds.xyz, direction, candidate.clouds.w + candidate.cloudColor.w);
        if (outer.x > outer.y || outer.y <= 0.0) {
            continue;
        }
        const float2 inner = RaySphere(candidate.clouds.xyz, direction, candidate.clouds.w);
        float from = max(outer.x, 0.0);
        float to = outer.y;
        if (inner.x <= inner.y && inner.y > 0.0) {
            if (inner.x > 0.0) {
                to = inner.x;            // au-dessus de la base : jusqu'à elle
            } else {
                from = max(from, inner.y); // sous la base : à partir de sa sortie
            }
        }
        // La planète arrête le rayon : sans elle, là où rien n'a écrit de profondeur (eau transparente, tuile pas
        // encore prête), le rayon qui plonge traverserait la planète et montrerait les nuages de l'autre côté.
        const float solid = candidate.water.w > 0.0 ? candidate.water.w : candidate.shape.x - candidate.shape.y;
        const float2 ground = RaySphere(candidate.clouds.xyz, direction, solid);
        if (ground.x <= ground.y && ground.x > 0.0) {
            to = min(to, ground.x);
        }
        if (to > from && from < start) {
            start = from;
            end = to;
            index = i;
        }
    }
    if (index >= MaxPlanets) {
        discard;
    }
    const PlanetData planet = f.planets[index];
    end = min(end, start + CloudMaxPath);
    const float baseRadius = planet.clouds.w;
    const float thickness = max(planet.cloudColor.w, 1.0);
    const float scale = planet.cloudShape.z; // 1 / taille des formations
    const float size = 1.0 / scale;

    // Moins de pas et d'octaves au loin (vue de l'espace : la couche n'est qu'une pellicule).
    // De près (moins de 45 km), la couche est traversée en 8 pas ; de loin, ses pas dessineraient des courbes de niveau
    // (une strate par pas) : chaque point n'est plus qu'une colonne, sa densité intégrée sur la hauteur. Fondu de
    // l'un à l'autre entre 45 et 60 km.
    const float near = saturate(1.0 - start / 60000.0);
    const float marched = saturate(near * 4.0);
    const int steps = marched > 0.0 ? 8 : 0;
    const float octaves = clamp(log2(size / max(4.0 * start * CloudPixelAngle, 1.0)) + 1.0, 1.0, 4.0);
    // Bords rongés de près seulement : leur bruit le plus fin (size / 14) doit couvrir 4 pixels, sinon il scintille.
    const float erosion = saturate(log2(size / (14.0 * max(4.0 * start * CloudPixelAngle, 1.0))));
    const float stepLength = (end - start) / float(steps);
    // Départ décalé au hasard par pixel, de près : du grain fin au lieu de strates. De loin, la couche est trop mince
    // pour faire des strates et ce grain se verrait : au milieu des pas. Avec l'anticrénelage temporel, le décalage
    // avance d'un nombre d'or à chaque image : la moyenne des images efface le grain.
    const float shift = float(f.lightCount.w % 1024u) * 0.61803398875;
    const float jitter = lerp(0.5, frac(PlanetHash(float3(input.position.xy, 0.0)) + shift), near);
    const float3 midDirection = CloudDrift(planet, RotateInverse(planet.cloudRotation,
                                                                 normalize(direction * (0.5 * (start + end)) - planet.clouds.xyz)));
    const float3 warp = CloudWarp(midDirection * (baseRadius * scale));

    const float3 toSun = f.toSun.xyz;
    const float mu = dot(direction, toSun);
    const float phase = lerp(CloudPhase(-0.15, mu), CloudPhase(0.65, mu), 0.45);
    const bool air = HasAtmosphere(f.atmosphere)
                   && dot(f.atmosphere.center.xyz - planet.clouds.xyz, f.atmosphere.center.xyz - planet.clouds.xyz)
                          < 1.0e-6 * dot(planet.clouds.xyz, planet.clouds.xyz) + 1.0;
    const float3 sky = environmentMap.SampleLevel(normalize(direction * start - planet.clouds.xyz), f.environment.y).rgb
                     * f.environment.x;

    float transmittance = 1.0;
    float3 light = float3(0.0);
    float firstHit = -1.0;
    for (int s = 0; s < steps; ++s) {
        const float t = start + (float(s) + jitter) * stepLength;
        const float3 fromCenter = direction * t - planet.clouds.xyz;
        const float radius = length(fromCenter);
        const float h = (radius - baseRadius) / thickness;
        const float3 up = fromCenter / radius;
        const float3 p = CloudDrift(planet, RotateInverse(planet.cloudRotation, up)) * (baseRadius * scale);
        const float cover = CloudCoverage(planet, p, warp, octaves, CloudMinimumCover(h));
        if (cover <= 0.0) {
            continue;
        }
        float density = CloudShape(cover, h);
        if (density <= 0.0) {
            continue;
        }
        if (erosion > 0.0) {
            // Bords rongés en bosses arrondies, en 3D (la hauteur compte) : du relief qui bouge avec la caméra.
            const float3 q = (p + warp) * 6.0 + up * (h * thickness * scale * 6.0);
            const float bite = CloudBillow(q) * 0.55 * erosion;
            density = saturate((density - bite) / (1.0 - bite));
        }
        density *= planet.cloudShape.y;
        if (density <= 0.001) {
            continue;
        }
        // Le soleil qui arrive ici : la planète le cache la nuit, l'air le rougit, le nuage au-dessus (vers le soleil)
        // l'arrête. Lumière du ciel : plus forte au sommet du nuage qu'à sa base.
        const float sunFacing = dot(up, toSun);
        float3 sunLight = f.sunColor.rgb * smoothstep(-0.08, 0.04, sunFacing);
        float3 ambient = sky;
        if (air) {
            sunLight = f.sunColor.rgb * AtmosphereSunTransmittance(f.atmosphere, fromCenter, toSun);
            ambient = AtmosphereAmbient(f.atmosphere, fromCenter, up, toSun) + sky * AtmosphereNightSky(f.atmosphere, fromCenter, toSun);
        }
        const float above = CloudShape(cover, h + 0.35 * max(sunFacing, 0.2)) * planet.cloudShape.y;
        const float selfShadow = exp(-above * 3.0) * (1.0 - exp(-density * 6.0) * 0.4); // et un peu plus clair au cœur (« powder »)
        const float3 lit = planet.cloudColor.rgb * 0.9 * (sunLight * selfShadow * phase + ambient * (0.45 + 0.55 * saturate(h)));
        // Extinction : un nuage plein (densité 1) arrête presque toute la lumière en une demi-épaisseur.
        const float stepTransmittance = exp(-density * 6.0 / thickness * stepLength);
        light += transmittance * (1.0 - stepTransmittance) * lit;
        transmittance *= stepTransmittance;
        if (firstHit < 0.0) {
            firstHit = t;
        }
        if (transmittance < 0.02) {
            break;
        }
    }
    if (marched < 1.0) {
        // Colonne au milieu de la traversée : le nuage d'autant plus opaque que le regard le traverse de biais.
        const float t = 0.5 * (start + end);
        const float3 fromCenter = direction * t - planet.clouds.xyz;
        const float3 up = normalize(fromCenter);
        const float3 p = CloudDrift(planet, RotateInverse(planet.cloudRotation, up)) * (baseRadius * scale);
        const float cover = CloudCoverage(planet, p, warp, octaves, 0.0);
        float columnAlpha = 0.0;
        float3 columnLight = float3(0.0);
        if (cover > 0.0) {
            const float core = saturate(cover / 0.14);
            const float column = core * (0.12 + 0.6 * core); // ∫ CloudShape dh, à peu près
            const float slant = clamp((end - start) / thickness, 1.0, 6.0);
            columnAlpha = 1.0 - exp(-column * 6.0 * slant * planet.cloudShape.y);
            const float sunFacing = dot(up, toSun);
            float3 sunLight = f.sunColor.rgb * smoothstep(-0.08, 0.04, sunFacing);
            float3 ambient = sky;
            if (air) {
                sunLight = f.sunColor.rgb * AtmosphereSunTransmittance(f.atmosphere, fromCenter, toSun);
                ambient = AtmosphereAmbient(f.atmosphere, fromCenter, up, toSun) + sky * AtmosphereNightSky(f.atmosphere, fromCenter, toSun);
            }
            columnLight = planet.cloudColor.rgb * 0.9 * (sunLight * (1.0 - 0.3 * core) * phase + ambient * 0.8) * columnAlpha;
        }
        light = lerp(columnLight, light, marched);
        transmittance = lerp(1.0 - columnAlpha, transmittance, marched);
    }
    const float alpha = 1.0 - transmittance;
    if (alpha < 0.004) {
        discard;
    }
    const float hit = firstHit > 0.0 ? firstHit : 0.5 * (start + end);
    // L'air entre la caméra et le nuage.
    float3 airTransmittance = float3(1.0);
    float3 scattered = float3(0.0);
    if (air) {
        scattered = AtmosphereScatter(f.atmosphere, direction, hit, toSun, f.sunColor.rgb, 8, 0.5, airTransmittance);
    }
    const float3 color = light / alpha; // non prémultiplié : le mélange multiplie par alpha
    const float3 up = normalize(direction * hit - planet.clouds.xyz);
    CloudOutput output;
    output.color = float4(Radiance(color * airTransmittance + scattered + PlanetRim(planet, up, -direction)), alpha);
    const float4 clip = Transform(f.viewProjection, direction * hit);
    output.depth = min(clip.z / clip.w, input.position.z);
    return output;
}
