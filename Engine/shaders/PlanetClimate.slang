// Climat, forêts et biomes des planètes, partagés par le sol (shaders/Mesh.slang, fragmentPlanet) et les arbres
// (shaders/PlanetTile.slang, computeTrees) : les arbres poussent selon les biomes, et au-delà des arbres le sol prend
// la couleur des forêts aux mêmes endroits.

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

// Bruit de valeur 3D (de 0 à 1) du climat et des forêts : comme PlanetNoise, mais haché sur des entiers, exact sur tous
// les GPU comme sur le CPU (ClimateNoise dans Planetology/TreeSowing.cpp, qui refait le semis des arbres pour leur
// collision). Le hachage en float de PlanetHash change d'un pilote à l'autre (multiplications fusionnées).
float ClimateHash(int3 c)
{
    uint x = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ uint(c.z) * 0xcb1ab31fu;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.0 / 16777216.0);
}

float ClimateNoise(float3 x)
{
    const float3 f = floor(x);
    const int3 i = int3(f);
    float3 t = x - f;
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(lerp(ClimateHash(i), ClimateHash(i + int3(1, 0, 0)), t.x),
                     lerp(ClimateHash(i + int3(0, 1, 0)), ClimateHash(i + int3(1, 1, 0)), t.x), t.y),
                lerp(lerp(ClimateHash(i + int3(0, 0, 1)), ClimateHash(i + int3(1, 0, 1)), t.x),
                     lerp(ClimateHash(i + int3(0, 1, 1)), ClimateHash(i + int3(1, 1, 1)), t.x), t.y), t.z);
}

// Humidité ajoutée selon la latitude (degrés), comme sur Terre : l'air chaud monte et pleut à l'équateur, redescend sec
// vers 25-30° (les grands déserts), pleut de nouveau vers 50-60° (forêts tempérées et boréales), redescend sec aux pôles.
float LatitudeHumidity(float degrees)
{
    const float equator = (degrees / 12.0) * (degrees / 12.0);
    const float tropics = ((degrees - 27.0) / 10.0) * ((degrees - 27.0) / 10.0);
    const float temperate = ((degrees - 55.0) / 13.0) * ((degrees - 55.0) / 13.0);
    return 0.3 * exp(-equator) - 0.35 * exp(-tropics) + 0.12 * exp(-temperate) - 0.15 * smoothstep(65.0, 90.0, degrees);
}

// Température (°C) et humidité (0 à 1 environ) au point `local` (repère de la planète, centre à l'origine).
// climate : température à l'équateur, aux pôles, humidité de la planète ; waterRadius : rayon de la surface de
// l'océan (0 : aucun). Les ceintures de latitude (LatitudeHumidity) décident d'abord, des taches de quelques centaines
// de km et de 30 km les déforment, les côtes sont plus humides.
float2 ClimateAt(float radius, float relief, float3 climate, float waterRadius, float3 local)
{
    relief = max(relief, 1.0);
    const float distance = length(local);
    const float altitude = distance - radius;
    const float latitude = abs(local.y) / max(distance, 1.0);
    const float temperature = lerp(climate.x, climate.y, (latitude * latitude) * (latitude * latitude))
                            - 0.0065 * max(altitude, 0.0) + (ClimateNoise(local * (100.0 / radius)) - 0.5) * 8.0;
    const float coast = waterRadius > 0.0 ? smoothstep(0.02 * relief, 0.0, distance - waterRadius) : 0.0;
    // Ceintures ondulées de ±8° (les déserts ne font pas le tour de la planète en ligne droite).
    const float wave = (ClimateNoise(local * (1.5 / radius) + 31.0) - 0.5) * 16.0;
    const float humidity = climate.z + LatitudeHumidity(abs(asin(saturate(latitude)) * 57.29578 + wave))
                         + (ClimateNoise(local * (2.0 / radius) + 17.0) - 0.5) * 0.5
                         + (ClimateNoise(local * (1.0 / 30000.0) + 5.0) - 0.5) * 0.3 + coast * 0.3;
    return float2(temperature, humidity);
}

// Forêts : taches de 150 m à 1 km sur la sphère du rayon de la planète (flatPoint, m, repère de la planète), que le
// relief ne déplace pas. ForestNoise ne dépend que du point ; ForestMask en tire la forêt (1) ou non (0) pour une part
// couverte cover, ce qui permet une couverture par biome sans refaire le bruit. blur (0 à 1) : vu de loin, les taches
// plus petites que l'écart des points calculés (les sommets) s'effacent vers leur moyenne, au lieu de scintiller.
float ForestNoise(float3 flatPoint, float2 blur = float2(0.0))
{
    return 0.5 + (ClimateNoise(flatPoint * (1.0 / 600.0)) - 0.5) * 0.65 * (1.0 - blur.y)
         + (ClimateNoise(flatPoint * (1.0 / 150.0) + 7.0) - 0.5) * 0.35 * (1.0 - blur.x);
}

float ForestMask(float noise, float cover, float2 blur = float2(0.0))
{
    const float threshold = 0.8 - 0.6 * cover;
    const float width = 0.03 + 0.3 * blur.y;
    return smoothstep(threshold - width, threshold + width, noise);
}

