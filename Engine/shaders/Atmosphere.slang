// Atmosphère d'une planète : diffusion de Rayleigh (le bleu du ciel, le rouge du couchant) et de Mie (la brume
// autour du soleil), une seule diffusion, en marchant le long du regard. Inclus par shaders/Sky.slang (le ciel) et
// shaders/Mesh.slang (ce qu'on voit à travers). Exactement les calculs de Engine/src/Planetology/Atmosphere.cpp (toute
// modification des deux côtés), sauf le décalage des pas, tiré au hasard par pixel (du bruit fin au lieu de bandes).
// Caméra dans l'air : le ciel et l'air jusqu'aux surfaces sont lus dans deux tables calculées à chaque image (plus bas),
// au lieu d'une marche par pixel et par sommet.

static const uint AtmosphereAmbientSize = 16;
static const float AtmospherePi = 3.14159265;
static const float AtmosphereMieExtinction = 1.1;

// Doit correspondre à AtmosphereGpu dans Planetology/AtmosphereGpu.hpp.
struct AtmosphereData
{
    float4 center;   // xyz : centre de la planète relatif à la caméra ; w : rayon du sol (m)
    float4 rayleigh; // rgb : diffusion de Rayleigh au sol (1/m) ; w : 1 / hauteur d'échelle
    float4 mie;      // x : diffusion de Mie au sol ; y : 1 / hauteur d'échelle ; z : g ; w : rayon du haut (0 : aucune)
    float4 scale;    // x : intensité de la lumière diffusée
    float4 ambient[AtmosphereAmbientSize]; // lumière du ciel au sol selon le cosinus du soleil (de -1 à 1)
};

bool HasAtmosphere(AtmosphereData a)
{
    return a.mie.w > 0.0;
}

// Entrée et sortie de la sphère de centre l'origine (x > y : manquée).
float2 AtmosphereRaySphere(float3 origin, float3 direction, float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0) {
        return float2(1.0, -1.0);
    }
    const float root = sqrt(discriminant);
    return float2(-b - root, -b + root);
}

float3 AtmosphereExtinction(AtmosphereData a, float rayleighDepth, float mieDepth)
{
    return exp(-(a.rayleigh.rgb * rayleighDepth + a.mie.x * AtmosphereMieExtinction * mieDepth));
}

// Épaisseur d'air jusqu'à l'infini (fonction de Chapman approchée, comme Column dans Atmosphere.cpp).
float AtmosphereColumn(float radius, float height, float falloff, float cosine)
{
    const float x = radius * falloff;
    const float h = max(height, 0.0) * falloff;
    const float c = sqrt(x + h);
    float depth;
    if (cosine >= 0.0) {
        depth = c / (c * cosine + 1.0) * exp(-h);
    } else {
        const float x0 = sqrt(1.0 - cosine * cosine) * (x + h);
        const float c0 = sqrt(x0);
        depth = 2.0 * c0 * exp(min(x - x0, 80.0)) - c / (1.0 - c * cosine) * exp(-h);
    }
    return depth / falloff;
}

