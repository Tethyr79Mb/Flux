// Herbe des planètes (VegetationComponent : grassDensity et suivants) : des brins générés par le GPU à chaque image sur
// les tuiles du sol proches de la caméra. Rien n'est gardé en mémoire ni préparé par le CPU, qui ne choisit que les
// tuiles (MeshRenderer::BuildTileCommands).
// Chaque brin est une courbe de Bézier (pied, point de courbure, pointe) en ruban qui s'affine vers la pointe, posé sur
// les triangles du sol (mêmes sommets que fragmentPlanet) là où le sol montre de l'herbe (GrassShare), à une place tirée
// au hasard de son rang dans sa tuile : la même à chaque image. Au loin, il en reste une part (GrassKeep), les premiers
// rangs ; ceux qui vont disparaître rapetissent avant (pas d'apparition brusque) et ceux qui restent s'élargissent
// pour couvrir autant de sol. Le vent passe par vagues dans le pré, plus un frémissement propre à chaque brin.
// L'herbe reçoit les ombres et la lumière comme le feuillage (ShadeFoliage : lumière qui traverse les brins), plus
// sombre à leur pied ; elle n'en porte pas (les cartes d'ombre n'en verraient presque rien, pour un coût élevé).
// Deux chemins :
// - mesh shaders (GrassMesh.slang) : une tâche par tuile teste ses 16 paquets (champ, pyramide de profondeur de
//   l'image précédente, présence d'herbe), chaque paquet retenu envoie ses brins par groupes de 32, en 3, 2 ou 1
//   segments selon la distance ;
// - sommets (vertexGrass, GPU sans mesh shaders) : un appel indirect par page, chaque instance 32 brins de 2 segments,
//   le nombre d'instances d'une tuile choisi par le CPU selon sa distance ; moins dense et moins loin.
// Commandes (RHI::DrawIndexedCommand, celles des tuiles, MeshRenderer) : vertexOffset = case de la tuile dans sa page
// x 512 (GrassVertexShift), firstInstance = DrawData de la tuile x 4096 (GrassInstanceShift).
// Réglages (DrawParams) : color = densité (brins par m²), hauteur moyenne (m), portée (m), largeur (m) ; emissive.x =
// force du vent.

#include "MeshCulling.slang"

static const uint GrassTileQuads = 32; // TileQuads
static const uint GrassTileSide = GrassTileQuads + 1;
static const uint GrassTileVertices = GrassTileSide * GrassTileSide + 4 * GrassTileSide; // TileVertexCount
static const uint GrassMeshletQuads = 8; // TileMeshletQuads
static const uint GrassMeshletSide = GrassTileQuads / GrassMeshletQuads;
static const uint GrassMeshletCount = GrassMeshletSide * GrassMeshletSide;
static const uint GrassVertexShift = 9;    // MeshRenderer::GrassVertexShift
static const uint GrassInstanceShift = 12; // MeshRenderer::GrassInstanceShift
// Jusqu'à cette part de la portée, tous les brins ; au-delà, leur part baisse comme 1 / distance.
static const float GrassNearBoost = 3.0;
static const float GrassFullDistance = 20.0; // m : tous les brins jusque-là (MeshRenderer.cpp)
// Chemin des sommets : brins de 2 segments, 32 par instance (MeshRenderer::GrassClusterBlades).
static const uint GrassClusterBlades = 32;
static const uint GrassVertexSegments = 2;

// Doit correspondre à Planetology::PlanetTileCompute::Meshlet (comme dans PlanetMesh.slang).
struct GrassMeshlet
{
    float4 low;
    float4 high;
};

// Page de tuiles (PlanetTileCompute) : ModelVertex (8 floats : position, normale, uv), boîtes des paquets.
[[vk::binding(0, 3)]] StructuredBuffer<float> pageVertices;
[[vk::binding(2, 3)]] StructuredBuffer<GrassMeshlet> pageMeshlets;

float GrassDensity() { return params.color.x; }
float GrassHeight() { return params.color.y; }
float GrassRange() { return params.color.z; }
float GrassWidth() { return params.color.w; }
float GrassWind() { return params.emissive.x; }

float3 ReadGrassPosition(uint firstVertex, uint index)
{
    const uint v = (firstVertex + index) * 8;
    return float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
}