// Humidité du relief (Planetology/ReliefWetness.hpp) : de -1 (crête) à 1 (fond de vallon), calculée par tuile
// (shaders/PlanetTile.slang) et rangée dans chaque sommet. Ce qu'elle ajoute à l'humidité du climat et à la part
// couverte de forêts d'un biome.
static const float ReliefHumidity = 0.25;
static const float ReliefForest = 0.3;

// Les sommets des tuiles portent dans uv.x les zones du relief (Terrain::Zones : collines, montagnes, 0 à 1) sur
// 10 bits chacune, en un entier exact dans un float, et dans uv.y l'humidité du relief.
float PackZones(float2 zones)
{
    const float2 q = round(saturate(zones) * 1023.0);
    return q.x * 1024.0 + q.y;
}

float2 UnpackZones(float packed)
{
    const float hills = floor(packed * (1.0 / 1024.0));
    return float2(hills, packed - hills * 1024.0) * (1.0 / 1023.0);
}

// --- Roche-mère (géologie, étape 2) ---

// Roche sous le sol, par régions de 20 à 80 km aux bords fondus sur quelques km (flatPoint : point sur la sphère du
// rayon de la planète, m ; mountains : zone des montagnes, Terrain::Zones). x : granit, surtout dans les chaînes ;
// y : calcaire et z : grès, dans les plaines et les bassins ; basalte (1 - x - y - z) par taches. Une formule, rien de
// stocké, sans effet sur la collision : seulement la couleur du sol et la profondeur de sa terre.
float3 Lithology(float3 flatPoint, float mountains)
{
    const float3 p = flatPoint * (1.0 / 45000.0);
    // Bords ondulés : le point lu est déplacé par un bruit plus fin (les régions ne sont pas des taches rondes).
    const float3 warp = float3(ClimateNoise(p * 3.0 + 13.0), ClimateNoise(p * 3.0 + 57.0), ClimateNoise(p * 3.0 + 91.0)) - 0.5;
    const float a = ClimateNoise(p + warp * 0.6 + 101.0);
    const float b = ClimateNoise(p * 1.7 + warp * 0.8 + 211.0);
    const float basalt = smoothstep(0.66, 0.72, b);
    const float granite = smoothstep(0.55, 0.62, a + 0.3 * mountains) * (1.0 - basalt);
    const float sediment = 1.0 - basalt - granite;
    const float sandstone = smoothstep(0.42, 0.5, b + (a - 0.5) * 0.3);
    return float3(granite, sediment * (1.0 - sandstone), sediment * sandstone);
}

// Profondeur de la terre (0 : roche à nu, 1 : sol épais) : selon l'humidité du relief (wet, de -1 sur les crêtes à 1
// au fond des vallons) et la roche (le calcaire et le grès font des sols minces, le basalte des sols riches).
float SoilDepth(float wet, float3 lithology)
{
    const float basalt = saturate(1.0 - lithology.x - lithology.y - lithology.z);
    return saturate(0.55 + 0.45 * wet - 0.2 * lithology.y - 0.1 * lithology.z + 0.1 * basalt);
}

// --- Biomes (Planetology::VegetationLibrary) ---

static const uint MaxBiomes = 8; // VegetationLibrary::MaxBiomes

// Doit correspondre à Planetology::BiomeGpu : où pousse un biome, et ses forêts vues de loin.
struct Biome
{
    float4 zones;    // x, y, z : part prise dans les plaines, les collines, les montagnes ; w : part couverte de forêts
    float4 ranges;   // x, y : altitude (fraction du relief, au-dessus du niveau zéro) ; z, w : température (°C)
    float4 humidity; // x, y : humidité ; z : densité des forêts ; w : taille des arbres (x celle de l'espèce)
    float4 color;    // rgb : couleur linéaire des cimes (moyenne de ses espèces) ; w : taille des bosquets d'une espèce (m)
};

// 1 entre low et high, 0 au-delà de soft de part et d'autre.
float BiomeBand(float x, float low, float high, float soft)
{
    return smoothstep(low - soft, low + soft, x) * (1.0 - smoothstep(high - soft, high + soft, x));
}

// Part du point dans les plaines, les collines et les montagnes (zones : Terrain::Zones, collines et montagnes).
float3 ZoneShares(float2 zones)
{
    return float3((1.0 - zones.x) * (1.0 - zones.y), zones.x * (1.0 - zones.y), zones.y);
}

// Convenance du biome au point (0 à 1) : zoneShares (ZoneShares), altitude (fraction du relief), climat (température,
// humidité, voir ClimateAt). Les biomes passent dans l'ordre de la planète : chacun prend sa convenance de ce que les
// précédents ont laissé (le premier qui convient prend tout), les plus particuliers d'abord. Là où aucun ne convient,
// rien ne pousse.
float BiomeFit(Biome biome, float3 zoneShares, float altitude, float2 climate)
{
    return saturate(dot(biome.zones.xyz, zoneShares)) * BiomeBand(altitude, biome.ranges.x, biome.ranges.y, 0.02)
         * BiomeBand(climate.x, biome.ranges.z, biome.ranges.w, 2.0) * BiomeBand(climate.y, biome.humidity.x, biome.humidity.y, 0.05);
}
