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
// - mesh shaders (GrassMesh.slang) : une tâche par tuile teste ses 64 carrés de 4 x 4 mailles (champ, pyramide de
//   profondeur de l'image précédente, présence d'herbe), chaque carré retenu envoie ses brins par groupes de 32, en 3,
//   2 ou 1 segments selon la distance ;
// - sommets (vertexGrass, GPU sans mesh shaders) : un appel indirect par page, chaque instance 32 brins de 2 segments,
//   le nombre d'instances d'une tuile choisi par le CPU selon sa distance ; moins dense et moins loin.
// Commandes (RHI::DrawIndexedCommand, celles des tuiles, MeshRenderer) : vertexOffset = case de la tuile dans sa page
// x 512 (GrassVertexShift), firstInstance = DrawData de la tuile x 4096 (GrassInstanceShift).
// Réglages (DrawParams) : color = densité (brins par m²), hauteur moyenne (m), portée (m), largeur (m) ; emissive.x =
// force du vent, emissive.y = part des brins fleuris.
// Fleurs : une part des brins (GrassFlowers) porte une tête colorée, élargie en losange au bout d'une tige fine, par
// taches de quelques mètres d'une même couleur (une espèce par tache), plus nombreuses au chaud et sur les plages vertes.

#include "MeshCulling.slang"

static const uint GrassTileQuads = 32; // TileQuads
static const uint GrassTileSide = GrassTileQuads + 1;
static const uint GrassTileVertices = GrassTileSide * GrassTileSide + 4 * GrassTileSide; // TileVertexCount
static const uint GrassVertexShift = 9;    // MeshRenderer::GrassVertexShift
static const uint GrassInstanceShift = 12; // MeshRenderer::GrassInstanceShift
// Jusqu'à cette part de la portée, tous les brins ; au-delà, leur part baisse comme 1 / distance.
static const float GrassNearBoost = 3.0;
static const float GrassFullDistance = 20.0; // m : tous les brins jusque-là (MeshRenderer.cpp)
// Chemin des sommets : brins de 2 segments, 32 par instance (MeshRenderer::GrassClusterBlades).
static const uint GrassClusterBlades = 32;
static const uint GrassVertexSegments = 2;

// Page de tuiles (PlanetTileCompute) : ModelVertex (8 floats : position, normale, uv), et MorphVertex (6 floats :
// glissement vers la forme parente, sa normale).
[[vk::binding(0, 3)]] StructuredBuffer<float> pageVertices;
[[vk::binding(1, 3)]] StructuredBuffer<float> pageMorph;

float GrassDensity() { return params.color.x; }
float GrassHeight() { return params.color.y; }
float GrassRange() { return params.color.z; }
float GrassWidth() { return params.color.w; }
float GrassWind() { return params.emissive.x; }
float GrassFlowers() { return params.emissive.y; }

// Couleurs des fleurs (albédo linéaire) : blanc, jaune, mauve, rouge, bleu, orange.
static const uint GrassFlowerColors = 6;
static const float3 GrassFlowerPalette[GrassFlowerColors] = {
    float3(0.72, 0.72, 0.66), float3(0.78, 0.55, 0.04), float3(0.38, 0.12, 0.52),
    float3(0.62, 0.05, 0.04), float3(0.14, 0.26, 0.70), float3(0.80, 0.30, 0.03)};
static const float GrassFlowerCell = 2.5; // m : taille d'une tache d'une même fleur
static const float GrassFlowerHead = 0.62; // hauteur relative où commence la tête

// Sommet `index` de la tuile glissé vers sa parente comme le sol le dessine (PlanetVertex) : l'herbe pousse aussi sur
// les tuiles qui glissent, sans flotter ni s'enfoncer.
float3 ReadGrassRestPosition(uint firstVertex, uint index)
{
    const uint v = (firstVertex + index) * 8;
    return float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
}

