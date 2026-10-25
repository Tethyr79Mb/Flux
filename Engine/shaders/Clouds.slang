// Nuages volumétriques des planètes (Planetology::CloudRenderer), en trois passes plein écran après la scène opaque :
// 1. fragmentCloudMarch, à la moitié de la résolution : chaque pixel traverse la couche de nuages la plus proche le
//    long de son rayon, jusqu'au relief ou au vaisseau qui l'arrête (profondeur de la scène). À chaque pas, la densité
//    vient des textures 3D de bruit (CloudNoise.slang) : la carte météo dit où sont les formations et leur type
//    (stratus plats, cumulus en tours), la texture de forme y sculpte des masses arrondies, celle de détail en ronge
//    les bords. Le soleil qui arrive à chaque point est suivi en 6 pas vers lui (l'ombre du nuage sur lui-même, ce qui
//    donne le volume), avec une diffusion multiple approchée (le cœur d'un nuage épais reste lumineux, les bords à
//    contre-jour s'argentent).
// 2. fragmentCloudResolve : mêlé à l'historique (la même chose à l'image précédente, retrouvée là où était le nuage),
//    10 % de nouveau : le départ de chaque rayon change à chaque image et la moyenne efface le grain, comme si chaque
//    pixel avait fait dix fois plus de pas. Ramené dans les couleurs du voisinage pour ne pas laisser de traînées.
// 3. fragmentCloudComposite, à pleine résolution : mélangé à la scène, chaque pixel prenant les pixels voisins de la
//    demi-résolution dont la profondeur ressemble à la sienne (pas de halo autour des montagnes), et rien devant un
//    objet plus proche que le premier nuage.
// Coût : surtout la passe 1, une poignée de lectures de texture par pas, pas éclairés seulement là où il y a du nuage ;
// au quart du nombre de pixels de l'image.

#include "Mesh.slang"

// Groupe 3 : ce que lisent les passes des nuages (chaque passe ne lit que les siennes).
[[vk::binding(0, 3)]] Sampler3D cloudShapeNoise;  // forme 128³ (CloudNoise.slang) : r Perlin-Worley, gba Worley
[[vk::binding(1, 3)]] Sampler3D cloudDetailNoise; // détail 32³ : rgb Worley
[[vk::binding(2, 3)]] Sampler2D cloudSceneDepth;  // profondeur de la scène, pleine résolution (reverse-Z)
[[vk::binding(3, 3)]] Sampler2D cloudCurrent;     // passe 1 : lumière (prémultipliée), alpha
[[vk::binding(4, 3)]] Sampler2D cloudDistances;   // passe 1 : x premier nuage, y nuage moyen, z fin du rayon (m)
[[vk::binding(5, 3)]] Sampler2D cloudHistory;     // passe 2 : le résultat d'avant ; passe 3 : celui de cette image

// Les réglages arrivent par DrawParams (Mesh.slang, une seule constante poussée par shader) :
// color.xy : 1 / taille de la demi-résolution ; color.z : 1 si l'historique est valable ; color.w : part de l'image
// actuelle ; emissive.xy : taille de la pleine résolution ; emissive.zw : de la demi-résolution (pixels).
float2 CloudHalfTexel() { return params.color.xy; }
float2 CloudFullSize() { return params.emissive.xy; }
float2 CloudHalfSize() { return params.emissive.zw; }

// Échelles des textures, en tailles de formation (CloudsComponent::size) : la forme se répète tous les demi-formations
// (des masses d'un huitième de formation), le détail tous les seizièmes.
static const float CloudShapeRepeat = 2.0;
static const float CloudDetailRepeat = 16.0;
// Valeur moyenne des masses de la texture de forme (Perlin-Worley creusé par le Worley), mesurée sur toute la texture.
static const float CloudShapeMean = 0.75;
// Extinction d'un nuage de densité 1 (par mètre) : un cumulus de 1 km arrête toute lumière, sa frange est diaphane.
static const float CloudExtinction = 0.03;
// Longueur traversée au plus (m) : de biais, la couche peut faire des centaines de km ; au-delà, l'air voile tout.
static const float CloudMaxPath = 60000.0;
static const int CloudSteps = 64;
static const int CloudLightSteps = 6;
// Distance qui veut dire « rien » (le ciel, pas de nuage) : finie, pour les comparaisons.
static const float CloudFar = 1.0e9;