float3 ReadGrassNormal(uint firstVertex, uint index)
{
    const uint v = (firstVertex + index) * 8 + 3;
    return float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
}

// Le sol de la tuile au point (s, t) de sa grille (en mailles, de 0 à GrassTileQuads), sur les deux triangles de sa
// maille tels que BuildTileIndices les découpe ((a, b, c) puis (a, c, d)) : le pied du brin est sur le sol dessiné.
void GrassGround(uint firstVertex, float2 st, out float3 position, out float3 normal)
{
    const uint2 q = min(uint2(st), uint2(GrassTileQuads - 1));
    const float2 f = saturate(st - float2(q));
    const uint a = q.y * GrassTileSide + q.x;
    const uint b = a + 1;
    const uint c = a + GrassTileSide + 1;
    const uint d = a + GrassTileSide;
    const float3 pa = ReadGrassPosition(firstVertex, a);
    const float3 pc = ReadGrassPosition(firstVertex, c);
    const float3 na = ReadGrassNormal(firstVertex, a);
    const float3 nc = ReadGrassNormal(firstVertex, c);
    if (f.x >= f.y) {
        const float3 pb = ReadGrassPosition(firstVertex, b);
        const float3 nb = ReadGrassNormal(firstVertex, b);
        position = pa + f.x * (pb - pa) + f.y * (pc - pb);
        normal = na + f.x * (nb - na) + f.y * (nc - nb);
    } else {
        const float3 pd = ReadGrassPosition(firstVertex, d);
        const float3 nd = ReadGrassNormal(firstVertex, d);
        position = pa + f.x * (pc - pd) + f.y * (pd - pa);
        normal = na + f.x * (nc - nd) + f.y * (nd - na);
    }
    normal = normalize(normal);
}

// Aire (m²) du carré de la grille de `origin` à `origin` + size mailles (ses quatre coins).
float GrassArea(uint firstVertex, uint2 origin, uint size)
{
    const float3 p0 = ReadGrassPosition(firstVertex, origin.y * GrassTileSide + origin.x);
    const float3 p1 = ReadGrassPosition(firstVertex, origin.y * GrassTileSide + origin.x + size);
    const float3 p2 = ReadGrassPosition(firstVertex, (origin.y + size) * GrassTileSide + origin.x + size);
    const float3 p3 = ReadGrassPosition(firstVertex, (origin.y + size) * GrassTileSide + origin.x);
    return 0.5 * length(cross(p2 - p0, p3 - p1));
}

// Part des brins gardés à cette distance de la caméra (au-delà de 1 de près) : tous jusqu'à GrassFullDistance, puis en distance^-1,5 (des brins
// plus larges couvrent autant de sol : le coût croît comme la racine de la portée, pas comme sa surface), et plus aucun
// à la portée, en rapetissant depuis 60 % de celle-ci : pas de limite nette.
float GrassKeep(float distance)
{
    const float range = GrassRange();
    // De près, sous les yeux quand on regarde le sol : trois fois plus de brins (peu de surface, peu de coût).
    const float near = lerp(GrassNearBoost, 1.0, smoothstep(2.0, 10.0, distance));
    return min(pow(GrassFullDistance / max(distance, 1.0e-3), 1.5), 1.0) * near * smoothstep(range, 0.6 * range, distance);
}

// Part des rangs dessinés pour une part gardée `keep` : un peu plus, ceux qui rapetissent avant de partir.
float GrassKept(float keep)
{
    return keep < 1.0 ? min(keep * 1.25, 1.0) : keep;
}

// Graine d'une tuile : son centre dans le repère de la planète (calculé en double puis arrondi, les mêmes bits à chaque
// image) ; deux tuiles différentes n'ont pas le même.
uint GrassSeed(uint draw)
{
    const float3 center = draws[draw].rim.xyz;
    return GroundHash(asuint(center.x) ^ GroundHash(asuint(center.y) ^ GroundHash(asuint(center.z))));
}

// Le sol montre de l'herbe en ce point de la tuile (part de 0 à 1, comme fragmentPlanet).
float GrassShareAt(uint draw, float3 position, float3 normal, float2 climate)
{
    const DrawData d = draws[draw];
    const PlanetData planet = frame[0].planets[asuint(d.rim.w)];
    const float3 local = d.rim.xyz + position;
    const float3 up = normalize(local);
    const float distance = length(Transform(d.world, position).xyz);
    const GroundCover cover = GetGroundCover(planet, local, dot(normal, up), length(local) - planet.shape.x, climate, distance);
    return GrassShare(cover);
}

