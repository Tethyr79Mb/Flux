// Atmosphère d'une planète : diffusion de Rayleigh (le bleu du ciel, le rouge du couchant) et de Mie (la brume
// autour du soleil), une seule diffusion, en marchant le long du regard. Inclus par shaders/Sky.slang (le ciel) et
// shaders/Mesh.slang (ce qu'on voit à travers). Exactement les calculs de Engine/src/Graphics/Atmosphere.cpp (toute
// modification des deux côtés), sauf le décalage des pas, tiré au hasard par pixel (du bruit fin au lieu de bandes).

static const uint AtmosphereAmbientSize = 16;
static const float AtmospherePi = 3.14159265;
static const float AtmosphereMieExtinction = 1.1;

// Doit correspondre à AtmosphereGpu dans Renderer/AtmosphereGpu.hpp.
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
// par le sol : fondue sur `soft` mètres quand le rayon le frôle (comme Atmosphere.cpp : une hauteur d'échelle dans
// l'air, sinon des arcs côté nuit ; net au sol).
float3 AtmosphereSunDepth(AtmosphereData a, float3 position, float3 toSun, float soft)
{
    const float distance = max(length(position), 1.0);
    const float cosine = dot(position, toSun) / distance;
    float visible = 1.0;
    if (cosine < 0.0) {
        const float closest = distance * sqrt(max(0.0, 1.0 - cosine * cosine));
        const float ground = min(a.center.w, distance);
        visible = smoothstep(ground - soft, ground + soft, closest);
        if (visible <= 0.0) {
            return float3(0.0);
        }
    }
    const float height = distance - a.center.w;
    return float3(AtmosphereColumn(a.center.w, height, a.rayleigh.w, cosine),
                  AtmosphereColumn(a.center.w, height, a.mie.y, cosine), visible);
}

// Part de la lumière du soleil qui arrive en `position` (relatif au centre) : rougie au couchant, nulle la nuit.
float3 AtmosphereSunTransmittance(AtmosphereData a, float3 position, float3 toSun)
{
    const float3 depth = AtmosphereSunDepth(a, position, toSun, 1.0);
    return depth.z <= 0.0 ? float3(0.0) : AtmosphereExtinction(a, depth.x, depth.y) * depth.z;
}

// Lumière diffusée vers la caméra (à l'origine) dans `direction`, sur `distance` mètres au plus. transmittance : ce
// qui passe de la lumière venue de plus loin. jitter : décalage des pas, de 0 à 1 (0,5 : comme Atmosphere.cpp).
float3 AtmosphereScatter(AtmosphereData a, float3 direction, float distance, float3 toSun, float3 sunColor, int steps,
                         float jitter, out float3 transmittance)
{
    transmittance = float3(1.0);
    const float3 origin = -a.center.xyz;
    const float2 hit = AtmosphereRaySphere(origin, direction, a.mie.w);
    if (hit.x > hit.y || hit.y <= 0.0) {
        return float3(0.0);
    }
    const float start = max(hit.x, 0.0);
    const float end = min(hit.y, distance);
    if (end <= start) {
        return float3(0.0);
    }
    const float step = (end - start) / float(steps);
    float viewRayleigh = 0.0;
    float viewMie = 0.0;
    float3 sumRayleigh = float3(0.0);
    float3 sumMie = float3(0.0);
    for (int i = 0; i < steps; ++i) {
        const float3 p = origin + direction * (start + (float(i) + jitter) * step);
        const float h = length(p) - a.center.w;
        const float rayleigh = exp(-h * a.rayleigh.w) * step;
        const float mie = exp(-h * a.mie.y) * step;
        viewRayleigh += 0.5 * rayleigh;
        viewMie += 0.5 * mie;
        const float3 sunDepth = AtmosphereSunDepth(a, p, toSun, 1.0 / a.rayleigh.w);
        if (sunDepth.z > 0.0) {
            const float3 light = AtmosphereExtinction(a, viewRayleigh + sunDepth.x, viewMie + sunDepth.y) * sunDepth.z;
            sumRayleigh += light * rayleigh;
            sumMie += light * mie;
        }
        viewRayleigh += 0.5 * rayleigh;
        viewMie += 0.5 * mie;
    }
    transmittance = AtmosphereExtinction(a, viewRayleigh, viewMie);
    const float mu = dot(direction, toSun);
    const float rayleighPhase = 3.0 / (16.0 * AtmospherePi) * (1.0 + mu * mu);
    const float g = a.mie.z;
    const float denominator = 1.0 + g * g - 2.0 * g * mu;
    const float miePhase = (1.0 - g * g) / (4.0 * AtmospherePi * denominator * sqrt(denominator));
    return (sumRayleigh * a.rayleigh.rgb * rayleighPhase + sumMie * (a.mie.x * miePhase)) * sunColor * a.scale.x;
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

// Bruit par pixel pour décaler les pas (« interleaved gradient noise » de Jimenez) : sans motif visible.
float AtmosphereJitter(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