float3 ReadGrassPosition(uint draw, uint firstVertex, uint index)
{
    const uint m = (firstVertex + index) * 6;
    const float3 position = ReadGrassRestPosition(firstVertex, index);
    return position + float3(pageMorph[m], pageMorph[m + 1], pageMorph[m + 2]) * PlanetMorph(draw, position);
}

// Climat des grandes échelles au sommet `index` (PlanetClimate), avec l'humidité du relief de ce sommet (uv.y), et en z
// la profondeur de la terre (SoilDepth), comme le sol (PlanetVertex) : l'herbe pousse là où il la montre.
float3 GrassClimate(uint draw, uint firstVertex, uint index, float3 local)
{
    const DrawData d = draws[draw];
    const PlanetData planet = frame[0].planets[asuint(d.rim.w)];
    const uint v = (firstVertex + index) * 8;
    const float wet = pageVertices[v + 7];
    const float3 lithology = Lithology(normalize(d.rim.xyz + local) * planet.shape.x, UnpackZones(pageVertices[v + 6]).y);
    return float3(PlanetClimate(planet, d.rim.xyz + local) + float2(0.0, ReliefHumidity * wet), SoilDepth(wet, lithology));
}

float3 ReadGrassNormal(uint draw, uint firstVertex, uint index)
{
    const uint v = (firstVertex + index) * 8;
    const uint m = (firstVertex + index) * 6;
    const float3 position = float3(pageVertices[v], pageVertices[v + 1], pageVertices[v + 2]);
    return lerp(float3(pageVertices[v + 3], pageVertices[v + 4], pageVertices[v + 5]),
                float3(pageMorph[m + 3], pageMorph[m + 4], pageMorph[m + 5]), PlanetMorph(draw, position));
}

// Le sol de la tuile au point (s, t) de sa grille (en mailles, de 0 à GrassTileQuads), sur les deux triangles de sa
// maille tels que BuildTileIndices les découpe ((a, b, c) puis (a, c, d)) : le pied du brin est sur le sol dessiné.
void GrassGround(uint draw, uint firstVertex, float2 st, out float3 position, out float3 normal)
{
    const uint2 q = min(uint2(st), uint2(GrassTileQuads - 1));
    const float2 f = saturate(st - float2(q));
    const uint a = q.y * GrassTileSide + q.x;
    const uint b = a + 1;
    const uint c = a + GrassTileSide + 1;
    const uint d = a + GrassTileSide;
    const float3 pa = ReadGrassPosition(draw, firstVertex, a);
    const float3 pc = ReadGrassPosition(draw, firstVertex, c);
    const float3 na = ReadGrassNormal(draw, firstVertex, a);
    const float3 nc = ReadGrassNormal(draw, firstVertex, c);
    if (f.x >= f.y) {
        const float3 pb = ReadGrassPosition(draw, firstVertex, b);
        const float3 nb = ReadGrassNormal(draw, firstVertex, b);
        position = pa + f.x * (pb - pa) + f.y * (pc - pb);
        normal = na + f.x * (nb - na) + f.y * (nc - nb);
    } else {
        const float3 pd = ReadGrassPosition(draw, firstVertex, d);
        const float3 nd = ReadGrassNormal(draw, firstVertex, d);
        position = pa + f.x * (pc - pd) + f.y * (pd - pa);
        normal = na + f.x * (nc - nd) + f.y * (nd - na);
    }
    normal = normalize(normal);
}

// Aire (m²) du carré de la grille de `origin` à `origin` + size mailles (ses quatre coins), sans le glissement : le
// nombre de brins ne bouge pas avec la caméra.
float GrassArea(uint firstVertex, uint2 origin, uint size)
{
    const float3 p0 = ReadGrassRestPosition(firstVertex, origin.y * GrassTileSide + origin.x);
    const float3 p1 = ReadGrassRestPosition(firstVertex, origin.y * GrassTileSide + origin.x + size);
    const float3 p2 = ReadGrassRestPosition(firstVertex, (origin.y + size) * GrassTileSide + origin.x + size);
    const float3 p3 = ReadGrassRestPosition(firstVertex, (origin.y + size) * GrassTileSide + origin.x);
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
float GrassShareAt(uint draw, float3 position, float3 normal, float3 climate)
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
    float4 petals; // fleur : couleur de sa tête, 1 en a ; 0 : un brin d'herbe
    uint planet;
    float3 rim;    // contour d'une planète sans air (PlanetRim), comme le sol
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
    blade.petals = float4(0.0);
    blade.planet = 0;
    blade.rim = float3(0.0);
    return blade;
}