struct GrassBlade
{
    bool alive;
    float3 root;   // relatif à la caméra
    float3 bend;   // point de courbure (Bézier)
    float3 tip;
    float3 side;   // demi-largeur au pied, dans le monde
    float3 facing; // normale du ruban, tournée vers la caméra
    float3 tint;   // multiplie la couleur de l'herbe du sol (fragmentGrass)
    uint planet;
    float3 airTransmittance;
    float3 airScattered;
    float clouds;
};

// Brin écarté : rien à dessiner.
GrassBlade NoGrassBlade()
{
    GrassBlade blade;
    blade.alive = false;
    blade.root = float3(0.0);
    blade.bend = float3(0.0);
    blade.tip = float3(0.0);
    blade.side = float3(0.0);
    blade.facing = float3(0.0, 1.0, 0.0);
    blade.tint = float3(0.0);
    blade.planet = 0;
    blade.airTransmittance = float3(0.0);
    blade.airScattered = float3(0.0);
    blade.clouds = 0.0;
    return blade;
}

// Air entre la caméra et un carré de brins, et ombre des nuages : calculés une fois au centre du carré (la tâche, ou la
// tuile sur le chemin des sommets), pas par brin. L'un et l'autre changent sur des centaines de mètres, le carré en
// fait une dizaine ; par brin, c'était une bonne part du coût de leur fabrication (lecture de la table de l'air, bruit
// de la météo des nuages).
struct GrassPatchLight
{
    float4 air;       // rgb : transmittance de l'air ; a : ombre des nuages (CloudShadow)
    float4 scattered; // rgb : lumière ajoutée par l'air (AirTo)
};

GrassPatchLight LightGrassPatch(float3 center)
{
    GrassPatchLight light;
    float3 transmittance;
    light.scattered = float4(AirTo(center, transmittance), 0.0);
    light.air = float4(transmittance, CloudShadow(center));
    return light;
}