struct CloudVertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD; // 0 à 1, origine en haut à gauche comme les textures
};

[shader("vertex")]
CloudVertexOutput vertexCloudPass(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    CloudVertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.uv = clip * 0.5 + 0.5;
    return output;
}

// Rayon (unitaire, relatif à la caméra) d'un point de l'image, sans le décalage d'un sous-pixel de l'anticrénelage.
float3 CloudRay(FrameData f, float2 uv)
{
    const float2 clip = uv * 2.0 - 1.0; // y vers le bas, comme le clip space de Vulkan
    return normalize(f.cameraForward.xyz + f.cameraRight.xyz * clip.x - f.cameraUp.xyz * clip.y);
}

// Distance le long du rayon `direction` jusqu'à une profondeur de la scène (reverse-Z infini : profondeur = proche /
// profondeur de vue) ; CloudFar pour le ciel.
float CloudDepthToDistance(FrameData f, float3 direction, float depth)
{
    if (depth <= 0.0) {
        return CloudFar;
    }
    const float3 forward = normalize(f.cameraForward.xyz);
    const float4 unit = Transform(f.viewProjection, forward); // un point à 1 m devant : sa profondeur vaut « proche »
    const float near = unit.z / unit.w;
    return near / depth / max(dot(direction, forward), 1.0e-4);
}

// Henyey-Greenstein, rapportée à une diffusion égale dans toutes les directions (1).
float CloudPhase(float g, float mu)
{
    const float d = 1.0 + g * g - 2.0 * g * mu;
    return (1.0 - g * g) / (d * sqrt(d));
}

float CloudRemap(float value, float from, float to, float newFrom, float newTo)
{
    return newFrom + (value - from) / (to - from) * (newTo - newFrom);
}

// La couche de nuages la plus proche sur le rayon : de son entrée à sa sortie, sans traverser la sphère de sa base ni
// la planète. false : aucune.
bool CloudLayer(FrameData f, float3 direction, out uint index, out float start, out float end)
{
    start = 3.0e38;
    end = 0.0;
    index = MaxPlanets;
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
                to = inner.x;              // au-dessus de la base : jusqu'à elle
            } else {
                from = max(from, inner.y); // sous la base : à partir de sa sortie
            }
        }
        // La planète arrête le rayon, même là où rien n'a écrit de profondeur (eau transparente, tuile pas prête).
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
    return index < MaxPlanets;
}

// Ce qui ne change pas d'un pas à l'autre d'un même rayon.
struct CloudRayContext
{
    PlanetData planet;
    float baseRadius;
    float thickness;
    float scale;   // 1 / taille des formations (1/m)
    float3 warp;   // torsion de la carte météo (CloudWarp), au milieu de la traversée
    float type;    // 0 : stratus (couche basse et plate) ; 1 : cumulus (tours)
};