// Touffes : les brins vont par GrassTuftBlades rangs de suite autour d'un pied tiré au hasard dans le carré, à moins de
// GrassTuftRadius ; une part (GrassScattered) reste semée seule entre elles. Un pré, pas un gazon semé brin à brin.
// Rangs de suite : au loin, où il ne reste que les premiers rangs, ce sont des touffes entières qui partent.
static const uint GrassTuftBlades = 24;
static const float GrassTuftRadius = 0.15; // m
static const float GrassScattered = 0.25;

// Brin de rang `index` (sur `total` à pleine densité) d'un carré de la tuile : de `origin` à `origin` + size mailles.
// climate : celui des grandes échelles au centre du carré (GrassClimate : avec la profondeur de la terre), le même pour
// tous ses brins. L'air et l'ombre des nuages viennent de la passe d'éclairage (Lighting.slang), une fois par pixel.
GrassBlade MakeGrassBlade(uint draw, uint firstVertex, uint seed, uint index, float2 origin, float size, float total, float3 climate)
{
    GrassBlade blade = NoGrassBlade();
    const uint h0 = GroundHash(seed ^ GroundHash(index * 0x9E3779B9u + 0x6A09E667u));
    const uint h1 = GroundHash(h0 + 0x3C6EF372u);
    const uint h2 = GroundHash(h1 + 0x510E527Fu);
    const float2 r0 = GroundRandom(h0);
    const float2 r1 = GroundRandom(h1);
    const float2 r2 = GroundRandom(h2);
    const uint h3 = GroundHash(h2 + 0x9B05688Cu);
    const float2 r3 = GroundRandom(h3);
    const float2 r4 = GroundRandom(GroundHash(h3 + 0x2545F491u));

    float2 st = origin + r0 * size;
    float inTuft = 0.0; // 1 au pied de la touffe, 0 à son bord ou hors touffe
    if (r4.x >= GrassScattered) {
        const float2 center = GroundRandom(GroundHash(seed ^ GroundHash((index / GrassTuftBlades) * 0x85EBCA6Bu + 0xC2B2AE35u)));
        // Écart des sommets de la tuile (m), le même partout à 1 % près : le rayon de la touffe en mailles.
        const float quad = max(length(ReadGrassRestPosition(firstVertex, 1) - ReadGrassRestPosition(firstVertex, 0)), 1.0e-3);
        const float radius = sqrt(r4.y);
        const float angle = r0.y * 6.2831853;
        st = clamp(origin + center * size + float2(cos(angle), sin(angle)) * (radius * GrassTuftRadius / quad),
                   float2(0.0), float2(GrassTileQuads));
        inTuft = 1.0 - radius;
    }

    float3 position, normal;
    GrassGround(draw, firstVertex, st, position, normal);
    const DrawData d = draws[draw];
    blade.root = Transform(d.world, position).xyz;
    // Hors du champ : écarté avant le plus cher (couverture du sol, repère, vent). Le paquet entier l'était déjà quand
    // il sortait du champ ; à cheval sur un bord (sous les pieds en regardant le ciel), ses brins hors de l'image ne
    // coûtent plus que ce test. Sphère : le plus haut brin (1,35 x 1,25 x 1,2 = 2,03 fois GrassHeight), penché ou
    // couché par le vent, et sa largeur au loin.
    if (!InView(blade.root, 2.1 * GrassHeight() + 8.0 * GrassWidth())) {
        return blade;
    }
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
    // Plus hauts au cœur d'une touffe, plus courts et plus couchés à son bord.
    const float tuftShape = r4.x >= GrassScattered ? lerp(0.75, 1.25, inTuft) : 1.0;
    // Fleur ? Tache de GrassFlowerCell m : sa couleur et sa richesse (une sur trois sans fleurs, les autres plus ou
    // moins fleuries) ; rien au froid ni sur les plages sèches.
    const int3 flowerCell = int3(floor(ground / GrassFlowerCell));
    const uint flowerHash = GroundHash(uint(flowerCell.x) * 0x9E3779B1u ^ uint(flowerCell.y) * 0x85EBCA77u ^ uint(flowerCell.z) * 0xC2B2AE3Du);
    const float2 flowerPatch = GroundRandom(flowerHash);
    const float flowerRich = saturate(flowerPatch.x * 1.5 - 0.5) * 3.0 * smoothstep(2.0, 10.0, cover.temperature) * patch;
    const bool flower = GroundRandom(GroundHash(h3 ^ 0x68E31DA4u)).x < GrassFlowers() * flowerRich;
    const float height = GrassHeight() * lerp(0.55, 1.35, clump) * lerp(0.65, 1.25, patch) * lerp(0.5, 1.2, r1.y) * tuftShape * grow;
    // Toujours un peu ployé (vers `facing`, au hasard autour du brin) : le point de courbure haut et droit, la pointe
    // qui retombe en arc.
    // Une fleur se tient plus droite, un peu au-dessus de l'herbe.
    const float lean = lerp(0.2, 0.8, r2.y) * (r4.x >= GrassScattered ? lerp(1.3, 0.7, inTuft) : 1.0) * (flower ? 0.3 : 1.0);
    const float3 tipDirection = normalize(growUp * (1.0 - 0.5 * lean) + facing * lean + windDirection * push * 1.5);
    blade.root -= growUp * 0.03; // sous le sol : pas de jour entre le brin et un triangle du sol voisin
    const float stem = flower ? height * 1.15 : height;
    blade.tip = blade.root + tipDirection * stem;
    blade.bend = blade.root + growUp * (stem * 0.8);
    // Plus larges quand il en reste peu (au loin : le sol reste couvert), plus fins quand ils rapetissent.
    const float width = GrassWidth() * lerp(0.7, 1.3, r0.x) * sqrt(grow) / sqrt(clamp(keep, 0.02, 1.0));
    blade.side = side * (0.5 * width);
    if (flower) {
        const uint kind = min(uint(flowerPatch.y * float(GrassFlowerColors)), GrassFlowerColors - 1);
        // Un peu de variété d'une fleur à l'autre de la même tache.
        blade.petals = float4(GrassFlowerPalette[kind] * lerp(0.8, 1.1, r3.x), 1.0);
    }
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
    // Lumière de contour comme le sol (PlanetGBuffer) : sinon le pré s'arrête sur une bande plus claire à sa portée.
    blade.rim = PlanetRim(planet, worldUp, normalize(-blade.root));
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
    // rgb : teinte du brin (GrassBlade::tint), ou couleur des pétales d'une fleur ; a : hauteur relative, + 2 pour une
    // fleur (pas de sortie de plus : les mesh shaders gardent leurs sommets petits).
    float4 tintHeight : TINT;
    nointerpolation float3 rim : RIM;
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
    // Feuille : large, puis effilée au bout. Fleur : tige fine, puis la tête en losange (la pointe la ferme).
    const float width = blade.petals.a > 0.0 ? lerp(0.35, 2.2, smoothstep(0.4, GrassFlowerHead + 0.05, t)) : 1.0 - t * t;
    const float3 position = center + blade.side * (across * width);
    GrassVertexOutput output;
    output.position = Transform(frame[0].viewProjection, position);
    output.worldPosition = position;
    const float3 sideDirection = normalize(blade.side);
    output.normal = normalize(blade.facing + sideDirection * (across * 0.5));
    output.tintHeight = blade.petals.a > 0.0 ? float4(blade.petals.rgb, t + 2.0) : float4(blade.tint, t);
    output.rim = blade.rim;
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
    const float3 climate = GrassClimate(draw, firstVertex, (GrassTileSide * GrassTileSide) / 2, float3(0.0));
    const float total = ceil(GrassArea(firstVertex, uint2(0, 0), GrassTileQuads) * GrassDensity());
    const uint index = cluster * GrassClusterBlades + inCluster / verticesPerBlade;
    const GrassBlade blade = MakeGrassBlade(draw, firstVertex, GrassSeed(draw), index, float2(0.0), float(GrassTileQuads), total, climate);
    if (!blade.alive) {
        // Les trois sommets au même point : rien à dessiner.
        GrassVertexOutput empty;
        empty.position = float4(0.0, 0.0, 0.0, 1.0);
        empty.worldPosition = float3(0.0);
        empty.normal = float3(0.0, 1.0, 0.0);
        empty.tintHeight = float4(0.0);
        empty.rim = float3(0.0);
        empty.planet = 0;
        return empty;
    }
    return GrassVertex(blade, inCluster % verticesPerBlade, GrassVertexSegments);
}