// Brin de rang `index` (sur `total` à pleine densité) d'un carré de la tuile : de `origin` à `origin` + size mailles.
// climate : celui des grandes échelles au centre du carré (PlanetClimate), le même pour tous ses brins ; light : son
// air et ses nuages (LightGrassPatch).
GrassBlade MakeGrassBlade(uint draw, uint firstVertex, uint seed, uint index, float2 origin, float size, float total, float2 climate,
                          GrassPatchLight light)
{
    GrassBlade blade = NoGrassBlade();
    const uint h0 = GroundHash(seed ^ GroundHash(index * 0x9E3779B9u + 0x6A09E667u));
    const uint h1 = GroundHash(h0 + 0x3C6EF372u);
    const uint h2 = GroundHash(h1 + 0x510E527Fu);
    const float2 r0 = GroundRandom(h0);
    const float2 r1 = GroundRandom(h1);
    const float2 r2 = GroundRandom(h2);
    const float2 r3 = GroundRandom(GroundHash(h2 + 0x9B05688Cu));

    float3 position, normal;
    GrassGround(firstVertex, origin + r0 * size, position, normal);
    const DrawData d = draws[draw];
    blade.root = Transform(d.world, position).xyz;
    const float distance = length(blade.root);
    // Gardé s'il est dans les premiers rangs (GrassKept) ; les derniers gardés rapetissent (ils seront les prochains à
    // partir), sur un cinquième des gardés : tous à leur taille quand keep vaut 1.
    const float keep = GrassKeep(distance);
    const float rank = (float(index) + 0.5) / max(total, 1.0);
    float grow = saturate((GrassKept(keep) - rank) / max(0.25 * keep, 1.0e-4));
    if (grow <= 0.0) {
        return blade;
    }
    blade.planet = asuint(d.rim.w);
    const PlanetData planet = frame[0].planets[blade.planet];
    const float3 local = d.rim.xyz + position;
    if (planet.water.w > 0.0 && length(local) < planet.water.w) {
        return blade; // sous l'eau
    }
    const float3 up = normalize(local);
    const GroundCover cover = GetGroundCover(planet, local, dot(normal, up), length(local) - planet.shape.x, climate, distance);
    // Autant de brins que d'herbe sur le sol ; plus courts au bord d'une zone d'herbe.
    grow *= saturate((GrassShare(cover) - r1.x * 0.9) * 4.0);
    if (grow < 0.05) {
        return blade;
    }

    // Repère du brin dans le monde (colonnes de la rotation de la planète) : il pousse surtout vers le ciel, un peu
    // selon la pente.
    const float3 worldUp = normalize(d.normal[0].xyz * up.x + d.normal[1].xyz * up.y + d.normal[2].xyz * up.z);
    const float3 worldNormal = normalize(d.normal[0].xyz * normal.x + d.normal[1].xyz * normal.y + d.normal[2].xyz * normal.z);
    const float3 growUp = normalize(lerp(worldNormal, worldUp, 0.6));
    const float3 helper = abs(growUp.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t1 = normalize(cross(growUp, helper));
    const float3 t2 = cross(growUp, t1);
    const float angle = r2.x * 6.2831853;
    const float3 side = t1 * cos(angle) + t2 * sin(angle);
    float3 facing = cross(side, growUp);

    // Vent : deux vagues qui traversent le pré (coordonnées du sol modulo GroundPeriod : nombres d'ondes entiers sur la
    // période, pas de saut d'une période à l'autre), et le frémissement de chaque brin. Il souffle selon l'axe x de la
    // planète, ramené sur le sol.
    const float3 ground = d.ground.xyz + position;
    const float time = frame[0].cameraForward.w;
    const float k = 6.2831853 / GroundPeriod;
    const float wave = sin(k * (37.0 * ground.x + 23.0 * ground.z + 11.0 * ground.y) - time * 1.3) * 0.5 + 0.5;
    const float gust = sin(k * (9.0 * ground.x - 6.0 * ground.z + 4.0 * ground.y) - time * 0.35) * 0.5 + 0.5;
    const float flutter = sin(time * 4.0 + r2.x * 40.0) * 0.08;
    const float3 planetX = d.normal[0].xyz;
    const float3 windDirection = normalize(planetX - growUp * dot(planetX, growUp) + 1.0e-5);
    const float push = GrassWind() * (0.2 + 0.8 * wave * gust + flutter);

    // Touffes : la hauteur suit une cellule d'environ 0,6 m (hachage de la cellule du sol), puis chaque brin : un pré
    // irrégulier, pas une brosse de poils tous pareils.
    const int3 cell = int3(floor(ground / 0.6));
    const float clump = GroundRandom(GroundHash(uint(cell.x) * 0x8DA6B343u ^ uint(cell.y) * 0xD8163841u
                                                ^ uint(cell.z) * 0xCB1AB31Fu)).x;
    // Plages de quelques mètres plus hautes ou plus rases, plus sèches (nombres d'ondes entiers sur GroundPeriod) :
    // le pré n'est pas une moquette égale au-delà de quelques pas.
    const float patch = saturate(0.5 + 0.5 * sin(k * (211.0 * ground.x + 137.0 * ground.z + 59.0 * ground.y))
                                     * sin(k * (-113.0 * ground.x + 181.0 * ground.z + 97.0 * ground.y))
                                 + 0.25 * sin(k * (83.0 * ground.x - 61.0 * ground.z + 151.0 * ground.y)));
    const float height = GrassHeight() * lerp(0.55, 1.35, clump) * lerp(0.65, 1.25, patch) * lerp(0.5, 1.2, r1.y) * grow;
    // Toujours un peu ployé (vers `facing`, au hasard autour du brin) : le point de courbure haut et droit, la pointe
    // qui retombe en arc.
    const float lean = lerp(0.2, 0.8, r2.y);
    const float3 tipDirection = normalize(growUp * (1.0 - 0.5 * lean) + facing * lean + windDirection * push * 1.5);
    blade.root -= growUp * 0.03; // sous le sol : pas de jour entre le brin et un triangle du sol voisin
    blade.tip = blade.root + tipDirection * height;
    blade.bend = blade.root + growUp * (height * 0.8);
    // Plus larges quand il en reste peu (au loin : le sol reste couvert), plus fins quand ils rapetissent.
    const float width = GrassWidth() * lerp(0.7, 1.3, r0.x) * sqrt(grow) / sqrt(clamp(keep, 0.02, 1.0));
    blade.side = side * (0.5 * width);
    facing = dot(facing, -blade.root) < 0.0 ? -facing : facing;
    // Normale penchée vers le ciel : les brins s'éclairent presque comme le sol sous eux. Sans cela, debout face à un
    // soleil bas, ils sortent bien plus sombres que le sol et chaque brin se détache comme un poil.
    blade.facing = normalize(facing + growUp * 1.2);

    // Couleur : celle de l'herbe du sol (lue par fragmentGrass), jaunie dans le froid et par taches sèches comme
    // fragmentPlanet, plus claire ou plus foncée selon les taches, et un peu différente d'un brin à l'autre.
    // Par brin : quelques-uns secs (r3.x), une teinte qui hésite entre vert bleuté et vert jaune (r3.y), et les
    // plages rases un peu plus sèches.
    const float dry = max(pow(r3.x, 6.0) * 0.8, (1.0 - patch) * 0.5);
    blade.tint = GrassDryTint(cover.temperature, cover.macro, dry) * (0.9 + cover.macro * 0.4) * lerp(0.75, 1.15, patch)
               * lerp(float3(0.85, 1.0, 0.95), float3(1.15, 1.05, 0.8), r3.y) * lerp(0.75, 1.2, r1.y);
    // Lumière de contour comme le sol (PlanetColor), avec l'air : sinon le pré s'arrête sur une bande plus claire à sa
    // portée.
    blade.airTransmittance = light.air.rgb;
    blade.airScattered = light.scattered.rgb + PlanetRim(planet, worldUp, normalize(-blade.root));
    blade.clouds = light.air.a;
    blade.alive = true;
    return blade;
}

// Le brin vu au pixel : position, normale (arrondie sur la largeur, comme une feuille pliée), hauteur relative (0 au
// pied) pour l'ombre de ses voisins.
struct GrassVertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION; // relatif à la caméra
    float3 normal : NORMAL;
    float4 tintHeight : TINT;                // rgb : teinte du brin (GrassBlade::tint) ; a : hauteur relative
    float4 airTransmittance : AIR_TRANSMITTANCE; // rgb ; a : ombre des nuages (CloudShadow)
    float3 airScattered : AIR_SCATTERED;
    nointerpolation uint planet : PLANET;
};