// Épaisseurs (Rayleigh, Mie) de `position` (relatif au centre) vers le soleil, et en z la part de soleil non cachée
// par le sol (sphère du rayon du sol, ou plus bas que le point : fond d'une vallée). Le soleil se couche quand son
// cosinus passe sous celui de l'horizon de cette sphère, puis s'éteint en douceur, sans marche sur la ligne
// jour/nuit : sur `soft` mètres sous le sol (dans l'air : une hauteur d'échelle, sinon des arcs côté nuit), ou si
// `angle` > 0, sur `angle` radians de soleil sous l'horizon (au sol : un coucher net dessinait une ligne droite, la
// même pour tous les creux, là où les pentes tournées vers le soleil étaient encore éclairées). Comme Atmosphere.cpp.
float3 AtmosphereSunDepth(AtmosphereData a, float3 position, float3 toSun, float soft, float angle)
{
    const float distance = max(length(position), 1.0);
    const float cosine = dot(position, toSun) / distance;
    float visible = 1.0;
    if (cosine < 0.0) {
        const float ground = min(a.center.w, distance);
        if (angle > 0.0) {
            const float ratio = ground / distance;
            const float horizon = -sqrt(max(0.0, 1.0 - ratio * ratio)); // cosinus du soleil à l'horizon
            visible = smoothstep(horizon - angle, horizon, cosine);
        } else {
            const float closest = distance * sqrt(max(0.0, 1.0 - cosine * cosine));
            visible = smoothstep(ground - 2.0 * soft, ground, closest);
        }
        if (visible <= 0.0) {
            return float3(0.0);
        }
    }
    const float height = distance - a.center.w;
    return float3(AtmosphereColumn(a.center.w, height, a.rayleigh.w, cosine),
                  AtmosphereColumn(a.center.w, height, a.mie.y, cosine), visible);
}

// Soleil couché au sol sur cet angle (radians, environ 3°) : voir AtmosphereSunDepth.
static const float AtmosphereSunsetAngle = 0.05;

// Part de la lumière du soleil qui arrive en `position` (relatif au centre) : rougie au couchant, nulle la nuit.
float3 AtmosphereSunTransmittance(AtmosphereData a, float3 position, float3 toSun)
{
    const float3 depth = AtmosphereSunDepth(a, position, toSun, 0.0, AtmosphereSunsetAngle);
    return depth.z <= 0.0 ? float3(0.0) : AtmosphereExtinction(a, depth.x, depth.y) * depth.z;
}

// Lumière diffusée vers la caméra (à l'origine) dans `direction`, sur `distance` mètres au plus. transmittance : ce
// qui passe de la lumière venue de plus loin. jitter : décalage des pas, de 0 à 1 (0,5 : comme Atmosphere.cpp).
float3 AtmosphereScatter(AtmosphereData a, float3 direction, float distance, float3 toSun, float3 sunColor, int steps,
                         float jitter, out float3 transmittance)
{
    return AtmosphereScatterFrom(a, -a.center.xyz, direction, distance, toSun, sunColor, steps, jitter, transmittance);
}

// Sommes de la marche le long d'un regard, avant les fonctions de phase (qui ne dépendent que de l'angle avec le
// soleil) : ce que gardent les tables de l'air (AtmosphereTables), la phase étant appliquée à la lecture.
struct AtmosphereSums
{
    float3 rayleigh;     // lumière diffusée par Rayleigh, par unité de lumière du soleil (avant phase et intensité)
    float3 mie;          // la même pour Mie
    float rayleighDepth; // épaisseur d'air traversée (m de densité du sol), pour la transmittance
    float mieDepth;
};