// Densité (0 à 1) au point `fromCenter` (relatif au centre de la planète). `lod` : niveau de mip de la forme (celui
// du détail en découle) ; `octaves` de la carte météo ; `detail` : bords rongés (pas les rayons vers le soleil).
float CloudDensity(CloudRayContext c, float3 fromCenter, float lod, float octaves, bool detail)
{
    const float radius = length(fromCenter);
    const float h = (radius - c.baseRadius) / c.thickness;
    if (h <= 0.0 || h >= 1.0) {
        return 0.0;
    }
    const float3 local = CloudDrift(c.planet, RotateInverse(c.planet.cloudRotation, fromCenter / radius));
    const float weather = CloudWeather(c.planet, local * (c.baseRadius * c.scale), c.warp, octaves);
    if (weather <= 0.0) {
        return 0.0;
    }
    // Profil en hauteur : base plate ; sommet arrondi, d'autant plus haut que le nuage est un cumulus et que la météo
    // y est forte (une tour monte au cœur d'une formation).
    const float top = lerp(0.18, 1.0, c.type) * lerp(0.55, 1.0, weather);
    const float profile = smoothstep(0.0, 0.06, h) * (1.0 - smoothstep(top * 0.55, top, h));
    if (profile <= 0.0) {
        return 0.0;
    }
    const float3 q = local * (radius * c.scale * CloudShapeRepeat);
    const float4 shape = cloudShapeNoise.SampleLevel(q, lod);
    const float worley = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    // Au loin, les masses sont plus petites qu'un pixel : leur moyenne au lieu des derniers mips, dont les quelques
    // texels interpolés dessineraient des bords en escalier (vu de l'espace).
    const float masses = lerp(saturate(CloudRemap(shape.r, worley - 1.0, 1.0, 0.0, 1.0)), CloudShapeMean, saturate(lod - 2.5));
    float density = masses * profile;
    // Peu de couverture : seuls les sommets du bruit dépassent, en petits nuages épars ; beaucoup : tout se rejoint.
    density = saturate(CloudRemap(density, 1.0 - weather, 1.0, 0.0, 1.0)) * weather;
    const float erosion = saturate(2.5 - lod); // le détail est 8 fois plus fin que la forme
    if (density <= 0.0 || !detail || erosion <= 0.0) {
        return density;
    }
    // Bords rongés : effilochés en bas, en bosses arrondies en haut.
    const float3 d = cloudDetailNoise.SampleLevel(q * (CloudDetailRepeat / CloudShapeRepeat), lod).rgb;
    const float fine = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    const float bite = lerp(fine, 1.0 - fine, saturate(h * 4.0)) * 0.45 * erosion;
    return saturate(CloudRemap(density, bite, 1.0, 0.0, 1.0));
}

struct CloudMarchOutput
{
    float4 light     : SV_Target0; // rgb : lumière (prémultipliée) ; a : opacité
    float4 distances : SV_Target1; // x : premier nuage ; y : nuage moyen ; z : fin du rayon (m)
};