// Mesure (« Sol et herbe sans pixels ») : brins fabriqués comme d'habitude, pixel presque vide et sans éclairage
// (sorte de matière 0) ; la différence avec fragmentGrass est le coût de leurs pixels et de leur éclairage.
[shader("fragment")]
GBufferOutput fragmentGrassGeometry(GrassVertexOutput input)
{
    const float unused = dot(input.worldPosition + input.normal + input.tintHeight.rgb + input.rim, float3(1.0))
                       + input.tintHeight.a + float(input.planet);
    return GBufferUnlit(float3(0.2, 0.35, 0.1) * (0.3 + 0.7 * input.tintHeight.a) + unused * 1.0e-30);
}

// Brin dans le G-buffer (GBufferGrass), éclairé ensuite comme le feuillage : la lumière du soleil déborde sur la face
// tournée de l'autre côté, et traverse le brin vu à contre-jour (leaves 0,4 et 0,35). Couleur : la moyenne de l'image
// de l'herbe du sol (ou sa couleur unie), qu'il prolonge. Immobile (le vent est trop fin pour l'anticrénelage).
[shader("fragment")]
GBufferOutput fragmentGrass(GrassVertexOutput input)
{
    const PlanetData planet = frame[0].planets[input.planet];
    const float3 grass = GrassBaseColor(planet);
    float3 normal = normalize(input.normal);
    normal = dot(normal, -input.worldPosition) < 0.0 ? -normal : normal;
    // Pied sombre (les voisins lui cachent ciel et soleil), pointe claire et un peu sèche : en moyenne
    // GrassMeadowShade, la couleur que le sol prend sous l'herbe (PlanetGBuffer).
    // Ciel compté moins fort que sur le sol (occlusion 0,85 en haut) : sinon, à l'ombre, les brins bleuissent.
    const bool flower = input.tintHeight.a >= 2.0;
    const float height = flower ? input.tintHeight.a - 2.0 : input.tintHeight.a;
    const float occlusion = lerp(0.15, 0.85, height);
    // Tige d'une fleur : l'herbe sans teinte propre.
    const float3 tint = flower ? float3(0.85) : input.tintHeight.rgb;
    float3 diffuse = grass * tint * lerp(0.2, 1.0, height * (2.0 - height))
                   * lerp(float3(1.0), float3(1.2, 1.15, 0.85), height * height);
    // Tête d'une fleur : ses pétales, au-dessus des brins voisins (presque pas d'ombre d'eux).
    if (flower && height > GrassFlowerHead) {
        diffuse = input.tintHeight.rgb;
    }
    return GBufferResult(input.rim, float2(0.0), diffuse, GBufferGrass, normal, normal, float4(0.4, 0.6, occlusion, 0.35));
}