// Marche de `origin` (relatif au centre de la planète) dans `direction`, sur `distance` mètres au plus. lowest :
// hauteur sous laquelle la densité ne monte plus (les tables y passent 0 : au-delà du sol, sous la sphère du sol,
// l'air ne monte pas sans fin) ; -1e30 : aucune limite, comme Atmosphere.cpp.
AtmosphereSums AtmosphereMarch(AtmosphereData a, float3 origin, float3 direction, float distance, float3 toSun, int steps,
                               float jitter, float lowest)
{
    AtmosphereSums sums;
    sums.rayleigh = float3(0.0);
    sums.mie = float3(0.0);
    sums.rayleighDepth = 0.0;
    sums.mieDepth = 0.0;
    const float2 hit = AtmosphereRaySphere(origin, direction, a.mie.w);
    if (hit.x > hit.y || hit.y <= 0.0) {
        return sums;
    }
    const float start = max(hit.x, 0.0);
    const float end = min(hit.y, distance);
    if (end <= start) {
        return sums;
    }
    const float step = (end - start) / float(steps);
    float viewRayleigh = 0.0;
    float viewMie = 0.0;
    float3 sumRayleigh = float3(0.0);
    float3 sumMie = float3(0.0);
    for (int i = 0; i < steps; ++i) {
        const float3 p = origin + direction * (start + (float(i) + jitter) * step);
        const float h = max(length(p) - a.center.w, lowest);
        const float rayleigh = exp(-h * a.rayleigh.w) * step;
        const float mie = exp(-h * a.mie.y) * step;
        viewRayleigh += 0.5 * rayleigh;
        viewMie += 0.5 * mie;
        const float3 sunDepth = AtmosphereSunDepth(a, p, toSun, 1.0 / a.rayleigh.w, 0.0);
        if (sunDepth.z > 0.0) {
            const float3 light = AtmosphereExtinction(a, viewRayleigh + sunDepth.x, viewMie + sunDepth.y) * sunDepth.z;
            sumRayleigh += light * rayleigh;
            sumMie += light * mie;
        }
        viewRayleigh += 0.5 * rayleigh;
        viewMie += 0.5 * mie;
    }
    sums.rayleigh = sumRayleigh * a.rayleigh.rgb;
    sums.mie = sumMie * a.mie.x;
    sums.rayleighDepth = viewRayleigh;
    sums.mieDepth = viewMie;
    return sums;
}

// Lumière diffusée vers l'œil (regard `direction`) et transmittance, d'après les sommes : les fonctions de phase.
float3 AtmosphereLight(AtmosphereData a, AtmosphereSums sums, float3 direction, float3 toSun, float3 sunColor,
                       out float3 transmittance)
{
    transmittance = AtmosphereExtinction(a, sums.rayleighDepth, sums.mieDepth);
    const float mu = dot(direction, toSun);
    const float rayleighPhase = 3.0 / (16.0 * AtmospherePi) * (1.0 + mu * mu);
    const float g = a.mie.z;
    const float denominator = 1.0 + g * g - 2.0 * g * mu;
    const float miePhase = (1.0 - g * g) / (4.0 * AtmospherePi * denominator * sqrt(denominator));
    return (sums.rayleigh * rayleighPhase + sums.mie * miePhase) * sunColor * a.scale.x;
}

// La même depuis `origin` (relatif au centre de la planète) au lieu de la caméra : le ciel que reflète l'eau.
float3 AtmosphereScatterFrom(AtmosphereData a, float3 origin, float3 direction, float distance, float3 toSun, float3 sunColor,
                             int steps, float jitter, out float3 transmittance)
{
    const AtmosphereSums sums = AtmosphereMarch(a, origin, direction, distance, toSun, steps, jitter, -1.0e30);
    return AtmosphereLight(a, sums, direction, toSun, sunColor, transmittance);
}

// --- Tables de l'air (calculées à chaque image par Sky.slang quand la caméra est dans l'air : scale.y à 1) ---
// Deux textures par table : rgb, AtmosphereSums::rayleigh (ou mie) ; a, l'épaisseur en hauteurs d'échelle (en mètres,
// elle dépasserait ce que tient un flottant 16 bits).

static const float AtmosphereTableDepthMax = 60000.0;

// Ciel vu de la caméra : colonnes, angle autour de la verticale depuis le soleil (0 à pi, symétrique) ; lignes, angle
// depuis le zénith, resserré près de l'horizon où le ciel change vite (Hillaire 2020), l'horizon au milieu.
static const uint2 AtmosphereSkySize = uint2(256, 128);

