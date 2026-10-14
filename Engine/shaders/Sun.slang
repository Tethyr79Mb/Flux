// Soleil : son propre shader, dessiné juste après le fond du ciel (Sky.slang), au même endroit (là où rien n'a été
// dessiné, profondeur restée à 0) et ajouté à lui. Un disque bien plus lumineux que tout le reste, assombri vers son
// bord comme le vrai (la lumière y traverse plus de gaz solaire), un halo serré et quelques rayons fins comme ceux
// d'un objectif (ou un long trait horizontal, au choix du composant Soleil). Sa lumière dépasse de loin le blanc : c'est le bloom (Post.slang) qui la fait déborder autour de
// lui. Vu de sous l'air d'une planète, il est rougi et atténué par l'air traversé, et caché sous l'horizon.
// Coût : un triangle plein écran, mais tout pixel à plus de MaxAngle du soleil (hors de la bande du trait) s'arrête
// aussitôt.

#include "SkyData.slang"

struct VertexOutput
{
    float4 position : SV_Position;
    float2 clip     : TEXCOORD; // -1 à 1, y vers le bas comme le clip space de Vulkan
};

// Comme le fond du ciel : un triangle qui couvre tout l'écran, à la profondeur 0 (infiniment loin).
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.clip = clip;
    return output;
}

// Au-delà de cet angle (radians) du centre, plus rien de visible.
static const float MaxAngle = 0.8;
// Éclat du disque par rapport à sa couleur (SkyData::discColor) : assez pour que le bloom l'entoure d'une lueur.
static const float DiscGain = 400.0;
// Assombrissement du bord : I(μ) = 1 - u (1 - μ), μ le cosinus de l'angle sous lequel on voit la surface du soleil.
static const float LimbDarkening = 0.6;
// Rayons : leur nombre (par paires opposées) et leur éclat.
static const float RayCount = 3.0;
static const float RayGain = 0.08;
// Trait horizontal (SunRays::Streak) : demi-épaisseur du trait fin en rayons du disque (au moins un pixel), sa
// longueur jusqu'au bord de l'écran (radians, de chaque côté) et son éclat, plus une bande floue autour.
static const float StreakWidth = 0.12;
static const float StreakLength = 0.6;
static const float StreakGain = 1.5;
static const float StreakBand = 6.0;
// Comme SunRays dans Components.hpp.
static const uint SunRaysStar = 0;
static const uint SunRaysStreak = 1;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    const SkyData s = sky[0];
    const float radius = s.toSun.w;
    if (radius <= 0.0) {
        discard;
    }
    const float3 direction = normalize(s.forward.xyz + s.right.xyz * input.clip.x - s.up.xyz * input.clip.y);
    const float3 toSun = s.toSun.xyz;
    const float angle = 2.0 * asin(min(length(direction - toSun) * 0.5, 1.0));
    const uint rayStyle = s.options.z;
    const float pixelAngle = s.forward.w;
    // Pour l'étoile : position autour du soleil, h vers la droite, v vers le haut (radians, à peu près).
    const float3 offset = direction - toSun;
    const float h = dot(offset, normalize(s.right.xyz));
    const float v = dot(offset, normalize(s.up.xyz));
    // Pour le trait : écart au soleil projeté sur l'écran (en tangentes, à peu près des radians près du centre), pour
    // un trait bien droit d'un bord à l'autre ; rien si le soleil est derrière la caméra.
    const float sunAhead = dot(toSun, s.forward.xyz);
    bool inStreak = false;
    float2 screen = float2(0.0);
    if (rayStyle == SunRaysStreak && sunAhead > 0.05) {
        const float2 halfTan = float2(length(s.right.xyz), length(s.up.xyz));
        const float2 sunClip = float2(dot(toSun, s.right.xyz) / (halfTan.x * halfTan.x),
                                      -dot(toSun, s.up.xyz) / (halfTan.y * halfTan.y)) / sunAhead;
        screen = (input.clip - sunClip) * halfTan;
        inStreak = abs(screen.y) < radius * StreakBand * 2.0 && abs(screen.x) < 3.0 * StreakLength;
    }
    if (angle > MaxAngle && !inStreak) {
        discard;
    }

    // Disque : bord adouci sur un pixel ; un peu plus rouge vers le bord (le bleu s'y assombrit plus vite).
    const float inside = saturate(angle / radius);
    const float mu = sqrt(max(1.0 - inside * inside, 0.0));
    const float3 limb = (1.0 - LimbDarkening * (1.0 - mu)) * pow(max(mu, 1.0e-3), float3(0.15, 0.25, 0.4));
    const float edge = saturate((radius - angle) / pixelAngle + 0.5);
    float3 light = limb * (edge * DiscGain);

    // Halo serré autour du disque et lueur large qui s'éteint vite.
    light += exp(-(angle * angle) / (radius * radius * 9.0)) * 0.5 + exp(-angle * 8.0) * 0.03;

    // Rayons fins dans le plan de l'écran, comme les branches d'étoile d'un objectif : plus longs que le halo, mais
    // faibles ; ils tournent avec la caméra.
    if (rayStyle == SunRaysStar) {
        const float phi = atan2(v, h);
        const float rays = pow(abs(cos(phi * RayCount)), 400.0) * exp(-angle / (radius * 3.0));
        light += rays * RayGain * saturate(angle / radius - 1.0) * DiscGain * 0.01;
    } else if (inStreak) {
        // Trait fin et très long, plus vif près du disque ; une bande floue plus courte autour, qu'élargit le bloom.
        // Lui aussi reste dans le plan de l'écran : il tourne avec la caméra.
        const float width = max(radius * StreakWidth, pixelAngle * 0.7);
        const float along = exp(-abs(screen.x) / StreakLength) + 2.0 * exp(-abs(screen.x) / (radius * 12.0));
        light += exp(-(screen.y * screen.y) / (width * width)) * along * StreakGain;
        const float band = radius * StreakBand;
        light += exp(-(screen.y * screen.y) / (band * band)) * (exp(-abs(screen.x) / (radius * 20.0)) + 0.3 * exp(-abs(screen.x) / StreakLength))
               * StreakGain * 0.1;
    }

    float3 color = s.discColor.rgb * light;
    if (HasAtmosphere(s.atmosphere)) {
        // Vu à travers l'air : caché par le sol, puis atténué par l'air traversé jusqu'à l'espace (rougi bas sur
        // l'horizon), comme le fond du ciel derrière lui.
        const float3 position = -s.atmosphere.center.xyz;
        const float2 ground = AtmosphereRaySphere(position, direction, s.atmosphere.center.w);
        if (ground.x <= ground.y && ground.x > 0.0) {
            discard;
        }
        const float3 depth = AtmosphereSunDepth(s.atmosphere, position, direction, 0.0, AtmosphereSunsetAngle);
        color *= depth.z <= 0.0 ? float3(0.0) : AtmosphereExtinction(s.atmosphere, depth.x, depth.y) * depth.z;
    }
    return float4(min(color, float3(60000.0)), 0.0);
}