// Sommet k du brin en `segments` segments : deux par étage (gauche, droite) du pied vers la pointe, puis la pointe.
GrassVertexOutput GrassVertex(GrassBlade blade, uint k, uint segments)
{
    const bool tip = k >= 2 * segments;
    const float t = tip ? 1.0 : float(k / 2) / float(segments);
    const float across = tip ? 0.0 : ((k & 1u) == 0 ? -1.0 : 1.0);
    const float u = 1.0 - t;
    const float3 center = u * u * blade.root + 2.0 * t * u * blade.bend + t * t * blade.tip;
    const float3 position = center + blade.side * (across * (1.0 - t * t)); // feuille : large, puis effilée au bout
    GrassVertexOutput output;
    output.position = Transform(frame[0].viewProjection, position);
    output.worldPosition = position;
    const float3 sideDirection = normalize(blade.side);
    output.normal = normalize(blade.facing + sideDirection * (across * 0.5));
    output.tintHeight = float4(blade.tint, t);
    output.airTransmittance = float4(blade.airTransmittance, blade.clouds);
    output.airScattered = blade.airScattered;
    output.planet = blade.planet;
    return output;
}

// Triangle j du brin en `segments` segments (indices de ses sommets, comme GrassVertex).
uint3 GrassTriangle(uint j, uint segments)
{
    const uint level = j / 2;
    const uint a = 2 * level;
    if (level + 1 == segments) {
        return uint3(a, a + 1, a + 2); // dernier étage : la pointe
    }
    return (j & 1u) == 0 ? uint3(a, a + 1, a + 3) : uint3(a, a + 3, a + 2);
}