// Air entre la caméra et ce qu'elle voit : AtmosphereAirSize x AtmosphereAirSize regards répartis sur le champ (un peu
// plus large, AtmosphereAirMargin : les sommets juste hors de l'écran), AtmosphereAirSlices distances de 0 à toute
// l'épaisseur de l'air, plus serrées près de la caméra. Tranches rangées en AtmosphereAirTiles dans une texture 2D.
static const uint AtmosphereAirSize = 64;
static const uint AtmosphereAirSlices = 64;
static const uint2 AtmosphereAirTiles = uint2(8, 8);
static const float AtmosphereAirMargin = 1.3;
static const float AtmosphereAirNear = 1000.0; // m : distances presque régulières en deçà, de plus en plus espacées au-delà

bool HasAtmosphereTables(AtmosphereData a)
{
    return a.scale.y > 0.0;
}

// Distance de la tranche s (0 à 1) ; scale.w : log(1 + distance la plus longue / AtmosphereAirNear).
float AtmosphereSliceDistance(AtmosphereData a, float s)
{
    return AtmosphereAirNear * (exp(s * a.scale.w) - 1.0);
}

float AtmosphereDistanceSlice(AtmosphereData a, float distance)
{
    return saturate(log(1.0 + distance / AtmosphereAirNear) / a.scale.w);
}

// Ce qu'écrit une table pour une marche, et ce qu'en tire la lecture.
void AtmosphereTableWrite(AtmosphereData a, AtmosphereSums sums, out float4 rayleigh, out float4 mie)
{
    rayleigh = float4(sums.rayleigh, min(sums.rayleighDepth * a.rayleigh.w, AtmosphereTableDepthMax));
    mie = float4(sums.mie, min(sums.mieDepth * a.mie.y, AtmosphereTableDepthMax));
}

AtmosphereSums AtmosphereTableRead(AtmosphereData a, float4 rayleigh, float4 mie)
{
    AtmosphereSums sums;
    sums.rayleigh = rayleigh.rgb;
    sums.mie = mie.rgb;
    sums.rayleighDepth = rayleigh.a / a.rayleigh.w;
    sums.mieDepth = mie.a / a.mie.y;
    return sums;
}