[shader("fragment")]
CloudMarchOutput fragmentCloudMarch(CloudVertexOutput input)
{
    const FrameData f = frame[0];
    const float3 direction = CloudRay(f, input.uv);

    // Le rayon s'arrête au plus loin des 4 pixels pleins sous ce pixel : la composition écarte ensuite le nuage devant
    // les pixels plus proches qui ne le voient pas (voir fragmentCloudComposite).
    const int2 full = int2(input.position.xy) * 2;
    const int2 last = int2(CloudFullSize()) - 1;
    float depth = 1.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        depth = min(depth, cloudSceneDepth.Load(int3(min(full + int2(i & 1, i >> 1), last), 0)).r);
    }
    const float geometry = CloudDepthToDistance(f, direction, depth);

    CloudMarchOutput output;
    output.light = float4(0.0);
    output.distances = float4(CloudFar, CloudFar, geometry, 0.0);
    uint index;
    float start;
    float end;
    if (!CloudLayer(f, direction, index, start, end)) {
        return output;
    }
    end = min(min(end, geometry), start + CloudMaxPath);
    if (end <= start) {
        return output;
    }

    CloudRayContext c;
    c.planet = f.planets[index];
    c.baseRadius = c.planet.clouds.w;
    c.thickness = max(c.planet.cloudColor.w, 1.0);
    c.scale = c.planet.cloudShape.z;
    const float3 middle = CloudDrift(c.planet, RotateInverse(c.planet.cloudRotation,
                                                             normalize(direction * (0.5 * (start + end)) - c.planet.clouds.xyz)));
    const float3 p = middle * (c.baseRadius * c.scale);
    c.warp = CloudWarp(p);
    // Peu de couverture : surtout des cumulus ; ciel couvert : surtout des stratus.
    c.type = saturate(PlanetNoise(p * 0.35 + 57.0) * 1.6 - 0.3 + (0.5 - c.planet.cloudShape.x) * 0.8);

    // Niveau de mip selon la taille d'un pixel (demi-résolution) au premier nuage : sans lui, la texture répétée au loin
    // scintillerait. Côté d'un texel de la forme : un demi de formation sur 128.
    const float pixel = length(f.cameraRight.xyz) * 2.0 / CloudHalfSize().x;
    const float texel = 1.0 / (c.scale * CloudShapeRepeat * 128.0);
    const float lod = max(log2(max(start, 1.0) * pixel / texel), 0.0);
    // Carte météo : moins d'octaves au loin, où ses détails sont plus petits qu'un pixel.
    const float octaves = clamp(log2(1.0 / (c.scale * max(4.0 * start * pixel, 1.0))) + 1.0, 1.0, 4.0);

    // Pas plus serrés près de l'entrée quand la caméra est dans la couche ou juste à côté (on y voit les détails) :
    // t = début + longueur x u^k, k de 1 (loin) à 2 (dedans). Départ décalé au hasard par pixel et par image (nombre
    // d'or) : du grain au lieu de strates, effacé par l'historique.
    const float span = end - start;
    const float k = lerp(1.0, 2.0, saturate(1.0 - start / c.thickness));
    const float shift = float(f.lightCount.w % 1024u) * 0.61803398875;
    const float jitter = frac(PlanetHash(float3(input.position.xy, 1.0)) + shift);
    const float3 toSun = f.toSun.xyz;
    const float mu = dot(direction, toSun);
    const bool air = HasAtmosphere(f.atmosphere)
                   && dot(f.atmosphere.center.xyz - c.planet.clouds.xyz, f.atmosphere.center.xyz - c.planet.clouds.xyz)
                          < 1.0e-6 * dot(c.planet.clouds.xyz, c.planet.clouds.xyz) + 1.0;
    const float3 sky = environmentMap.SampleLevel(normalize(direction * start - c.planet.clouds.xyz), f.environment.y).rgb
                     * f.environment.x;
    const float opacity = c.planet.cloudShape.y;

    float transmittance = 1.0;
    float3 light = float3(0.0);
    float firstHit = CloudFar;
    float weighted = 0.0;
    float weights = 0.0;
    float previous = start;
    for (int s = 0; s < CloudSteps; ++s) {
        const float u = (float(s) + jitter) / float(CloudSteps);
        const float t = start + span * pow(u, k);
        const float stepLength = t - previous;
        previous = t;
        const float3 fromCenter = direction * t - c.planet.clouds.xyz;
        const float density = CloudDensity(c, fromCenter, lod, octaves, true) * opacity;
        if (density <= 0.002) {
            continue;
        }
        const float sigma = density * CloudExtinction;
        const float3 up = normalize(fromCenter);
        const float h = saturate((length(fromCenter) - c.baseRadius) / c.thickness);

        // Le soleil jusqu'ici : 6 pas vers lui, de plus en plus longs, sur deux épaisseurs de couche au plus.
        float depthToSun = 0.0;
        float lightStep = c.thickness * 0.03;
        float along = 0.0;
        for (int l = 0; l < CloudLightSteps; ++l) {
            along += lightStep * 0.5;
            depthToSun += CloudDensity(c, fromCenter + toSun * along, lod + 1.0, 1.0, false) * lightStep;
            along += lightStep * 0.5;
            lightStep *= 1.8;
        }
        const float opticalDepth = depthToSun * opacity * CloudExtinction;

        // Le soleil qui arrive au-dessus du nuage : la planète le cache la nuit, l'air le rougit.
        const float sunFacing = dot(up, toSun);
        float3 sunLight = f.sunColor.rgb * smoothstep(-0.08, 0.04, sunFacing);
        float3 ambient = sky;
        if (air) {
            sunLight = f.sunColor.rgb * AtmosphereSunTransmittance(f.atmosphere, fromCenter, toSun);
            ambient = AtmosphereAmbient(f.atmosphere, fromCenter, up, toSun) + sky * AtmosphereNightSky(f.atmosphere, fromCenter, toSun);
        }
        // Diffusion multiple approchée (Wrenninge) : trois ordres, chacun moins atténué et moins orienté que le précédent.
        float scattered = 0.0;
        float weight = 1.0;
        float extinction = 1.0;
        float anisotropy = 1.0;
        [unroll]
        for (int o = 0; o < 3; ++o) {
            const float phase = lerp(CloudPhase(-0.2 * anisotropy, mu), CloudPhase(0.8 * anisotropy, mu), 0.5);
            scattered += weight * phase * exp(-opticalDepth * extinction);
            weight *= 0.5;
            extinction *= 0.4;
            anisotropy *= 0.5;
        }
        // Bords tournés vers le soleil un peu plus sombres (« powder ») : la lumière n'y a pas encore assez rebondi.
        const float powder = lerp(1.0, 1.0 - exp(-sigma * 120.0), saturate(0.5 - 0.5 * mu));
        const float3 source = c.planet.cloudColor.rgb * (sunLight * scattered * powder + ambient * lerp(0.35, 1.0, h));

        // Intégrée sur le pas (Hillaire) : juste même pour un pas long dans un nuage dense.
        const float stepTransmittance = exp(-sigma * stepLength);
        const float absorbed = transmittance * (1.0 - stepTransmittance);
        light += absorbed * source;
        weighted += absorbed * t;
        weights += absorbed;
        transmittance *= stepTransmittance;
        if (firstHit >= CloudFar) {
            firstHit = t;
        }
        if (transmittance < 0.01) {
            break;
        }
    }
    const float alpha = 1.0 - transmittance;
    if (alpha < 0.002) {
        return output;
    }
    const float hit = weights > 0.0 ? weighted / weights : firstHit;
    // L'air entre la caméra et le nuage, et la lumière de contour de la planète.
    float3 airTransmittance = float3(1.0);
    float3 airLight = float3(0.0);
    if (air) {
        airLight = AirTo(direction * hit, airTransmittance);
    }
    const float3 up = normalize(direction * hit - c.planet.clouds.xyz);
    output.light = float4(Radiance(light * airTransmittance + (airLight + PlanetRim(c.planet, up, -direction)) * alpha), alpha);
    output.distances = float4(firstHit, hit, geometry, 0.0);
    return output;
}