// Chemin des sommets : instance = un groupe de GrassClusterBlades brins d'une tuile. Les brins écartés (au loin, sans
// herbe) donnent des triangles vides.
[shader("vertex")]
GrassVertexOutput vertexGrass(uint vertex : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
    const uint verticesPerBlade = 2 * GrassVertexSegments + 1;
    const uint tileInPage = vertex >> GrassVertexShift;
    const uint inCluster = vertex & ((1u << GrassVertexShift) - 1u);
    const uint draw = instance >> GrassInstanceShift;
    const uint cluster = instance & ((1u << GrassInstanceShift) - 1u);
    const uint firstVertex = tileInPage * GrassTileVertices;
    const DrawData d = draws[draw];
    const float2 climate = PlanetClimate(frame[0].planets[asuint(d.rim.w)], d.rim.xyz);
    const float total = ceil(GrassArea(firstVertex, uint2(0, 0), GrassTileQuads) * GrassDensity());
    const uint index = cluster * GrassClusterBlades + inCluster / verticesPerBlade;
    // Air et nuages au milieu de la tuile (chemin de repli : recalculés par sommet, mais une fois et non par brin).
    const float3 middle = Transform(d.world, ReadGrassPosition(firstVertex, (GrassTileQuads / 2) * GrassTileSide + GrassTileQuads / 2)).xyz;
    const GrassBlade blade = MakeGrassBlade(draw, firstVertex, GrassSeed(draw), index, float2(0.0), float(GrassTileQuads), total, climate,
                                            LightGrassPatch(middle));
    if (!blade.alive) {
        // Les trois sommets au même point : rien à dessiner.
        GrassVertexOutput empty;
        empty.position = float4(0.0, 0.0, 0.0, 1.0);
        empty.worldPosition = float3(0.0);
        empty.normal = float3(0.0, 1.0, 0.0);
        empty.tintHeight = float4(0.0);
        empty.airTransmittance = float4(0.0);
        empty.airScattered = float3(0.0);
        empty.planet = 0;
        return empty;
    }
    return GrassVertex(blade, inCluster % verticesPerBlade, GrassVertexSegments);
}

// Mesure (« Sol et herbe sans pixels ») : brins fabriqués comme d'habitude, pixel presque vide ; la différence avec
// fragmentGrass est le coût de l'éclairage des brins.
[shader("fragment")]
SceneOutput fragmentGrassGeometry(GrassVertexOutput input)
{
    const float unused = dot(input.worldPosition + input.normal + input.tintHeight.rgb + input.airTransmittance.rgb
                             + input.airScattered, float3(1.0))
                       + input.tintHeight.a + input.airTransmittance.a + float(input.planet);
    return SceneResult(float4(float3(0.2, 0.35, 0.1) * (0.3 + 0.7 * input.tintHeight.a) + unused * 1.0e-30, 1.0), float2(0.0));
}

// Brin éclairé comme le feuillage : la lumière du soleil déborde sur la face tournée de l'autre côté, et traverse le brin
// vu à contre-jour. Couleur : la moyenne de l'image de l'herbe du sol (ou sa couleur unie), qu'il prolonge.
[shader("fragment")]
SceneOutput fragmentGrass(GrassVertexOutput input)
{
    shadowPixel = input.position.xy;
    simpleShadow = true;
    const PlanetData planet = frame[0].planets[input.planet];
    const float3 grass = GrassBaseColor(planet);
    Surface surface;
    surface.toEye = normalize(-input.worldPosition);
    float3 normal = normalize(input.normal);
    normal = dot(normal, surface.toEye) < 0.0 ? -normal : normal;
    surface.normal = normal;
    surface.f0 = float3(0.04);
    surface.roughness = 0.6;
    // Pied plus sombre : les brins voisins lui cachent le ciel et une part du soleil.
    const float height = input.tintHeight.a;
    // Pied sombre (les voisins lui cachent ciel et soleil), pointe claire et un peu sèche : en moyenne
    // GrassMeadowShade, la couleur que le sol prend sous l'herbe (PlanetColor).
    // Ciel compté moins fort que sur le sol (occlusion 0,85 en haut) : sinon, à l'ombre, les brins bleuissent.
    const float occlusion = lerp(0.15, 0.85, height);
    surface.diffuse = grass * input.tintHeight.rgb * lerp(0.2, 1.0, height * (2.0 - height)) * lerp(float3(1.0), float3(1.2, 1.15, 0.85), height * height);
    float inside;
    const float3 lit = LightSurface(surface, normal, input.worldPosition, occlusion, input.airTransmittance.a, inside, true,
                                    float2(0.4, 0.35));
    sunRadiance = Radiance(sunDirect * input.airTransmittance.rgb);
    return SceneResult(float4(Radiance(lit * input.airTransmittance.rgb + input.airScattered), 1.0), float2(0.0));
}