// Côté du soleil dans le plan de l'horizon du lieu (unitaire) ; soleil au zénith ou au nadir : un côté quelconque.
float3 AtmosphereSunSide(float3 up, float3 toSun)
{
    const float3 side = toSun - up * dot(toSun, up);
    const float length2 = dot(side, side);
    if (length2 < 1.0e-8) {
        return normalize(cross(up, abs(up.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
    }
    return side * rsqrt(length2);
}

// Angle (radians) entre le zénith et l'horizon du sol vu à `radius` du centre, et sa part sous l'horizontale.
float2 AtmosphereHorizon(AtmosphereData a, float radius)
{
    const float r = max(radius, a.center.w + 1.0);
    const float beta = acos(saturate(sqrt((r - a.center.w) * (r + a.center.w)) / r));
    return float2(AtmospherePi - beta, beta);
}

// Coordonnées du ciel vu de `origin` (relatif au centre) dans `direction`.
float2 AtmosphereSkyUv(AtmosphereData a, float3 origin, float3 direction, float3 toSun)
{
    const float radius = length(origin);
    const float3 up = origin / radius;
    const float cosZenith = clamp(dot(direction, up), -1.0, 1.0);
    const float3 flat = direction - up * cosZenith;
    const float flatLength = length(flat);
    const float cosAzimuth = flatLength > 1.0e-6 ? clamp(dot(flat, AtmosphereSunSide(up, toSun)) / flatLength, -1.0, 1.0) : 1.0;
    const float2 horizon = AtmosphereHorizon(a, radius);
    const float zenith = acos(cosZenith);
    float v;
    if (zenith < horizon.x) {
        v = 0.5 * (1.0 - sqrt(saturate(1.0 - zenith / horizon.x)));
    } else {
        v = 0.5 + 0.5 * sqrt(saturate((zenith - horizon.x) / horizon.y));
    }
    return float2(acos(cosAzimuth) / AtmospherePi, v);
}

// Inverse de AtmosphereSkyUv : le regard d'un texel de la table.
float3 AtmosphereSkyDirection(AtmosphereData a, float3 origin, float2 uv, float3 toSun)
{
    const float radius = length(origin);
    const float3 up = origin / radius;
    const float3 side = AtmosphereSunSide(up, toSun);
    const float2 horizon = AtmosphereHorizon(a, radius);
    float zenith;
    if (uv.y < 0.5) {
        const float s = 1.0 - 2.0 * uv.y;
        zenith = (1.0 - s * s) * horizon.x;
    } else {
        const float c = 2.0 * uv.y - 1.0;
        zenith = horizon.x + c * c * horizon.y;
    }
    const float azimuth = uv.x * AtmospherePi;
    return up * cos(zenith) + (side * cos(azimuth) + cross(up, side) * sin(azimuth)) * sin(zenith);
}

// Coordonnées (0 à 1) dans une tranche de la table de l'air du point `p` (relatif à la caméra), regard de la caméra
// `forward`, `right` et `up` à l'échelle du champ ; false : derrière la caméra ou hors de la table.
bool AtmosphereAirUv(float3 p, float3 forward, float3 right, float3 up, out float2 uv)
{
    const float depth = dot(p, forward);
    uv = float2(0.0);
    if (depth <= 0.0) {
        return false;
    }
    const float2 tangent = float2(dot(p, right) / dot(right, right), dot(p, up) / dot(up, up)) / depth;
    uv = tangent / AtmosphereAirMargin * 0.5 + 0.5;
    return all(uv >= 0.0) && all(uv <= 1.0);
}

// Coordonnées dans la texture de la tranche `slice`, et l'inverse (la tranche et le regard d'un texel).
float2 AtmosphereAirTexel(float2 uv, uint slice)
{
    const float2 inside = (0.5 + uv * float(AtmosphereAirSize - 1)) / float(AtmosphereAirSize);
    return (float2(slice % AtmosphereAirTiles.x, slice / AtmosphereAirTiles.x) + inside) / float2(AtmosphereAirTiles);
}

// Part de la lumière du ciel de l'espace (texture du ciel) qui éclaire encore un point dans l'air de la planète :
// rien de jour (le ciel bleu de l'atmosphère le cache, AtmosphereAmbient éclaire à sa place), peu la nuit (la nuit
// tombe du côté opposé au soleil, avec un crépuscule), toute au-dessus de l'air. De jour, la nébuleuse teintait en
// violet le personnage et les vaisseaux posés.
static const float AtmosphereNightSkyLight = 0.12;

float AtmosphereNightSky(AtmosphereData a, float3 position, float3 toSun)
{
    const float distance = length(position);
    const float day = smoothstep(-0.2, 0.05, dot(position / max(distance, 1.0), toSun));
    const float inAir = saturate((a.mie.w - distance) / max(a.mie.w - a.center.w, 1.0));
    return lerp(1.0, lerp(AtmosphereNightSkyLight, 0.0, day), inAir);
}

// Lumière du ciel reçue en `position` (relatif au centre) par une surface de normale `normal` : le tableau lu selon
// la hauteur du soleil sur l'horizon du lieu, plus faible en altitude (moins d'air au-dessus) et pour une surface
// tournée vers le sol.
float3 AtmosphereAmbient(AtmosphereData a, float3 position, float3 normal, float3 toSun)
{
    const float distance = length(position);
    const float3 up = position / max(distance, 1.0);
    const float index = (dot(up, toSun) * 0.5 + 0.5) * float(AtmosphereAmbientSize - 1);
    const uint i0 = min(uint(index), AtmosphereAmbientSize - 2);
    const float3 sky = lerp(a.ambient[i0].rgb, a.ambient[i0 + 1].rgb, saturate(index - float(i0)));
    const float altitude = exp(-max(distance - a.center.w, 0.0) * a.rayleigh.w);
    return sky * altitude * (0.6 + 0.4 * dot(normal, up));
}