// Couleurs HDR ramenées entre 0 et 1 avant les moyennes du voisinage (comme Temporal.slang) : un bord éclairé par le
// soleil pèserait sinon sur toute la boîte.
float3 CloudCompress(float3 color)
{
    return color / (1.0 + max(max(color.r, color.g), color.b));
}

float3 CloudExpand(float3 color)
{
    return color / max(1.0 - max(max(color.r, color.g), color.b), 1.0e-4);
}

// Où était à l'image précédente le point à `distance` sur le rayon `direction` (uv de l'image précédente) ; CloudFar :
// à l'infini, seule la rotation de la caméra compte.
float2 CloudReproject(FrameData f, float3 direction, float distance)
{
    const float4 m[4] = f.previousViewProjection;
    const float4 clip = distance >= CloudFar
                          ? m[0] * direction.x + m[1] * direction.y + m[2] * direction.z
                          : Transform(f.previousViewProjection, direction * distance + f.cameraDelta.xyz);
    return clip.w > 0.0 ? clip.xy / clip.w * 0.5 + 0.5 : float2(-1.0);
}

[shader("fragment")]
float4 fragmentCloudResolve(CloudVertexOutput input) : SV_Target
{
    const FrameData f = frame[0];
    const float2 uv = input.uv;
    const float2 texel = CloudHalfTexel();
    // Voisinage 3 x 3 de l'image actuelle : la boîte où ramener l'historique (lumière compressée et opacité).
    float4 low = float4(1.0e9);
    float4 high = float4(-1.0e9);
    float4 center = float4(0.0);
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float4 value = cloudCurrent.SampleLevel(uv + float2(x, y) * texel, 0.0);
            const float4 compressed = float4(CloudCompress(max(value.rgb, float3(0.0))), value.a);
            low = min(low, compressed);
            high = max(high, compressed);
            if (x == 0 && y == 0) {
                center = compressed;
            }
        }
    }
    const float4 distances = cloudDistances.SampleLevel(uv, 0.0);
    // Le nuage moyen s'il y en a un, sinon ce qui arrête le rayon (relief, ou l'infini).
    const float distance = distances.y < CloudFar ? distances.y : distances.z;
    const float2 previousUv = CloudReproject(f, CloudRay(f, uv), distance);
    const bool inside = all(previousUv >= 0.0) && all(previousUv <= 1.0);
    float4 result = center;
    if (params.color.z > 0.0 && inside) {
        const float4 old = cloudHistory.SampleLevel(previousUv, 0.0);
        const float4 clamped = clamp(float4(CloudCompress(max(old.rgb, float3(0.0))), old.a), low, high);
        result = lerp(clamped, center, params.color.w);
    }
    return float4(CloudExpand(result.rgb), result.a);
}

