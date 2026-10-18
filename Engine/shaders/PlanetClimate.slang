// Climat des planètes, partagé par le sol (shaders/Mesh.slang, fragmentPlanet) et les arbres
// (shaders/PlanetTile.slang, computeTrees) : les arbres poussent là où le sol est de l'herbe.

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
