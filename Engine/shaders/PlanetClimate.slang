// Climat et forêts des planètes, partagés par le sol (shaders/Mesh.slang, fragmentPlanet) et les arbres
// (shaders/PlanetTile.slang, computeTrees) : les arbres poussent là où le sol est de l'herbe, et au-delà des arbres
// le sol prend la couleur des forêts aux mêmes endroits.

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

// Température (°C) et humidité (0 à 1 environ) au point `local` (repère de la planète, centre à l'origine).
// climate : température à l'équateur, aux pôles, humidité de la planète ; waterRadius : rayon de la surface de
// l'océan (0 : aucun).
float2 ClimateAt(float radius, float relief, float3 climate, float waterRadius, float3 local)
{
    relief = max(relief, 1.0);
    const float distance = length(local);
    const float altitude = distance - radius;
    const float latitude = abs(local.y) / max(distance, 1.0);
    const float temperature = lerp(climate.x, climate.y, (latitude * latitude) * (latitude * latitude))
                            - 0.0065 * max(altitude, 0.0) + (PlanetNoise(local * (100.0 / radius)) - 0.5) * 8.0;
    const float coast = waterRadius > 0.0 ? smoothstep(0.02 * relief, 0.0, distance - waterRadius) : 0.0;
    const float humidity = climate.z + (PlanetNoise(local * (2.0 / radius) + 17.0) - 0.5) * 0.9
                         + (PlanetNoise(local * (1.0 / 30000.0) + 5.0) - 0.5) * 0.4 + coast * 0.3;
    return float2(temperature, humidity);
}

// Forêts : taches de 150 m à 1 km sur la sphère du rayon de la planète (flatPoint, m, repère de la planète), que le
// relief ne déplace pas. cover : part du sol couverte. 1 en forêt, 0 ailleurs. blur (0 à 1) : vu de loin, les taches
// plus petites que l'écart des points calculés (les sommets) s'effacent vers leur moyenne, au lieu de scintiller.
float ForestAt(float3 flatPoint, float cover, float2 blur = float2(0.0))
{
    const float noise = 0.5 + (PlanetNoise(flatPoint * (1.0 / 600.0)) - 0.5) * 0.65 * (1.0 - blur.y)
                      + (PlanetNoise(flatPoint * (1.0 / 150.0) + 7.0) - 0.5) * 0.35 * (1.0 - blur.x);
    const float threshold = 0.8 - 0.6 * cover;
    const float width = 0.03 + 0.3 * blur.y;
    return smoothstep(threshold - width, threshold + width, noise);
}

// Climat où poussent les arbres (température °C, humidité) : celui de l'herbe, sans gel.
float TreeClimate(float2 climate)
{
    return smoothstep(0.38, 0.55, climate.y) * smoothstep(0.0, 4.0, climate.x);
}