[shader("fragment")]
float4 fragmentCloudComposite(CloudVertexOutput input) : SV_Target
{
    const FrameData f = frame[0];
    const float2 uv = input.uv;
    const float3 direction = CloudRay(f, uv);
    const float scene = CloudDepthToDistance(f, direction, cloudSceneDepth.SampleLevel(uv, 0.0).r);
    // Les 4 pixels de la demi-résolution autour de celui-ci, pondérés comme un filtre bilinéaire et par la ressemblance
    // entre la fin de leur rayon et la profondeur de ce pixel : au bord d'une montagne, ce pixel prend le nuage de ses
    // voisins du même côté que lui.
    const float2 position = uv * CloudHalfSize() - 0.5;
    const float2 base = floor(position);
    const float2 fraction = position - base;
    float4 sum = float4(0.0);
    float first = 0.0;
    float firstWeights = 0.0;
    float total = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const float2 offset = float2(i & 1, i >> 1);
        const float2 at = (base + offset + 0.5) * CloudHalfTexel();
        const float4 distances = cloudDistances.SampleLevel(at, 0.0);
        const float2 bilinear = lerp(1.0 - fraction, fraction, offset);
        const float difference = abs(log2(max(distances.z, 1.0) / max(scene, 1.0)));
        const float weight = bilinear.x * bilinear.y * max(exp2(-difference * 6.0), 1.0e-3);
        const float4 value = cloudHistory.SampleLevel(at, 0.0);
        sum += value * weight;
        // Premier nuage moyen des seuls voisins qui en voient un (pondéré par leur opacité) : ceux sans nuage
        // (CloudFar) feraient écarter tout le bord du nuage, en escalier de 2 pixels.
        first += min(distances.x, CloudFar) * weight * value.a;
        firstWeights += weight * value.a;
        total += weight;
    }
    const float4 cloud = sum / max(total, 1.0e-6);
    // Un objet plus proche que le premier nuage n'en a aucun devant lui (une antenne, un arbre fins devant le ciel).
    if (cloud.a < 0.004 || scene < first / max(firstWeights, 1.0e-6) * 0.97) {
        discard;
    }
    return float4(cloud.rgb / cloud.a, cloud.a); // non prémultiplié : le mélange multiplie par alpha
}
